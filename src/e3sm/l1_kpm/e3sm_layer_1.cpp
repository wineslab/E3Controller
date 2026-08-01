/*
 * E3SMLayer1 - L1-KPM Service Model (RAN function ID 2).
 *
 * Slot-level data plane: the SlotIqPipeline hands us one fully-assembled
 * UL slot per fire (cbf16_t IQ, already laid out [sym][subc]). We
 * publish the slot to /e3_ran_buffers via ShmIqWriter, encode one
 * L1KPM-Indication PDU in the agent's configured wire encoding (JSON or
 * APER), and fan out to every subscriber. No per-symbol accumulation,
 * no dedup, no BFP - everything that used to live here is upstream of
 * the codelet now.
 */

#include "e3sm_layer_1.h"

#include "e3sm_layer1_wrapper.h"
#include "e3sm_layer1_json.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>

libe3::ErrorCode E3SMLayer1::init() {
    if (!shm_writer_.open(shm_name_, shm_size_, geom_, cbf16_scale_)) {
        return libe3::ErrorCode::INTERNAL_ERROR;
    }

    // Register the pipeline consumer ONCE, at SM registration time. The
    // SlotIqPipeline keeps consumers across start/stop cycles (codelet
    // load and worker thread are what start/stop manage). Doing it here
    // rather than in start() also lets us defer start() to first dApp
    // subscription without losing the consumer wiring.
    pipeline_.register_consumer(
        [this](const e3sm_pipeline::SlotSample& s) {
            on_sample(s);
        });

    return libe3::ErrorCode::SUCCESS;
}

void E3SMLayer1::destroy() {
    stop();
    shm_writer_.close();
}

libe3::ErrorCode E3SMLayer1::start() {
    if (running_) {
        return libe3::ErrorCode::SUCCESS;
    }

    // Reserve encoded-buffer capacity once. JSON typically lands around
    // 100 bytes for a slot indication; APER smaller still. 1 KiB is
    // generous.
    encoded_buf_.reserve(1024);

    // Lazy pipeline boot: codelet load via LCM IPC + worker thread spawn
    // only happen here. libe3 calls start() on first L1-KPM subscription,
    // so the LCM IPC socket (exposed by srsRAN) is guaranteed to be up
    // by then in the standard launch order (controller → srsRAN → dApp).
    auto rc = pipeline_.start();
    if (rc != libe3::ErrorCode::SUCCESS) {
        std::fprintf(stderr,
            "[E3SMLayer1] SlotIqPipeline start failed (%s) - is srsRAN running?\n",
            libe3::error_code_to_string(rc));
        return rc;
    }

    running_ = true;
    std::printf("[E3SMLayer1] Started - slot pipeline codelet loaded, worker running\n");
    return libe3::ErrorCode::SUCCESS;
}

void E3SMLayer1::stop() {
    if (!running_) return;
    running_ = false;
    // Tear down the data plane on last unsubscribe. Consumer
    // registration survives (vector entry persists) so a subsequent
    // start() reloads codelets and the same callback fires again.
    pipeline_.stop();
}

libe3::ErrorCode E3SMLayer1::handle_control_action(
    uint32_t request_message_id,
    const libe3::DAppControlAction& action)
{
    // No controls defined for L1-KPM today. NACK any incoming control -
    // a dApp shouldn't be sending one against RF=2 in the first place.
    auto nack = make_message_ack_pdu(request_message_id,
                                     libe3::ResponseCode::NEGATIVE);
    emit_outbound(std::move(nack));
    std::fprintf(stderr,
        "[E3SMLayer1] Rejected unexpected control action from dApp %u\n",
        action.dapp_identifier);
    return libe3::ErrorCode::SM_ERROR_INVALID_PARAM;
}

std::vector<uint8_t> E3SMLayer1::ran_function_data() const {
    // Single encoding: encode in the agent's configured wire format.
    const libe3::EncodingFormat enc =
        agent_ ? agent_->config().encoding : libe3::EncodingFormat::ASN1;

    std::vector<uint8_t> encoded;
    bool ok = (enc == libe3::EncodingFormat::JSON)
                ? e3sm_layer1::encode_ran_function_data_json(encoded)
                : e3sm_layer1::encode_ran_function_data_aper(encoded);
    if (!ok) {
        std::fprintf(stderr, "[E3SMLayer1] Failed to encode RAN function data\n");
        return {};
    }
    return encoded;
}

