#include "e3sm_spectrum.h"
#include "e3sm/e3sm_spect_wrapper.h"
#include "e3sm/bfp_decompress.h"
#include <chrono>
#include <fstream>
#include <cerrno>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

bool E3SMSpectrum::load_codelets()
{
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, lcm_socket_path_.c_str(), JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_load_req_s load_req = {};

    // Codeletset identity
    std::strncpy(load_req.codeletset_id.name, "ecpri_iq_samples", JBPF_CODELETSET_NAME_LEN - 1);

    // Single codelet: ecpri_iq_collect on the capture_xran_packet hook
    load_req.num_codelet_descriptors = 1;
    auto& desc = load_req.codelet_descriptor[0];

    std::strncpy(desc.codelet_name, "collector", JBPF_CODELET_NAME_LEN - 1);
    std::strncpy(desc.hook_name, "capture_xran_packet", sizeof(desc.hook_name) - 1);

    std::string full_path = codelet_base_path_ + "/ecpri_iq_samples/ecpri_iq_collect.o";
    std::strncpy(desc.codelet_path, full_path.c_str(), JBPF_PATH_LEN - 1);

    desc.priority = 1;
    desc.runtime_threshold = 0;
    desc.num_in_io_channel = 1;
    std::strncpy(desc.in_io_channel[0].name, "prb_config_map", sizeof(desc.in_io_channel[0].name) - 1);
    std::memcpy(&desc.in_io_channel[0].stream_id, &prb_config_stream_id_, sizeof(jbpf_io_stream_id_t));
    desc.in_io_channel[0].has_serde = false;

    desc.num_linked_maps = 0;

    // Output channel: stream_id must match ecpri_iq_stream_id_ so the dispatcher routes to us
    desc.num_out_io_channel = 1;
    std::strncpy(desc.out_io_channel[0].name, "output_map", sizeof(desc.out_io_channel[0].name) - 1);
    std::memcpy(&desc.out_io_channel[0].stream_id, &ecpri_iq_stream_id_, sizeof(jbpf_io_stream_id_t));
    desc.out_io_channel[0].has_serde = false;

    int rc = jbpf_lcm_ipc_send_codeletset_load_req(&address, &load_req);
    if (rc != 0) {
        std::fprintf(stderr, "[E3SMSpectrum] Failed to load codeletset 'ecpri_iq_samples' via LCM IPC (rc=%d)\n", rc);
        return false;
    }
    std::printf("[E3SMSpectrum] Codeletset 'ecpri_iq_samples' loaded successfully\n");
    return true;
}

void E3SMSpectrum::unload_codelets()
{
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, lcm_socket_path_.c_str(), JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_unload_req_s unload_req = {};
    std::strncpy(unload_req.codeletset_id.name, "ecpri_iq_samples", JBPF_CODELETSET_NAME_LEN - 1);

    int rc = jbpf_lcm_ipc_send_codeletset_unload_req(&address, &unload_req);
    if (rc != 0) {
        std::fprintf(stderr, "[E3SMSpectrum] Failed to unload codeletset 'ecpri_iq_samples' (rc=%d)\n", rc);
    } else {
        std::printf("[E3SMSpectrum] Codeletset 'ecpri_iq_samples' unloaded successfully\n");
    }
}

libe3::ErrorCode E3SMSpectrum::start()
{
    if (running_) {
        return libe3::ErrorCode::SUCCESS;
    }

    // Pre-allocate worker-side buffers so the steady-state path never reallocates.
    // Sized for the largest plausible num_prbu (273 = full 100 MHz BWP at 30 kHz SCS):
    //   ofdm_sym_size = 273*12 = 3276 subcarriers, fft_size = next_pow2(3276) = 4096.
    constexpr uint32_t kMaxNumPrbu = 273;
    constexpr uint32_t kMaxOfdmSymSize = kMaxNumPrbu * 12;            // 3276 active subcarriers
    constexpr uint32_t kMaxFftSize = 4096;                            // next_pow2(3276)
    decompressed_buf_.reserve(kMaxOfdmSymSize * 2);                   // I+Q int16 pairs
    padded_buf_.reserve(kMaxFftSize * 2);                             // I+Q int16 pairs at fft_size
    encoded_buf_.reserve(64 * 1024);                                  // APER output upper bound
    indication_.iq_data.reserve(kMaxFftSize * 2 * sizeof(int16_t));   // bytes

    // Load codelets into srsRAN if a codelet path was configured
    if (!codelet_base_path_.empty()) {
        if (!load_codelets()) {
            return libe3::ErrorCode::INTERNAL_ERROR;
        }
        codelets_loaded_ = true;
    }

    // Mark running BEFORE spawning the worker so its first iteration sees the flag.
    running_.store(true, std::memory_order_release);

    // Spawn worker thread and pin it to the dedicated core (if requested).
    worker_ = std::thread([this] { worker_loop(); });
    if (worker_core_ >= 0) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(worker_core_, &mask);
        if (pthread_setaffinity_np(worker_.native_handle(), sizeof(mask), &mask) != 0) {
            std::fprintf(stderr,
                "[E3SMSpectrum] WARNING: failed to pin worker thread to core %d (%s)\n",
                worker_core_, std::strerror(errno));
        } else {
            std::printf("[E3SMSpectrum] Worker thread pinned to core %d\n", worker_core_);
        }
    }

    // Register our stream with the central dispatcher.
    // The dispatcher routes buffers matching our stream_id to process_buffers().
    // Note: buffers are released by the dispatcher, not by us.
    dispatcher_.register_stream(
        ecpri_iq_stream_id_,
        [this](struct jbpf_io_stream_id* sid, void** bufs, int n) {
            process_buffers(sid, bufs, n);
        });

    return libe3::ErrorCode::SUCCESS;
}

