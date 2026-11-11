/*
 * E3SMSpectrum — RF=1. PRB blacklist control, applied in the gNB's scheduler
 * through PrbBlockRelay when prb_block.enabled; otherwise the legacy IQ
 * telemetry via IqPipeline (ecpri_iq_samples). See the header for the contract.
 */

#include "e3sm_spectrum.h"

#include <libe3/latrec.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

// aux2 of the decode stages: the PDU type of a dApp control.
static constexpr uint64_t kPduDAppControl = static_cast<uint64_t>(libe3::PduType::DAPP_CONTROL_ACTION);

// CLOCK_MONOTONIC in ns, the clock of the stage records and of the gNB.
static uint64_t monotonic_now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

libe3::ErrorCode E3SMSpectrum::init() {
    // Register the pipeline consumer once at SM registration. IqPipeline keeps
    // consumers across start/stop cycles; doing it here (not start()) lets us
    // defer start() to the first dApp subscription without losing the wiring.
    pipeline_.register_consumer(
        [this](const e3sm_pipeline::DecompressedSample& s) { on_sample(s); });
    // The relay ACKs a block once it is in the gNB, from the main loop.
    if (prb_block_ != nullptr) {
        prb_block_->set_ack_handler([this](uint32_t id, bool ok) { send_ack(id, ok); });
    }
    return libe3::ErrorCode::SUCCESS;
}

void E3SMSpectrum::destroy() { stop(); }

