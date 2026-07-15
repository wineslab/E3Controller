/*
 * IqPipeline implementation — extracted from the legacy single-SM
 * E3SMSpectrum. See iq_pipeline.h for the architectural rationale.
 */

#include "iq_pipeline.h"
#include "e3sm/utils/bfp_decompress.h"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

extern "C" {
#include "jbpf_lcm_ipc.h"
#include "jbpf_mem_mgmt.h"
}

namespace e3sm_pipeline {

namespace {

// PRB-filter config message — must match the codelet's expected layout
// (codelets/ecpri_iq_samples/ecpri_iq_data.h:struct prb_filter_config).
// Kept here (not in a shared header) because it's private to the codelet
// contract; if the codelet adds fields we update both sides.
// NOTE: expected_num_prbu is uint16_t to match the codelet — the jbpf
// control-input channel element size is sizeof() of the codelet's struct
// (2 bytes). Sending a wider type makes jbpf_io_channel_send_msg reject the
// message ("Data size 4 is larger than the element size 2").
struct prb_filter_config {
    uint16_t expected_num_prbu;
};

}  // namespace

IqPipeline::IqPipeline(JbpfDispatcher& dispatcher,
                       jbpf_io_ctx* io_ctx,
                       Config cfg)
    : dispatcher_(dispatcher),
      io_ctx_(io_ctx),
      config_(std::move(cfg)),
      queue_(QUEUE_CAPACITY)
{}

IqPipeline::~IqPipeline() {
    stop();
}

void IqPipeline::register_consumer(Consumer c) {
    consumers_.push_back(std::move(c));
}

bool IqPipeline::load_codelets() {
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, config_.lcm_socket_path.c_str(),
                 JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_load_req_s load_req = {};
    std::strncpy(load_req.codeletset_id.name, "ecpri_iq_samples",
                 JBPF_CODELETSET_NAME_LEN - 1);

    load_req.num_codelet_descriptors = 1;
    auto& desc = load_req.codelet_descriptor[0];

    std::strncpy(desc.codelet_name, "collector", JBPF_CODELET_NAME_LEN - 1);
    std::strncpy(desc.hook_name, "capture_xran_packet",
                 sizeof(desc.hook_name) - 1);

    const std::string full_path =
        config_.codelet_base_path + "/ecpri_iq_samples/ecpri_iq_collect.o";
    std::strncpy(desc.codelet_path, full_path.c_str(), JBPF_PATH_LEN - 1);

    desc.priority = 1;
    desc.runtime_threshold = 0;
    desc.num_in_io_channel = 1;
    std::strncpy(desc.in_io_channel[0].name, "prb_config_map",
                 sizeof(desc.in_io_channel[0].name) - 1);
    std::memcpy(&desc.in_io_channel[0].stream_id, &prb_config_stream_id_,
                sizeof(jbpf_io_stream_id_t));
    desc.in_io_channel[0].has_serde = false;

    desc.num_linked_maps = 0;

    desc.num_out_io_channel = 1;
    std::strncpy(desc.out_io_channel[0].name, "output_map",
                 sizeof(desc.out_io_channel[0].name) - 1);
    std::memcpy(&desc.out_io_channel[0].stream_id, &ecpri_iq_stream_id_,
                sizeof(jbpf_io_stream_id_t));
    desc.out_io_channel[0].has_serde = false;

    int rc = jbpf_lcm_ipc_send_codeletset_load_req(&address, &load_req);
    if (rc != 0) {
        std::fprintf(stderr,
            "[IqPipeline] Failed to load codeletset 'ecpri_iq_samples' "
            "via LCM IPC (rc=%d)\n", rc);
        return false;
    }
    std::printf("[IqPipeline] Codeletset 'ecpri_iq_samples' loaded successfully\n");
    return true;
}

void IqPipeline::unload_codelets() {
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, config_.lcm_socket_path.c_str(),
                 JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_unload_req_s unload_req = {};
    std::strncpy(unload_req.codeletset_id.name, "ecpri_iq_samples",
                 JBPF_CODELETSET_NAME_LEN - 1);

    int rc = jbpf_lcm_ipc_send_codeletset_unload_req(&address, &unload_req);
    if (rc != 0) {
        std::fprintf(stderr,
            "[IqPipeline] Failed to unload codeletset 'ecpri_iq_samples' (rc=%d)\n",
            rc);
    } else {
        std::printf("[IqPipeline] Codeletset 'ecpri_iq_samples' unloaded successfully\n");
    }
}

libe3::ErrorCode IqPipeline::start() {
    if (running_.load(std::memory_order_acquire)) {
        return libe3::ErrorCode::SUCCESS;
    }

    // Reserve worker scratch sized for the largest plausible BWP
    // (273 PRBs * 12 SCs * 2 I/Q = 6552 int16) so the steady-state path
    // never reallocates after the first decompress call.
    constexpr uint32_t kMaxNumPrbu     = 273;
    constexpr uint32_t kMaxOfdmSymSize = kMaxNumPrbu * 12;
    decompressed_buf_.reserve(kMaxOfdmSymSize * 2);

    if (!config_.codelet_base_path.empty()) {
        if (!load_codelets()) {
            return libe3::ErrorCode::INTERNAL_ERROR;
        }
        codelets_loaded_ = true;
    }

    // Mark running BEFORE spawning the worker so its first iteration sees the flag.
    running_.store(true, std::memory_order_release);

    worker_ = std::thread([this] { worker_loop(); });
    if (config_.worker_core >= 0) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(config_.worker_core, &mask);
        if (pthread_setaffinity_np(worker_.native_handle(), sizeof(mask), &mask) != 0) {
            std::fprintf(stderr,
                "[IqPipeline] WARNING: failed to pin worker thread to core %d (%s)\n",
                config_.worker_core, std::strerror(errno));
        } else {
            std::printf("[IqPipeline] Worker thread pinned to core %d\n",
                        config_.worker_core);
        }
    }

