#include "e3sm_spectrum.h"
#include "e3sm/e3sm_spect_wrapper.h"

libe3::ErrorCode E3SMSpectrum::start()
{
    if (running_) {
        return libe3::ErrorCode::SUCCESS;
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
        // TODO: need to be checked
        // Why I need to check has subscribers if the sm does not start if I do not have any?
        // Also, I think the library will check to which subscriber send the indication
        if (has_subscribers && sample->symbol_id == 12) {
            // Build the indication from shared memory data
            e3sm_spectrum::SpectrumIQIndication indication;
            indication.iq_data.assign(
                sample->iq_payload,
                sample->iq_payload + sample->payload_size);
            indication.sample_count = static_cast<uint32_t>(sample->num_prbu) * 12;
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