libe3::ErrorCode E3SMSpectrum::start() {
    if (running_.load(std::memory_order_acquire)) {
        return libe3::ErrorCode::SUCCESS;
    }

    // prb_block.enabled: the SM serves the PRB block control only. The prb_block
    // codelet goes into the gNB's scheduler; no IQ telemetry on RF=1 (the
    // L1-KPM SM carries the IQ).
    if (prb_block_ != nullptr) {
        if (!prb_block_->start()) {
            return libe3::ErrorCode::INTERNAL_ERROR;
        }
        running_.store(true, std::memory_order_release);
        std::printf("[E3SMSpectrum] Started - prb_block codelet loaded, PRB blocks go to the gNB's scheduler\n");
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
    if (prb_block_ != nullptr) {
        // Unloading the codelet clears the block in the gNB.
        prb_block_->stop();
        return;
    }
    pipeline_.stop();
}

void E3SMSpectrum::send_ack(uint32_t id, bool ok) {
    emit_outbound(make_message_ack_pdu(
        id, ok ? libe3::ResponseCode::POSITIVE : libe3::ResponseCode::NEGATIVE));
}

std::vector<uint8_t> E3SMSpectrum::ran_function_data() const {
    // In the agent's encoding: APER Spectrum-RanFunctionData, or its JSON twin
    // (the same fields as a JSON object).
    const libe3::EncodingFormat enc =
        agent_ ? agent_->config().encoding : libe3::EncodingFormat::ASN1;

    std::vector<uint8_t> encoded;
    bool ok = false;
    if (enc == libe3::EncodingFormat::JSON) {
        ok = e3sm_spectrum::encode_spectrum_ran_function_data_json(encoded);
    } else {
        ok = e3sm_spectrum::encode_spectrum_ran_function_data(encoded);
    }
    if (!ok) {
        std::fprintf(stderr, "[E3SMSpectrum] Failed to encode RAN function data\n");
        return {};
    }
    return encoded;
}

void E3SMSpectrum::on_sample(const e3sm_pipeline::DecompressedSample& s) {
    if (!running_.load(std::memory_order_acquire)) return;

    const auto subs = get_subscribers();
    if (subs.empty()) return;  // No one is listening.

    using clock = std::chrono::steady_clock;

    // CLOCK_REALTIME ns at on_sample entry. Same domain as
    // s.gnb_ts_ns / s.codelet_ts_ns / s.dispatch_ts_ns, so the three
    // RAN-side stage durations subtract cleanly into statistics_spectrum.log.
    // Mirrors E3SMLayer1's handler-entry stamp.
    auto realtime_ns_now = []() -> uint64_t {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    };
    const uint64_t handler_entry_ns = realtime_ns_now();

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
    const auto t_encode_start = clock::now();
    const bool encoded_ok =
        e3sm_spectrum::encode_spectrum_iq_indication(indication_, encoded_buf_);
    const auto t_encode_end = clock::now();
    if (!encoded_ok) {
        std::fprintf(stderr, "[E3SMSpectrum] Failed to APER-encode IQ indication\n");
        return;
    }

    // Fan out one indication per subscriber.
    const auto t_emit_start = clock::now();
    for (uint32_t dapp_id : subs) {
        libe3::Pdu pdu = make_indication_pdu(dapp_id, RAN_FUNCTION_ID, encoded_buf_);
        auto rc = emit_outbound(std::move(pdu));
        if (rc != libe3::ErrorCode::SUCCESS) {
            std::fprintf(stderr,
                "[E3SMSpectrum] Failed to send indication to dApp %u: %s\n",
                dapp_id, libe3::error_code_to_string(rc));
        }
    }
    const auto t_emit_end = clock::now();

    // --- Per-sample statistics (disabled unless --stats-log was given) ---
    ++sample_publish_seq_;
    if (stats_log_path_.empty()) {
        return;
    }
    // Schema (matches E3SMLayer1's statistics_layer1.log columns for the
    // three RAN-side stages; the trailing per-SM stage names differ):
    //   sample_seq,
    //   gnb_to_codelet_us,     ocudu hook -> codelet entry (jbpf invocation)
    //   codelet_to_dispatch_us,codelet -> IqPipeline dispatcher poll
    //   dispatch_to_handler_us,dispatcher -> on_sample (SPSC queue wait)
    //   decompress_ns,         BFP-9 decompression (IqPipeline worker)
    //   encode_ns,             APER encode cost
    //   emit_ns,               emit_outbound fan-out (SM-side enqueue)
    //   fft_size,              zero-padded FFT bin count
    //   payload_bytes          codelet-side compressed section size
    if (!stats_log_.is_open()) {
        stats_log_.open(stats_log_path_, std::ios::out | std::ios::trunc);
        stats_log_ << "sample_seq,"
                      "gnb_to_codelet_us,codelet_to_dispatch_us,dispatch_to_handler_us,"
                      "decompress_ns,encode_ns,emit_ns,fft_size,payload_bytes\n";
    }
    auto ns_between = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
    };
    auto sat_us = [](uint64_t lhs, uint64_t rhs) -> uint64_t {
        return (lhs > rhs) ? ((lhs - rhs) / 1000ULL) : 0ULL;
    };
    stats_log_ << sample_publish_seq_ << ','
               << sat_us(s.codelet_ts_ns,  s.gnb_ts_ns)      << ','
               << sat_us(s.dispatch_ts_ns, s.codelet_ts_ns)  << ','
               << sat_us(handler_entry_ns, s.dispatch_ts_ns) << ','
               << s.decompress_ns                            << ','
               << ns_between(t_encode_start, t_encode_end)   << ','
               << ns_between(t_emit_start,   t_emit_end)     << ','
               << fft_size                                   << ','
               << s.raw->payload_size                        << '\n';
    stats_log_.flush();
}

