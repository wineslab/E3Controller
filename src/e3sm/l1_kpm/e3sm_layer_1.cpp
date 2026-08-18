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
#include <sstream>

/* ---------------------------------------------------------------------------
 * Drop accounting
 *
 * The point of naming every reason separately: "we dropped 4000 slots" is not
 * actionable, whereas "4000 NoSubscribers" (nobody had subscribed yet) and
 * "4000 RanPublishedNothing" (the gNB helper never wrote a row) call for
 * completely different fixes, and one of them is not even a fault.
 * ------------------------------------------------------------------------- */

const char* E3SMLayer1::drop_name(Drop d) {
    switch (d) {
    case Drop::NotRunning:          return "not_running";
    case Drop::SlotFiltered:        return "slot_filtered";
    case Drop::GeometryMismatch:    return "geometry_mismatch";
    case Drop::NoIq:                return "no_iq";
    case Drop::BlobTooSmall:        return "blob_too_small";
    case Drop::RanPublishedNothing: return "ran_published_nothing";
    case Drop::NoSubscribers:       return "no_subscribers";
    case Drop::EncodeFailed:        return "encode_failed";
    case Drop::EmitFailed:          return "emit_failed";
    case Drop::COUNT:               break;
    }
    return "unknown";
}

uint64_t E3SMLayer1::total_drops() const {
    uint64_t t = 0;
    for (std::size_t i = 0; i < kDropCount; ++i) {
        t += drops_[i].load(std::memory_order_relaxed);
    }
    return t;
}

void E3SMLayer1::note_drop(Drop d) {
    drops_[static_cast<std::size_t>(d)].fetch_add(1, std::memory_order_relaxed);
    maybe_log_drops();
}

void E3SMLayer1::maybe_log_drops() {
    /* Throttle to <=1/s. Without this, a condition that drops EVERY slot -- no
     * subscriber yet, or a geometry mismatch -- would emit a line and a CSV row
     * at slot rate (2000/s), which is both useless and a real cost on the
     * worker thread. */
    const auto now = std::chrono::steady_clock::now();
    if (drops_last_report_.time_since_epoch().count() != 0 &&
        now - drops_last_report_ < std::chrono::seconds(1)) {
        return;
    }
    drops_last_report_ = now;

    const uint64_t total = total_drops();
    if (total == drops_last_total_) {
        return;  /* nothing new since the last report */
    }
    const uint64_t since = total - drops_last_total_;
    drops_last_total_    = total;

    /* Live line, so a run that is silently dropping everything is visible
     * without waiting for shutdown or opening the CSV. */
    std::fprintf(stderr,
        "[E3SMLayer1] dropped %lu slot(s) in the last second (%lu total, %lu published)\n",
        static_cast<unsigned long>(since),
        static_cast<unsigned long>(total),
        static_cast<unsigned long>(published_slots()));

    if (stats_log_path_.empty()) {
        return;  /* counters still maintained; only the CSV is opt-in */
    }
    if (!drops_log_.is_open()) {
        /* Sibling of the stage CSV, same convention e3_controller.cpp uses for
         * the Spectrum SM's path: insert the suffix before the extension. */
        const std::size_t dot   = stats_log_path_.find_last_of('.');
        const std::size_t slash = stats_log_path_.find_last_of('/');
        const bool has_ext = (dot != std::string::npos &&
                              (slash == std::string::npos || dot > slash));
        const std::string path = has_ext
            ? stats_log_path_.substr(0, dot) + "_drops" + stats_log_path_.substr(dot)
            : stats_log_path_ + "_drops";
        drops_log_.open(path, std::ios::out | std::ios::trunc);
        if (!drops_log_.is_open()) {
            std::fprintf(stderr, "[E3SMLayer1] cannot open drop log %s\n", path.c_str());
            stats_log_path_.clear();   /* do not retry every second */
            return;
        }
        drops_log_ << "uptime_s,published,dropped_total";
        for (std::size_t i = 0; i < kDropCount; ++i) {
            drops_log_ << ',' << drop_name(static_cast<Drop>(i));
        }
        drops_log_ << '\n';
        drops_log_start_ = now;
    }

    const double uptime_s =
        std::chrono::duration<double>(now - drops_log_start_).count();
    drops_log_ << uptime_s << ',' << published_slots() << ',' << total;
    for (std::size_t i = 0; i < kDropCount; ++i) {
        drops_log_ << ',' << drops_[i].load(std::memory_order_relaxed);
    }
    drops_log_ << '\n';
    drops_log_.flush();   /* a crash mid-run must not lose the accounting */
}