void E3SMLayer1::on_sample(const e3sm_pipeline::SlotSample& s) {
    if (!running_) return;

    // Optional single-slot filter (debug/diagnostic): target_slot_ is
    // the absolute slot index within a radio frame (subframe_id*2 +
    // slot_id under 30 kHz SCS). -1 disables.
    if (target_slot_ >= 0) {
        // slot_id is already ocudu's slot_point::slot_index(), i.e. the index
        // within the 10 ms radio frame (0..slots_per_frame()-1), so use it
        // directly rather than recomputing from subframe_id with a hardcoded
        // 2 slots/subframe that only held at 30 kHz SCS.
        const int abs_slot = static_cast<int>(s.slot_id);
        if (abs_slot != target_slot_) return;
    }

    // Validate the CONFIGURED geometry against what the RAN actually reports,
    // once, on the first slot.
    if (!geometry_checked_) {
        geometry_checked_ = true;
        std::string gerr;
        if (!e3config::validate_against_ran(geom_, s.nof_ports, s.nof_symbols, s.nof_subcarriers, gerr)) {
            geometry_ok_ = false;
            std::fprintf(stderr,
                "[E3SMLayer1] GEOMETRY MISMATCH — refusing to publish.\n"
                "  configured: %s\n"
                "  RAN slot:   %u ports x %u sym x %u subc\n"
                "  %s\n",
                geom_.describe().c_str(), s.nof_ports, s.nof_symbols, s.nof_subcarriers, gerr.c_str());
        } else {
            std::printf("[E3SMLayer1] geometry confirmed against the RAN: %s\n", geom_.describe().c_str());
        }
    }
    if (!geometry_ok_) {
        return;
    }

    // Sanity: the blob must carry every antenna port we intend to publish,
    // clamped to the SHM row's antenna capacity. Catches a grid-shape drift
    // (e.g. ocudu shipping fewer ports than nof_ports claims).
    const uint16_t ports_clamped =
        (s.nof_ports == 0) ? 1u
        : (s.nof_ports > geom_.nof_ports ? geom_.nof_ports : s.nof_ports);
    const uint32_t kPerAntBytes = geom_.cbf16_bytes_per_ant();
    const uint32_t kExpectedBytes = static_cast<uint32_t>(ports_clamped) * kPerAntBytes;
    if (s.iq == nullptr || s.iq_size_bytes < kExpectedBytes) {
        std::fprintf(stderr,
            "[E3SMLayer1] Slot blob too small: %u bytes (need >= %u). "
            "Grid shape mismatch (nof_ports=%u nof_symbols=%u nof_subc=%u)?\n",
            s.iq_size_bytes, kExpectedBytes,
            s.nof_ports, s.nof_symbols, s.nof_subcarriers);
        return;
    }

    using clock = std::chrono::steady_clock;

    // CLOCK_REALTIME ns at on_sample entry. Same domain as
    // s.gnb_ts_ns / s.codelet_ts_ns / s.dispatch_ts_ns, so the four
    // RAN-side stage durations subtract cleanly into statistics_layer1.log.
    auto realtime_ns_now = []() -> uint64_t {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    };
    const uint64_t handler_entry_ns = realtime_ns_now();

    // Single encoding: one wire format for every subscriber.
    const libe3::EncodingFormat enc =
        agent_ ? agent_->config().encoding : libe3::EncodingFormat::ASN1;
    const auto subs = get_subscribers();
    if (subs.empty()) {
        return;  // No one is listening; nothing to publish.
    }

    // Publish the slot to /e3_ran_buffers. publish_row_cbf16 does the
    // bf16 → fp16 conversion inline (the dApp expects fp16 on wire).
    const auto t_shm_start = clock::now();
    uint8_t  fh_buf_idx   = 0;
    uint32_t fh_write_idx = 0;
    shm_writer_.publish_row_cbf16(s.iq, ports_clamped, fh_buf_idx, fh_write_idx);
    const auto t_shm_end = clock::now();

    // Absolute slot within a 10 ms frame (0..19 at 30 kHz SCS).
    //
    // SlotIqPipeline forwards ocudu's slot_point::slot_index() directly
    // through as SlotSample.slot_id, and slot_index() already returns
    // slot-within-frame (the value the dApp wants). So we use it as-is,
    // unlike the legacy IqPipeline path where the codelet sourced
    // slot_id from the eCPRI radio-app header (slot-within-subframe,
    // 0..1) and the SM had to combine it with subframe_id.
    //
    // Double-counting bug guard: if you ever pull this back to
    // subframe_id*2 + slot_id, abs_slot for slot 9 becomes 17 and slot
    // 19 becomes 37 (both invalid).
    const uint16_t abs_slot = s.slot_id;

    // Encode the slot once in the configured wire format. Encoder is
    // O(small); we do it once per slot regardless of subscriber count.
    const bool want_json = (enc == libe3::EncodingFormat::JSON);
    const auto t_encode_start = clock::now();
    encoded_buf_.clear();
    const bool encoded_ok = want_json
        ? e3sm_layer1::encode_iq_indication_json(
              s.gnb_ts_ns, s.sfn, abs_slot, shm_name_,
              fh_buf_idx, fh_write_idx, ports_clamped, encoded_buf_)
        : e3sm_layer1::encode_iq_indication_aper(
              s.gnb_ts_ns, s.sfn, abs_slot, shm_name_,
              fh_buf_idx, fh_write_idx, ports_clamped, encoded_buf_);
    const uint64_t encode_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock::now() - t_encode_start).count();

    if (!encoded_ok) {
        std::fprintf(stderr,
            "[E3SMLayer1] Failed to %s-encode indication (buf=%u row=%u)\n",
            want_json ? "JSON" : "APER", fh_buf_idx, fh_write_idx);
        return;
    }

    // Fan out one indication per subscriber. emit_ns here captures the
    // SM-side enqueue cost (PDU build + emit_outbound); the encode + ZMQ
    // send stages happen downstream in libe3's RAN outbound loop.
    const auto t_emit_start = clock::now();
    for (uint32_t dapp_id : subs) {
        libe3::Pdu pdu = make_indication_pdu(dapp_id, RAN_FUNCTION_ID,
                                             encoded_buf_);
        auto rc = emit_outbound(std::move(pdu));
        if (rc != libe3::ErrorCode::SUCCESS) {
            std::fprintf(stderr,
                "[E3SMLayer1] Failed to send indication to dApp %u: %s\n",
                dapp_id, libe3::error_code_to_string(rc));
        }
    }
    const auto t_emit_end = clock::now();

    // --- Per-slot statistics (disabled unless --stats-log was given) ---
    ++slot_publish_seq_;
    if (stats_log_path_.empty()) {
        return;
    }
    // Schema:
    //   slot_seq,
    //   gnb_to_codelet_us,     ocudu hook -> codelet entry (jbpf invocation)
    //   codelet_to_dispatch_us,codelet -> controller dispatcher poll
    //   dispatch_to_handler_us,dispatcher -> on_sample (SPSC queue wait)
    //   shm_ns,                publish_row_cbf16 cost
    //   encode_ns,             SM payload encoder cost
    //   emit_ns,               emit_outbound fan-out (SM-side enqueue) cost
    //   nof_subc, iq_bytes     slot size (handy if BWP changes)
    //
    // The three "us" stages are derived from RAN-side CLOCK_REALTIME ns
    // stamps (gNB -> codelet -> dispatcher -> handler). The post-encode
    // ZMQ-send stage lives in libe3's publisher-stage CSV, joinable by
    // message_id.
    if (!stats_log_.is_open()) {
        stats_log_.open(stats_log_path_, std::ios::out | std::ios::trunc);
        stats_log_ << "slot_seq,"
                      "gnb_to_codelet_us,codelet_to_dispatch_us,dispatch_to_handler_us,"
                      "shm_ns,encode_ns,emit_ns,nof_subc,iq_bytes\n";
    }
    auto ns_between = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
    };
    // Saturating subtractions: the three RAN-side stages are derived
    // from absolute realtime stamps. In the (rare) clock-warp case
    // they could go negative — clamp to 0 so the CSV row stays parseable.
    auto sat_us = [](uint64_t lhs, uint64_t rhs) -> uint64_t {
        return (lhs > rhs) ? ((lhs - rhs) / 1000ULL) : 0ULL;
    };
    stats_log_ << slot_publish_seq_ << ','
               << sat_us(s.codelet_ts_ns,  s.gnb_ts_ns)      << ','
               << sat_us(s.dispatch_ts_ns, s.codelet_ts_ns)  << ','
               << sat_us(handler_entry_ns, s.dispatch_ts_ns) << ','
               << ns_between(t_shm_start,  t_shm_end)        << ','
               << encode_ns                                  << ','
               << ns_between(t_emit_start, t_emit_end)       << ','
               << s.nof_subcarriers                          << ','
               << s.iq_size_bytes                            << '\n';
    stats_log_.flush();
}