libe3::ErrorCode E3SMSpectrum::handle_control_action(
    uint32_t request_message_id,
    const libe3::DAppControlAction& action)
{
    // Stamped on libe3's inbound thread (its libe3.inbound ring), seq = the
    // control's E3 message id. A NACK sent here gets ACK_SENT here; an accepted
    // block gets it from PrbBlockRelay.
    const uint64_t rx_ns = monotonic_now_ns();
    latrec_tstamp(request_message_id, LATREC_DECODE_E3SM_BEGIN, action.action_data.size(), kPduDAppControl);
    auto reject = [&](libe3::ErrorCode rc) {
        send_ack(request_message_id, false);
        latrec_tstamp(request_message_id, LATREC_ACK_SENT,
                      static_cast<uint64_t>(libe3::ResponseCode::NEGATIVE), 0);
        return rc;
    };

    // Only the PRB blacklist control (id 1) is handled. The Spectrum SM also
    // defines 2 (sensing policy), 3 (sensing reservation) and 4 (PUSCH slots).
    if (action.control_identifier != CONTROL_ID_PRB_BLACKLIST) {
        std::fprintf(stderr,
            "[E3SMSpectrum] Rejected control id %u from dApp %u: only %u (PRB blacklist) is handled\n",
            action.control_identifier, action.dapp_identifier, CONTROL_ID_PRB_BLACKLIST);
        return reject(libe3::ErrorCode::SM_ERROR_INVALID_PARAM);
    }

    // The control comes in the agent's encoding: JSON in a JSON run, APER in an
    // ASN.1 run.
    const libe3::EncodingFormat enc =
        agent_ ? agent_->config().encoding : libe3::EncodingFormat::ASN1;
    const bool json = (enc == libe3::EncodingFormat::JSON);

    e3sm_spectrum::SpectrumPRBBlacklistControl ctrl;
    std::string why;
    e3sm_spectrum::DecodeResult result;
    if (json) {
        result = e3sm_spectrum::decode_spectrum_dapp_control_json(action.action_data, ctrl, why);
    } else {
        result = e3sm_spectrum::decode_spectrum_dapp_control_aper(action.action_data, ctrl, why);
    }

    if (result != e3sm_spectrum::DecodeResult::ok) {
        std::fprintf(stderr,
            "[E3SMSpectrum] Rejected %s control from dApp %u: %s\n",
            json ? "JSON" : "APER", action.dapp_identifier, why.c_str());
        return reject(result == e3sm_spectrum::DecodeResult::unsupported
                          ? libe3::ErrorCode::SM_ERROR_INVALID_PARAM
                          : libe3::ErrorCode::DECODE_FAILED);
    }
    latrec_tstamp(request_message_id, LATREC_DECODE_E3SM_DONE, ctrl.blacklisted_prbs.size(), kPduDAppControl);

    if (prb_block_ == nullptr) {
        // prb_block.enabled is false: decoded and ACKed, not applied.
        std::printf("[E3SMSpectrum] PRBBlacklistControl from dApp %u (not applied, prb_block.enabled is false): prbs=[",
                    action.dapp_identifier);
        for (std::size_t i = 0; i < ctrl.blacklisted_prbs.size(); ++i) {
            std::printf("%s%d", i > 0 ? ", " : "", ctrl.blacklisted_prbs[i]);
        }
        std::printf("]\n");
        send_ack(request_message_id, true);
        latrec_tstamp(request_message_id, LATREC_ACK_SENT,
                      static_cast<uint64_t>(libe3::ResponseCode::POSITIVE), 0);
        return libe3::ErrorCode::SUCCESS;
    }

    // Every PRB must be on the carrier. validityPeriod and samplingThreshold
    // are ignored: a block lasts until the next one replaces it.
    for (const int prb : ctrl.blacklisted_prbs) {
        if (prb < 0 || static_cast<uint32_t>(prb) >= nof_prbs_) {
            std::fprintf(stderr,
                "[E3SMSpectrum] Rejected PRB block from dApp %u: PRB %d is outside the carrier's 0..%u\n",
                action.dapp_identifier, prb, nof_prbs_ - 1);
            return reject(libe3::ErrorCode::SM_ERROR_INVALID_PARAM);
        }
    }

    if (!prb_block_->submit(request_message_id, ctrl.blacklisted_prbs, rx_ns)) {
        std::fprintf(stderr,
            "[E3SMSpectrum] Rejected PRB block from dApp %u: the prb_block codelet is not loaded, "
            "or too many blocks are waiting for the gNB\n",
            action.dapp_identifier);
        return reject(libe3::ErrorCode::INTERNAL_ERROR);
    }
    // PrbBlockRelay ACKs it once the block is in the gNB.
    return libe3::ErrorCode::SUCCESS;
}