std::string E3SMLayer1::drops_summary() const {
    const uint64_t total = total_drops();
    const uint64_t pub   = published_slots();

    std::ostringstream os;
    os << "[E3SMLayer1] slots published: " << pub << ", dropped: " << total;
    if (total == 0) {
        os << " (none)";
        return os.str();
    }
    /* Share of everything that reached this SM, which is the number worth
     * quoting: dropped/(published+dropped), not dropped/published. */
    const double pct = 100.0 * static_cast<double>(total) /
                       static_cast<double>(total + pub);
    os << " (" << pct << "% of arrivals)";
    for (std::size_t i = 0; i < kDropCount; ++i) {
        const uint64_t c = drops_[i].load(std::memory_order_relaxed);
        if (c != 0) {
            os << "\n    " << drop_name(static_cast<Drop>(i)) << ": " << c;
        }
    }
    return os.str();
}

libe3::ErrorCode E3SMLayer1::init() {
    if (!shm_writer_.open(shm_name_, shm_size_, geom_, cbf16_scale_, writer_mode_)) {
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
    if (!running_) { note_drop(Drop::NotRunning); return; }

    // Optional single-slot filter (debug/diagnostic): target_slot_ is
    // the absolute slot index within a radio frame (subframe_id*2 +
    // slot_id under 30 kHz SCS). -1 disables.
    if (target_slot_ >= 0) {
        // slot_id is already ocudu's slot_point::slot_index(), i.e. the index
        // within the 10 ms radio frame (0..slots_per_frame()-1), so use it
        // directly rather than recomputing from subframe_id with a hardcoded
        // 2 slots/subframe that only held at 30 kHz SCS.
        const int abs_slot = static_cast<int>(s.slot_id);
        if (abs_slot != target_slot_) { note_drop(Drop::SlotFiltered); return; }
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
        note_drop(Drop::GeometryMismatch);
        return;
    }

    // Sanity: the blob must carry every antenna port we intend to publish,
    // clamped to the SHM row's antenna capacity. Catches a grid-shape drift
    // (e.g. ocudu shipping fewer ports than nof_ports claims).
    const uint16_t ports_clamped =
        (s.nof_ports == 0) ? 1u
        : (s.nof_ports > geom_.nof_ports ? geom_.nof_ports : s.nof_ports);
    if (shm_writer_.writes_rows()) {
        /* Controller mode: the blob must carry every antenna port we intend to
         * publish. Catches a grid-shape drift (e.g. ocudu shipping fewer ports
         * than nof_ports claims). */
        const uint32_t kPerAntBytes   = geom_.cbf16_bytes_per_ant();
        const uint32_t kExpectedBytes = static_cast<uint32_t>(ports_clamped) * kPerAntBytes;
        if (s.iq == nullptr || s.iq_size_bytes == 0) {
            /* Not a grid-shape problem: zero bytes means the codelet published a
             * DESCRIPTOR, i.e. it ran the gNB-side publish path, while this
             * controller is configured to convert the IQ itself. The shipped
             * codelet only does the descriptor path, so `shm.writer: controller`
             * cannot work with it. Warn once — at slot rate this would otherwise
             * bury the log. */
            if (!writer_mode_warned_) {
                writer_mode_warned_ = true;
                std::fprintf(stderr,
                    "[E3SMLayer1] shm.writer is 'controller' but the codelet published a "
                    "descriptor with no IQ (0 bytes).\n"
                    "  The shipped uplink_slot_samples codelet writes rows via the gNB-side "
                    "helper and sends only a ~64 B descriptor,\n"
                    "  so the controller has nothing to convert. Set shm.writer: gnb in the "
                    "config.\n"
                    "  (Grid dims reported by the RAN are fine: %u ports x %u sym x %u subc.)\n",
                    s.nof_ports, s.nof_symbols, s.nof_subcarriers);
            }
            note_drop(Drop::NoIq);
            return;
        }
        if (s.iq_size_bytes < kExpectedBytes) {
            std::fprintf(stderr,
                "[E3SMLayer1] Slot blob too small: %u bytes (need >= %u). "
                "Grid shape mismatch (nof_ports=%u nof_symbols=%u nof_subc=%u)?\n",
                s.iq_size_bytes, kExpectedBytes,
                s.nof_ports, s.nof_symbols, s.nof_subcarriers);
            note_drop(Drop::BlobTooSmall);
            return;
        }
    } else {
        /* gNB mode: no IQ crosses the jbpf ring — the helper already wrote the
         * row. What can go wrong here is the helper REFUSING, which it signals
         * with TRUNCATED and bytes_written == 0 rather than writing a partial
         * row. Publishing an Indication for that would point the dApp at a row
         * nobody wrote. */
        if ((s.flags & E3_SLOT_FLAG_TRUNCATED) != 0 || s.bytes_written == 0) {
            if (!truncated_warned_) {
                truncated_warned_ = true;
                std::fprintf(stderr,
                    "[E3SMLayer1] gNB helper published nothing for this slot "
                    "(flags=0x%02x bytes_written=%u, grid %u ports x %u sym x %u subc). "
                    "Check that the region geometry matches and that the helper attached.\n",
                    s.flags, s.bytes_written, s.nof_ports, s.nof_symbols, s.nof_subcarriers);
            }
            note_drop(Drop::RanPublishedNothing);
            return;
        }
    }

    using clock = std::chrono::steady_clock;

    // CLOCK_MONOTONIC ns at on_sample entry. Same domain as
    // s.gnb_ts_ns / s.codelet_ts_ns / s.dispatch_ts_ns, so the four
    // RAN-side stage durations subtract cleanly into statistics_layer1.log.
    //
    // Was CLOCK_REALTIME; the entire chain moved together. Monotonic matches
    // latrec's clock (so stage rows and rings join without an offset) and cannot
    // be stepped by NTP mid-run, which is what the saturating subtractions below
    // were defending against.
    auto realtime_ns_now = []() -> uint64_t {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    };
    const uint64_t handler_entry_ns = realtime_ns_now();

    // Single encoding: one wire format for every subscriber.
    const libe3::EncodingFormat enc =
        agent_ ? agent_->config().encoding : libe3::EncodingFormat::ASN1;
    const auto subs = get_subscribers();
    if (subs.empty()) {
        /* Counted, but NOT a fault: before the first dApp subscribes every slot
         * lands here, so a large no_subscribers count on a healthy run is
         * normal and is exactly why the summary breaks reasons out. */
        note_drop(Drop::NoSubscribers);
        return;  // No one is listening; nothing to publish.
    }

    // Publish the slot to /e3_ran_buffers.
    //
    // In `writer: controller` mode this converts cbf16 -> fp16 inline (the dApp
    // expects fp16 on the wire). In `writer: gnb` mode the gNB-side helper has
    // already written the row in the PHY RX thread and reported which one, so we
    // must NOT write here: both writers keep their own ring cursor and two active
    // writers would silently overwrite each other. publish_row_cbf16 hard-refuses
    // in that mode; we take the indices the RAN reported instead.
    const auto t_shm_start = clock::now();
    uint8_t  fh_buf_idx   = 0;
    uint32_t fh_write_idx = 0;
    if (shm_writer_.writes_rows()) {
        shm_writer_.publish_row_cbf16(s.iq, ports_clamped, fh_buf_idx, fh_write_idx);
    } else {
        fh_buf_idx   = s.fh_buffer_index;
        fh_write_idx = s.fh_write_index;
    }
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
        note_drop(Drop::EncodeFailed);
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
            /* Per-DAPP, so with several subscribers one slot can bump this more
             * than once while still being published to the others. That makes
             * emit_failed the one counter which is not mutually exclusive with
             * a successful publish -- deliberate, since the alternative (drop
             * the slot from the count entirely) would hide a partial fan-out. */
            note_drop(Drop::EmitFailed);
        }
    }
    const auto t_emit_end = clock::now();

    // --- Per-slot statistics (disabled unless --stats-log was given) ---
    const uint64_t publish_seq =
        slot_publish_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (stats_log_path_.empty()) {
        return;
    }
    // Schema:
    //   slot_seq,
    //   gnb_to_codelet_us,     NOT "hook -> codelet entry". This is
    //                          hook -> END of the codelet, and in `writer: gnb`
    //                          mode that INCLUDES the whole data plane:
    //
    //                            gnb_ts_ns    clock_gettime(CLOCK_MONOTONIC) taken
    //                                         immediately before the hook fires, on
    //                                         the last symbol of the slot, i.e. when
    //                                         the resource grid is complete
    //                                         (upper_phy_rx_symbol_handler_impl.cpp:73)
    //                            ...          ubpf JIT dispatch + the codelet's
    //                                         bounds-check ladder
    //                            ...          jbpf_e3_publish_slot(): the bf16 -> fp16
    //                                         AVX2/F16C convert AND the full
    //                                         733,824-byte row write into
    //                                         /e3_ran_buffers
    //                            codelet_ts_ns  jbpf_time_get_ns() at the END of the
    //                                         codelet (uplink_slot_collect.c:220,
    //                                         after the publish call at :174-180)
    //
    //                          So read this column as "gNB hook -> the IQ is in
    //                          shared memory", not as jbpf invocation overhead.
    //                          Sanity check: the standalone convert benchmarks at
    //                          66.5 us for a 4-port slot against a measured p50 of
    //                          ~71 us, so the copy is ~94% of this stage and jbpf
    //                          dispatch is only the remaining ~4-5 us.
    //   codelet_to_dispatch_us,codelet -> controller dispatcher poll
    //   dispatch_to_handler_us,dispatcher -> on_sample (SPSC queue wait)
    //   shm_ns,                publish_row_cbf16 cost
    //   encode_ns,             SM payload encoder cost
    //   emit_ns,               emit_outbound fan-out (SM-side enqueue) cost
    //   nof_subc, iq_bytes     slot size (handy if BWP changes)
    //
    // The three "us" stages are derived from RAN-side CLOCK_MONOTONIC ns
    // stamps (gNB -> codelet -> dispatcher -> handler). Because that is also
    // latrec's clock, these absolute stamps and the latrec rings can be joined
    // directly -- no mono/real offset arithmetic. The post-encode ZMQ-send stage
    // lives in libe3's outbound latrec leg (L0..L3).
    if (!stats_log_.is_open()) {
        stats_log_.open(stats_log_path_, std::ios::out | std::ios::trunc);
        stats_log_ << "slot_seq,"
                      "gnb_to_codelet_us,codelet_publish_us,"
                      "codelet_to_dispatch_us,dispatch_to_handler_us,"
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
    const uint64_t publish_us =
        (s.bytes_written > 0) ? sat_us(s.codelet_ts_ns, s.codelet_entry_ts_ns) : 0ULL;
    stats_log_ << publish_seq << ','
               << sat_us(s.codelet_entry_ts_ns, s.gnb_ts_ns) << ','
               << publish_us                                 << ','
               << sat_us(s.dispatch_ts_ns, s.codelet_ts_ns)  << ','
               << sat_us(handler_entry_ns, s.dispatch_ts_ns) << ','
               << ns_between(t_shm_start,  t_shm_end)        << ','
               << encode_ns                                  << ','
               << ns_between(t_emit_start, t_emit_end)       << ','
               << s.nof_subcarriers                          << ','
               << s.iq_size_bytes                            << '\n';
    stats_log_.flush();
}
