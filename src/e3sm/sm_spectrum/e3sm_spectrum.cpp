/*
 * E3SMSpectrum — RF=1. IQ telemetry via IqPipeline (ecpri_iq_samples) + PRB-
 * blacklist control. See the header for the contract. PRB-blacklist application
 * to the RAN is still a stub (logged as a TODO in handle_control_action).
 */

#include "e3sm_spectrum.h"

#include <cstdio>
#include <cstring>

libe3::ErrorCode E3SMSpectrum::init() {
    // Register the pipeline consumer once at SM registration. IqPipeline keeps
    // consumers across start/stop cycles; doing it here (not start()) lets us
    // defer start() to the first dApp subscription without losing the wiring.
    pipeline_.register_consumer(
        [this](const e3sm_pipeline::DecompressedSample& s) { on_sample(s); });
    return libe3::ErrorCode::SUCCESS;
}

void E3SMSpectrum::destroy() { stop(); }

libe3::ErrorCode E3SMSpectrum::start() {
    if (running_.load(std::memory_order_acquire)) {
        return libe3::ErrorCode::SUCCESS;
    }

    // Reserve worker-side scratch once so the steady-state path never reallocs.
    // Sized for the largest plausible BWP (273 PRBs -> 3276 SCs -> fft 4096).
    constexpr uint32_t kMaxFftSize = 4096;
    padded_buf_.reserve(static_cast<size_t>(kMaxFftSize) * 2u);
    encoded_buf_.reserve(64 * 1024);
    indication_.iq_data.reserve(static_cast<size_t>(kMaxFftSize) * 2u * sizeof(int16_t));

    // Lazy pipeline boot: codelet load via LCM IPC + worker thread spawn happen
    // here. libe3 calls start() on the first RF=1 subscription, so srsRAN's LCM
    // IPC socket is up by then in the standard launch order (controller -> srsRAN
    // -> dApp).
    auto rc = pipeline_.start();
    if (rc != libe3::ErrorCode::SUCCESS) {
        std::fprintf(stderr,
            "[E3SMSpectrum] IqPipeline start failed (%s) - is srsRAN running?\n",
            libe3::error_code_to_string(rc));
        return rc;
    }

    running_.store(true, std::memory_order_release);
    std::printf("[E3SMSpectrum] Started - ecpri_iq codelet loaded, worker running\n");
    return libe3::ErrorCode::SUCCESS;
}

void E3SMSpectrum::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    pipeline_.stop();
}

std::vector<uint8_t> E3SMSpectrum::ran_function_data() const {
    // RF=1 Spectrum is ASN.1/APER-only (Spectrum-RanFunctionData). The dApp
    // that consumes RF=1 (the public Spectrum Sharing dApp) speaks asn1.
    std::vector<uint8_t> encoded;
    if (!e3sm_spectrum::encode_spectrum_ran_function_data(encoded)) {
        std::fprintf(stderr, "[E3SMSpectrum] Failed to encode RAN function data\n");
        return {};
    }
    return encoded;
}