std::vector<uint8_t> E3SMSpectrum::ran_function_data() const {
    std::vector<uint8_t> encoded;
    if (!e3sm_spectrum::encode_spectrum_ran_function_data(encoded)) {
        std::fprintf(stderr, "[E3SMSpectrum] Failed to APER-encode RAN function data\n");
        return {};
    }
    return encoded;
}

void E3SMSpectrum::process_buffers(struct jbpf_io_stream_id* stream_id, void** bufs, int num_bufs) {
    // Send PRB filter config on first buffer arrival.
    // Must be done here (dispatcher poll thread) — not from start() (subscriber thread),
    // because jbpf_io_channel_send_msg requires the same thread context as the io_ctx owner.
    if (!prb_config_sent_) {
        struct prb_filter_config cfg = {};
        cfg.expected_num_prbu = expected_num_prbu_;
        int rc = jbpf_io_channel_send_msg(io_ctx_, &prb_config_stream_id_, &cfg, sizeof(cfg));
        if (rc == 0) {
            std::printf("[E3SMSpectrum] Sent PRB filter config: expected_num_prbu=%u\n",
                        cfg.expected_num_prbu);
            prb_config_sent_ = true;
        } else {
            std::fprintf(stderr, "[E3SMSpectrum] Failed to send PRB config (rc=%d), will retry\n", rc);
        }
    }

    // Capture a single "now" for the whole batch — saves a syscall per buffer
    // and is accurate to within the time it takes to drain the batch (~µs).
    uint32_t now_us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() & 0x7FFFFFFFULL);

    for (int i = 0; i < num_bufs; i++) {
        auto* sample = static_cast<struct iq_sample_data*>(bufs[i]);

        // Filter on the poll thread so we don't waste queue slots / worker time on
        // packets the dApp would discard anyway.
        if (sample->direction != 1) continue;
        if (expected_num_prbu_ > 0 && sample->num_prbu != expected_num_prbu_) continue;

        // SPSC enqueue. We're the sole producer, so head_ can be loaded relaxed.
        // tail_ is updated by the worker; an acquire load makes its progress visible
        // to our capacity check.
        uint64_t head = queue_head_.load(std::memory_order_relaxed);
        uint64_t tail = queue_tail_.load(std::memory_order_acquire);
        if (head - tail >= QUEUE_CAPACITY) {
            // Full — drop. Real-time data is stale by the time the queue clears.
            dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        QueueEntry& slot = queue_[head & (QUEUE_CAPACITY - 1)];
        std::memcpy(&slot.sample, sample, sizeof(*sample));
        uint32_t codelet_us = static_cast<uint32_t>(sample->timestamp);
        slot.recv_us = (codelet_us > 0) ? ((now_us - codelet_us) & 0x7FFFFFFFu) : 0;
        slot.sample_id = ++total_samples_received_;

        // Release publishes the slot data to the worker.
        queue_head_.store(head + 1, std::memory_order_release);
    }
    // Note: buffer release is handled by the dispatcher — do NOT release here
}

void E3SMSpectrum::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        uint64_t tail = queue_tail_.load(std::memory_order_relaxed);
        uint64_t head = queue_head_.load(std::memory_order_acquire);
        if (head == tail) {
#if defined(__x86_64__) || defined(__i386__)
            _mm_pause();
#endif
            continue;
        }

        // We're the sole consumer — safe to access the slot until we advance tail.
        process_sample(queue_[tail & (QUEUE_CAPACITY - 1)]);

        queue_tail_.store(tail + 1, std::memory_order_release);
    }
}

