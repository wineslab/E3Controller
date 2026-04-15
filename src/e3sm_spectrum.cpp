#include "e3sm_spectrum.h"
#include "e3sm/e3sm_spect_wrapper.h"
#include "e3sm/bfp_decompress.h"
#include <unistd.h>

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

    // Load codelets into srsRAN if a codelet path was configured
    if (!codelet_base_path_.empty()) {
        if (!load_codelets()) {
            return libe3::ErrorCode::INTERNAL_ERROR;
        }
        codelets_loaded_ = true;
    }

    // Register our stream with the central dispatcher.
    // The dispatcher routes buffers matching our stream_id to process_buffers().
    // Note: buffers are released by the dispatcher, not by us.
    dispatcher_.register_stream(
        ecpri_iq_stream_id_,
        [this](struct jbpf_io_stream_id* sid, void** bufs, int n) {
            process_buffers(sid, bufs, n);
        });

    running_ = true;
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

    // Check for subscribers once per batch
    auto subscribers = get_subscribers();
    bool has_subscribers = !subscribers.empty();

    for (int i = 0; i < num_bufs; i++) {
        auto* sample = static_cast<struct iq_sample_data*>(bufs[i]);
        total_samples_received_++;

        std::printf("[EcpriIQ] #%d | ts=%lu dir=%s frame=%u sf=%u slot=%u sym=%u "
                    "sec=%u prb_start=%u prb_num=%u comp=%u iq_w=%u payload=%u bytes\n",
                    total_samples_received_,
                    sample->timestamp,
                    sample->direction == 0 ? "DL" : "UL",
                    sample->frame_id,
                    sample->subframe_id,
                    sample->slot_id,
                    sample->symbol_id,
                    sample->section_id,
                    sample->start_prbu,
                    sample->num_prbu,
                    sample->comp_method,
                    sample->iq_width,
                    sample->payload_size);
        if (has_subscribers && sample->direction == 1) {
            // PRB filtering is now done in the codelet — all samples here have matching PRB count
            // Decompress BFP 9-bit IQ data to int16 pairs
            std::vector<int16_t> decompressed;
            if (!e3sm_spectrum::decompress_bfp_9bit(
                    sample->iq_payload, sample->payload_size,
                    sample->num_prbu, decompressed)) {
                std::fprintf(stderr,
                    "[E3SMSpectrum] BFP decompression failed for sample #%d "
                    "(payload=%u, prbs=%u)\n",
                    total_samples_received_, sample->payload_size, sample->num_prbu);
                continue;
            }

            // Zero-pad decompressed IQ to FFT size with correct subcarrier mapping.
            // The dApp expects fft_size complex samples in FFT-bin order:
            //   - Lower-freq subcarriers (first half) → upper FFT bins
            //   - Upper-freq subcarriers (second half) → lower FFT bins
            //   - Guard band (zeros) in the middle
            uint32_t ofdm_sym_size = static_cast<uint32_t>(sample->num_prbu) * 12;
            uint32_t fft_size = 1;
            while (fft_size < ofdm_sym_size) fft_size <<= 1; // next power of 2
            uint32_t first_carrier_offset = fft_size - (ofdm_sym_size / 2);
            uint32_t half_sc = ofdm_sym_size / 2; // half of active subcarriers

            std::vector<int16_t> padded(fft_size * 2, 0); // fft_size complex = fft_size*2 int16s

            // Lower-freq half of subcarriers → upper FFT bins [first_carrier_offset, fft_size)
            std::memcpy(&padded[first_carrier_offset * 2],
                        decompressed.data(),
                        half_sc * 2 * sizeof(int16_t));

            // Upper-freq half of subcarriers → lower FFT bins [0, half_sc)
            std::memcpy(&padded[0],
                        &decompressed[half_sc * 2],
                        half_sc * 2 * sizeof(int16_t));

            // Build the indication with FFT-sized int16 I/Q pairs
            e3sm_spectrum::SpectrumIQIndication indication;
            indication.iq_data.assign(
                reinterpret_cast<const uint8_t*>(padded.data()),
                reinterpret_cast<const uint8_t*>(padded.data()) +
                    padded.size() * sizeof(int16_t));
            // sample_count = number of complex I/Q samples (fft_size)
            indication.sample_count = fft_size;
            indication.timestamp = static_cast<uint32_t>(sample->timestamp / 1000000000ULL);

            // APER encode
            std::vector<uint8_t> encoded;
            if (e3sm_spectrum::encode_spectrum_iq_indication(indication, encoded)) {
                for (uint32_t dapp_id : subscribers) {
                    libe3::Pdu pdu = make_indication_pdu(dapp_id, RAN_FUNCTION_ID, encoded);
                    auto rc = emit_outbound(std::move(pdu));
                    if (rc != libe3::ErrorCode::SUCCESS) {
                        std::fprintf(stderr,
                            "[E3SMSpectrum] Failed to send indication to dApp %u: %s\n",
                            dapp_id, libe3::error_code_to_string(rc));
                    }
                }
            } else {
                std::fprintf(stderr,
                    "[E3SMSpectrum] Failed to APER-encode IQ indication #%d\n",
                    total_samples_received_);
            }
        }
        // Note: buffer release is handled by the dispatcher — do NOT release here
    }
}

void E3SMSpectrum::stop() {
    if (!running_) {
        return;
    }
    // Unregister from the dispatcher so we stop receiving buffers
    dispatcher_.unregister_stream(ecpri_iq_stream_id_);
    running_ = false;

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