void E3SMSpectrum::on_sample(const e3sm_pipeline::DecompressedSample& s) {
    if (!running_.load(std::memory_order_acquire)) return;

    const auto subs = get_subscribers();
    if (subs.empty()) return;  // No one is listening.

    // RF=1 in-band IQ telemetry is APER-only (Spectrum-IQDataIndication). 
    // There is no in-band IQ JSON encoder, so
    // warn once and drop if the agent is running JSON.
    const libe3::EncodingFormat enc =
        agent_ ? agent_->config().encoding : libe3::EncodingFormat::ASN1;
    if (enc != libe3::EncodingFormat::ASN1) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::fprintf(stderr,
                "[E3SMSpectrum] RF=1 IQ indication requires --encoding asn1; "
                "JSON in-band IQ is not implemented - dropping indications.\n");
        }
        return;
    }

    if (s.raw == nullptr || s.decompressed_iq == nullptr) return;

    // Zero-pad the decompressed section to FFT size with the standard centered
    // subcarrier mapping (DC at index 0; upper half at high indices). This is
    // the exact layout the dApp's FFT expects — ported from the pre-IqPipeline
    // path so the public dApp is bit-compatible.
    const uint32_t ofdm_sym_size = static_cast<uint32_t>(s.raw->num_prbu) * 12u;
    if (ofdm_sym_size == 0) return;
    uint32_t fft_size = 1;
    while (fft_size < ofdm_sym_size) fft_size <<= 1;
    const uint32_t half_sc = ofdm_sym_size / 2u;
    const uint32_t first_carrier_offset = fft_size - half_sc;

    // Defensive: the pipeline must have handed us a full section.
    if (s.decompressed_size < static_cast<size_t>(ofdm_sym_size) * 2u) return;

    padded_buf_.assign(static_cast<size_t>(fft_size) * 2u, 0);
    // Upper half of the spectrum -> high indices from first_carrier_offset.
    std::memcpy(&padded_buf_[static_cast<size_t>(first_carrier_offset) * 2u],
                s.decompressed_iq,
                static_cast<size_t>(half_sc) * 2u * sizeof(int16_t));
    // Lower half -> low indices from 0.
    std::memcpy(&padded_buf_[0],
                &s.decompressed_iq[static_cast<size_t>(half_sc) * 2u],
                static_cast<size_t>(half_sc) * 2u * sizeof(int16_t));

    indication_.iq_data.assign(
        reinterpret_cast<const uint8_t*>(padded_buf_.data()),
        reinterpret_cast<const uint8_t*>(padded_buf_.data()) +
            padded_buf_.size() * sizeof(int16_t));
    indication_.sample_count = fft_size;
    indication_.timestamp = static_cast<uint32_t>(s.raw->timestamp);

    encoded_buf_.clear();
    if (!e3sm_spectrum::encode_spectrum_iq_indication(indication_, encoded_buf_)) {
        std::fprintf(stderr, "[E3SMSpectrum] Failed to APER-encode IQ indication\n");
        return;
    }

    // Fan out one indication per subscriber.
    for (uint32_t dapp_id : subs) {
        libe3::Pdu pdu = make_indication_pdu(dapp_id, RAN_FUNCTION_ID, encoded_buf_);
        auto rc = emit_outbound(std::move(pdu));
        if (rc != libe3::ErrorCode::SUCCESS) {
            std::fprintf(stderr,
                "[E3SMSpectrum] Failed to send indication to dApp %u: %s\n",
                dapp_id, libe3::error_code_to_string(rc));
        }
    }
}

libe3::ErrorCode E3SMSpectrum::handle_control_action(
    uint32_t request_message_id,
    const libe3::DAppControlAction& action)
{
    e3sm_spectrum::SpectrumPRBBlacklistControl ctrl;
    if (!e3sm_spectrum::decode_spectrum_prb_blacklist_control(
            action.action_data, ctrl)) {
        std::fprintf(stderr,
            "[E3SMSpectrum] Failed to APER-decode PRBBlacklistControl from dApp %u\n",
            action.dapp_identifier);
        auto nack = make_message_ack_pdu(request_message_id,
                                         libe3::ResponseCode::NEGATIVE);
        emit_outbound(std::move(nack));
        return libe3::ErrorCode::DECODE_FAILED;
    }

    std::printf("[E3SMSpectrum] PRBBlacklistControl from dApp %u: prbs=[",
                action.dapp_identifier);
    for (std::size_t i = 0; i < ctrl.blacklisted_prbs.size(); ++i) {
        std::printf("%s%d", i > 0 ? ", " : "", ctrl.blacklisted_prbs[i]);
    }
    std::printf("] samplingThreshold=%d validityPeriod=%d\n",
                ctrl.sampling_threshold, ctrl.validity_period);

    // TODO: Apply the PRB blacklist to the RAN. Currently an ACK-and-log
    // stub; flagged in README Known Limitations.

    auto ack = make_message_ack_pdu(request_message_id,
                                    libe3::ResponseCode::POSITIVE);
    emit_outbound(std::move(ack));
    return libe3::ErrorCode::SUCCESS;
}