    // Register the dispatcher stream LAST so we only start receiving once
    // the worker is alive. defer_release=true → we own the jbpf ring
    // buffers from the moment they land here; the worker releases each
    // after the consumer fan-out (zero-copy path — no 8 KB memcpy on the
    // poll thread).
    dispatcher_.register_stream(
        ecpri_iq_stream_id_,
        [this](struct jbpf_io_stream_id* sid, void** bufs, int n) {
            process_buffers(sid, bufs, n);
        },
        /*defer_release=*/true);

    return libe3::ErrorCode::SUCCESS;
}

void IqPipeline::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    dispatcher_.unregister_stream(ecpri_iq_stream_id_);

    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }

    // The worker may have exited (running_=false) with buffers still queued.
    // We own them (defer_release=true), so release any it didn't reach BEFORE
    // unload_codelets() tears down the channel/mempool below. The worker has
    // joined, so this is the only thread touching the queue now.
    {
        const uint64_t head = queue_head_.load(std::memory_order_relaxed);
        uint64_t       tail = queue_tail_.load(std::memory_order_relaxed);
        for (; tail != head; ++tail) {
            jbpf_io_channel_release_buf(queue_[tail & (QUEUE_CAPACITY - 1)].buf);
        }
        queue_tail_.store(head, std::memory_order_relaxed);
    }

    const uint64_t dropped = dropped_.load(std::memory_order_relaxed);
    if (dropped > 0) {
        std::printf("[IqPipeline] Dropped %lu samples due to full SPSC queue\n",
                    static_cast<unsigned long>(dropped));
    }

    if (codelets_loaded_) {
        unload_codelets();
        codelets_loaded_ = false;
    }
}