void E3SMSpectrum::process_sample(const QueueEntry& entry) {
    using clock = std::chrono::steady_clock;
    const auto& sample = entry.sample;

    auto subscribers = get_subscribers();
    if (subscribers.empty()) return;

    // Decompress BFP 9-bit IQ data to int16 pairs into the pre-allocated buffer.
    auto t_decompress_start = clock::now();
    if (!e3sm_spectrum::decompress_bfp_9bit(
            sample.iq_payload, sample.payload_size,
            sample.num_prbu, decompressed_buf_)) {
        std::fprintf(stderr,
            "[E3SMSpectrum] BFP decompression failed for sample #%u "
            "(payload=%u, prbs=%u)\n",
            entry.sample_id, sample.payload_size, sample.num_prbu);
        return;
    }
    auto t_decompress_end = clock::now();

    // Zero-pad decompressed IQ to FFT size with correct subcarrier mapping.
    auto t_fft_start = clock::now();
    uint32_t ofdm_sym_size = static_cast<uint32_t>(sample.num_prbu) * 12;
    uint32_t fft_size = 1;
    while (fft_size < ofdm_sym_size) fft_size <<= 1;
    uint32_t first_carrier_offset = fft_size - (ofdm_sym_size / 2);
    uint32_t half_sc = ofdm_sym_size / 2;

    // assign() reuses capacity reserved in start(); only zeros (no realloc) past first call.
    padded_buf_.assign(fft_size * 2, 0);

    std::memcpy(&padded_buf_[first_carrier_offset * 2],
                decompressed_buf_.data(),
                half_sc * 2 * sizeof(int16_t));
    std::memcpy(&padded_buf_[0],
                &decompressed_buf_[half_sc * 2],
                half_sc * 2 * sizeof(int16_t));
    auto t_fft_end = clock::now();

    // Build the indication; reusing indication_ keeps iq_data's heap buffer hot.
    indication_.iq_data.assign(
        reinterpret_cast<const uint8_t*>(padded_buf_.data()),
        reinterpret_cast<const uint8_t*>(padded_buf_.data()) +
            padded_buf_.size() * sizeof(int16_t));
    indication_.sample_count = fft_size;
    indication_.timestamp = static_cast<uint32_t>(sample.timestamp);

    // APER encode into the pre-allocated buffer.
    auto t_encode_start = clock::now();
    encoded_buf_.clear();
    bool encode_ok = e3sm_spectrum::encode_spectrum_iq_indication(indication_, encoded_buf_);
    auto t_encode_end = clock::now();

    if (encode_ok) {
        for (uint32_t dapp_id : subscribers) {
            libe3::Pdu pdu = make_indication_pdu(dapp_id, RAN_FUNCTION_ID, encoded_buf_);
            auto rc = emit_outbound(std::move(pdu));
            if (rc != libe3::ErrorCode::SUCCESS) {
                std::fprintf(stderr,
                    "[E3SMSpectrum] Failed to send indication to dApp %u: %s\n",
                    dapp_id, libe3::error_code_to_string(rc));
            }
        }
    } else {
        std::fprintf(stderr,
            "[E3SMSpectrum] Failed to APER-encode IQ indication #%u\n",
            entry.sample_id);
    }

    auto ns = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
    };
    static std::ofstream stats_log = [] {
        std::ofstream f("statistics.log", std::ios::out | std::ios::trunc);
        f << "sample_id,recv_us,decompress_ns,fft_ns,encode_ns\n";
        return f;
    }();
    stats_log << entry.sample_id << ','
              << entry.recv_us << ','
              << ns(t_decompress_start, t_decompress_end) << ','
              << ns(t_fft_start, t_fft_end) << ','
              << ns(t_encode_start, t_encode_end) << '\n';
    stats_log.flush();
}

void E3SMSpectrum::stop() {
    if (!running_) {
        return;
    }
    // Unregister from the dispatcher so we stop receiving buffers
    dispatcher_.unregister_stream(ecpri_iq_stream_id_);

    // Signal worker to exit and wait for it.
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }

    uint64_t dropped = dropped_.load(std::memory_order_relaxed);
    if (dropped > 0) {
        std::printf("[E3SMSpectrum] Dropped %lu samples due to full SPSC queue\n",
                    static_cast<unsigned long>(dropped));
    }

    // Unload codelets from srsRAN
    if (codelets_loaded_) {
        unload_codelets();
        codelets_loaded_ = false;
    }
}

libe3::ErrorCode E3SMSpectrum::handle_control_action(
    uint32_t request_message_id,
    const libe3::DAppControlAction& action)
{
    e3sm_spectrum::SpectrumPRBBlacklistControl ctrl;
    if (!e3sm_spectrum::decode_spectrum_prb_blacklist_control(action.action_data, ctrl)) {
        std::fprintf(stderr,
            "[E3SMSpectrum] Failed to APER-decode PRBBlacklistControl from dApp %u\n",
            action.dapp_identifier);
        auto nack = make_message_ack_pdu(request_message_id, libe3::ResponseCode::NEGATIVE);
        emit_outbound(std::move(nack));
        return libe3::ErrorCode::DECODE_FAILED;
    }

    std::printf("[E3SMSpectrum] PRBBlacklistControl from dApp %u: "
                "prbs=[", action.dapp_identifier);
    for (size_t i = 0; i < ctrl.blacklisted_prbs.size(); i++) {
        std::printf("%s%d", i > 0 ? ", " : "", ctrl.blacklisted_prbs[i]);
    }
    std::printf("] samplingThreshold=%d validityPeriod=%d\n",
                ctrl.sampling_threshold, ctrl.validity_period);

    // TODO: Apply the PRB blacklist to the RAN

    auto ack = make_message_ack_pdu(request_message_id, libe3::ResponseCode::POSITIVE);
    emit_outbound(std::move(ack));
    return libe3::ErrorCode::SUCCESS;
}