void IqPipeline::process_buffers(struct jbpf_io_stream_id* /*stream_id*/,
                                 void** bufs, int num_bufs) {
    // Send PRB filter config on first buffer arrival. Must be done here
    // (dispatcher poll thread) — not from start() — because
    // jbpf_io_channel_send_msg requires the same thread context as the
    // io_ctx owner.
    if (!prb_config_sent_) {
        struct prb_filter_config cfg = {};
        cfg.expected_num_prbu = config_.expected_num_prbu;
        int rc = jbpf_io_channel_send_msg(io_ctx_, &prb_config_stream_id_,
                                          &cfg, sizeof(cfg));
        if (rc == 0) {
            std::printf("[IqPipeline] Sent PRB filter config: expected_num_prbu=%u\n",
                        cfg.expected_num_prbu);
            prb_config_sent_ = true;
        } else {
            std::fprintf(stderr,
                "[IqPipeline] Failed to send PRB config (rc=%d), will retry\n", rc);
        }
    }

    // One "now" for the whole batch — saves a syscall per buffer.
    // CLOCK_REALTIME ns matches the codelet's sample.codelet_ts_ns clock
    // domain so consumers can subtract them for codelet_to_dispatch_us.
    struct timespec dispatch_ts;
    clock_gettime(CLOCK_REALTIME, &dispatch_ts);
    const uint64_t dispatch_ts_ns =
        static_cast<uint64_t>(dispatch_ts.tv_sec) * 1000000000ULL +
        static_cast<uint64_t>(dispatch_ts.tv_nsec);
    const uint32_t now_us = static_cast<uint32_t>(
        (dispatch_ts_ns / 1000ULL) & 0x7FFFFFFFULL);

    for (int i = 0; i < num_bufs; i++) {
        auto* sample = static_cast<struct iq_sample_data*>(bufs[i]);

        // Direction filter (1 = UL); num_prbu filter (drop SRS/PRACH/control).
        // defer_release=true means we own the buffer even for filter-drops -
        // release it right away so the jbpf ring slot goes back into circulation.
        if (sample->direction != 1
            || (config_.expected_num_prbu > 0
                && sample->num_prbu != config_.expected_num_prbu)) {
            jbpf_io_channel_release_buf(bufs[i]);
            continue;
        }

        // SPSC enqueue. Single producer, so head_ is relaxed; tail_ acquire
        // makes the worker's progress visible to our capacity check.
        const uint64_t head = queue_head_.load(std::memory_order_relaxed);
        const uint64_t tail = queue_tail_.load(std::memory_order_acquire);
        if (head - tail >= QUEUE_CAPACITY) {
            // Queue full: worker will never see this buffer, so release it here
            // ourselves to avoid leaking the jbpf ring slot.
            dropped_.fetch_add(1, std::memory_order_relaxed);
            jbpf_io_channel_release_buf(bufs[i]);
            continue;
        }

        // Option A: enqueue the buffer POINTER (zero-copy) - no 8 KB memcpy.
        // The worker reads sample fields straight from the jbpf ring and releases
        // it after the consumer fan-out.
        QueueEntry& slot = queue_[head & (QUEUE_CAPACITY - 1)];
        slot.buf = bufs[i];
        const uint32_t codelet_us = static_cast<uint32_t>(sample->timestamp);
        slot.recv_us        = (codelet_us > 0) ? ((now_us - codelet_us) & 0x7FFFFFFFu) : 0;
        slot.dispatch_ts_ns = dispatch_ts_ns;
        slot.sample_id      = ++total_samples_received_;

        queue_head_.store(head + 1, std::memory_order_release);
    }
    // We own the buffers (defer_release=true): enqueued ones are released by the
    // worker after fan-out, dropped/filtered ones were released above. So the
    // dispatcher must NOT release here - and it won't (defer_release).
}

void IqPipeline::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        const uint64_t tail = queue_tail_.load(std::memory_order_relaxed);
        const uint64_t head = queue_head_.load(std::memory_order_acquire);
        if (head == tail) {
#if defined(__x86_64__) || defined(__i386__)
            _mm_pause();
#endif
            continue;
        }

        QueueEntry& entry = queue_[tail & (QUEUE_CAPACITY - 1)];
        dispatch_sample(entry);
        // Consumers are synchronous, so the jbpf buffer is safe to release now.
        // We own it (defer_release=true) - release exactly once per enqueued slot.
        jbpf_io_channel_release_buf(entry.buf);

        queue_tail_.store(tail + 1, std::memory_order_release);
    }
}

void IqPipeline::dispatch_sample(const QueueEntry& entry) {
    using clock = std::chrono::steady_clock;
    // entry.buf points straight at the jbpf ring buffer (Option A, zero-copy).
    // Valid until the worker releases it right after this fan-out returns.
    const auto& sample = *static_cast<const struct iq_sample_data*>(entry.buf);

    const auto t_decompress_start = clock::now();
    if (!e3sm_spectrum::decompress_bfp_9bit(
            sample.iq_payload, sample.payload_size,
            sample.num_prbu, decompressed_buf_)) {
        std::fprintf(stderr,
            "[IqPipeline] BFP decompression failed for sample #%u "
            "(payload=%u, prbs=%u)\n",
            entry.sample_id, sample.payload_size, sample.num_prbu);
        return;
    }
    const auto t_decompress_end = clock::now();

    DecompressedSample dec;
    dec.raw = &sample;
    dec.decompressed_iq = decompressed_buf_.data();
    dec.decompressed_size = decompressed_buf_.size();
    dec.recv_us = entry.recv_us;
    dec.sample_id = entry.sample_id;
    dec.gnb_ts_ns      = sample.gnb_ts_ns;
    dec.codelet_ts_ns  = sample.codelet_ts_ns;
    dec.dispatch_ts_ns = entry.dispatch_ts_ns;
    dec.decompress_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t_decompress_end - t_decompress_start).count();

    // Fan out by const-ref — consumers must finish their callback before
    // the worker reuses decompressed_buf_ for the next sample.
    for (auto& c : consumers_) {
        c(dec);
    }
}

}  // namespace e3sm_pipeline
