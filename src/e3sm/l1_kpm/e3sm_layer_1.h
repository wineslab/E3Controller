/*
 * E3SMLayer1 - L1-KPM Service Model.
 *
 * RAN Function ID 2. Telemetry-only: emits one indication per UL slot
 * carrying a pointer (shm_name, fh_buffer_index, fh_write_index) into
 * the /e3_ran_buffers POSIX SHM ring that this SM writes via
 * ShmIqWriter.
 *
 * Single-encoding: libe3 serves exactly one wire encoding (chosen by the
 * controller's --encoding flag and read back from E3Agent::config()).
 * Each slot is encoded once into that format (JSON or APER) and fanned
 * out to every subscriber - encode cost is O(1) per slot, fan-out cost
 * is O(subscribers) for the small pointer PDU.
 *
 * Data plane is owned by SlotIqPipeline, not by this SM. We register
 * a consumer callback at init() time and receive one fully-assembled
 * UL slot per fire on the pipeline's worker thread - no per-symbol
 * accumulation here.
 */
#ifndef E3_SM_LAYER1_H
#define E3_SM_LAYER1_H

#include "e3sm/utils/e3sm_shm_writer.h"
#include "e3sm/slot_iq_pipeline.h"

#include <libe3/libe3.hpp>

#include <atomic>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

class E3SMLayer1 : public libe3::ServiceModel {
public:
    static constexpr uint32_t RAN_FUNCTION_ID = 2;

    E3SMLayer1(e3sm_pipeline::SlotIqPipeline& pipeline,
               libe3::E3Agent& agent,
               const e3config::ControllerConfig& cfg)
        : pipeline_(pipeline),
          agent_(&agent),
          shm_name_(cfg.shm.name),
          shm_size_(cfg.shm.size_bytes),
          target_slot_(cfg.target_slot),
          drops_log_path_(cfg.logging.drops_log_path),
          geom_(cfg.radio),
          cbf16_scale_(cfg.shm.cbf16_scale),
          writer_mode_(cfg.shm.writer)
    {}

    std::string name() const override { return "L1 KPM Service Model"; }
    uint32_t    version() const override { return 1; }
    uint32_t    ran_function_id() const override { return RAN_FUNCTION_ID; }
    std::vector<uint32_t> telemetry_ids() const override { return {1}; }
    std::vector<uint32_t> control_ids()   const override { return {}; }

    libe3::ErrorCode init() override;
    void             destroy() override;
    libe3::ErrorCode start() override;
    void             stop() override;
    bool             is_running() const override { return running_; }

    /* Descriptive RAN function data, encoded in the agent's configured
     * encoding (E3Agent::config().encoding) - JSON or APER. */
    std::vector<uint8_t> ran_function_data() const override;

    libe3::ErrorCode handle_control_action(
        uint32_t request_message_id,
        const libe3::DAppControlAction& action) override;

    enum class Drop : uint8_t {
        NotRunning = 0,      /* stopped, or a straggler after stop() */
        SlotFiltered,        /* target_slot filter excluded it (deliberate)   */
        GeometryMismatch,    /* config vs RAN disagreement; latched refusal   */
        NoIq,                /* controller mode, but codelet sent descriptor  */
        BlobTooSmall,        /* short blob; a partial row would be wrong data */
        RanPublishedNothing, /* gNB helper flagged TRUNCATED / wrote 0 bytes  */
        NoSubscribers,       /* nobody subscribed to RF=2 yet                 */
        EncodeFailed,        /* APER/JSON encode error                        */
        EmitFailed,          /* libe3 rejected the outbound enqueue           */
        COUNT
    };
    static constexpr std::size_t kDropCount = static_cast<std::size_t>(Drop::COUNT);
    static const char*           drop_name(Drop d);

    /* Relaxed loads: on_sample writes these on the worker thread while the
     * summary reads them from main.*/
    uint64_t drop_count(Drop d) const {
        return drops_[static_cast<std::size_t>(d)].load(std::memory_order_relaxed);
    }
    uint64_t total_drops() const;
    uint64_t published_slots() const {
        return slot_publish_seq_.load(std::memory_order_relaxed);
    }

    /* Multi-line human summary: published, total dropped, and each non-zero
     * reason. Returns the "nothing dropped" line when all counters are zero,
     * so the caller can print it unconditionally. */
    std::string drops_summary() const;

private:
    /* Bump a reason. Also drives the throttled live log line. */
    void note_drop(Drop d);

    /* Append a cumulative row to drops_log_path_, at most once a second.
     * Cumulative rather than per-interval so a row is meaningful on its own and
     * a missed flush cannot lose events. No-op when the path is empty.
     *
     * This is the only file this SM writes. It is aggregate and throttled, so
     * unlike the per-slot stage CSV it replaced it does no work on the slot
     * path -- stage timing is latrec's job now, see l1_kpm_trace.h. */
    void maybe_log_drops();

    /* SlotIqPipeline consumer callback (worker thread). Receives one
     * fully-assembled UL slot from ocudu's resource grid. */
    void on_sample(const e3sm_pipeline::SlotSample& s);

    e3sm_pipeline::SlotIqPipeline& pipeline_;
    libe3::E3Agent*                agent_;

    std::string shm_name_;
    std::size_t shm_size_;
    /* Optional single-slot filter; slot index within a 10 ms frame, i.e. the
     * value ocudu's slot_point::slot_index() returns (0..19 at 30 kHz SCS).
     * -1 disables. Superseded by workstream F's codelet-side slot_mask, which
     * makes the same decision before any data moves. */
    int         target_slot_;
    /* Path for the throttled drop-accounting CSV. Empty disables it. */
    std::string drops_log_path_;

    e3sm_spectrum::ShmIqWriter shm_writer_;
    bool                       running_{false};

    /* Row geometry from the YAML config; replaces the old constexpr in
     * e3sm_shm_writer.h. Treated as a bootstrap and checked against the RAN on
     * the first slot -- see the validate_against_ran() call in on_sample. */
    e3config::RadioGeometry geom_;
    float                   cbf16_scale_{1.0f};

    /* Who converts and writes the fp16 rows. Exactly one process may; see
     * e3config::ShmWriter. */
    e3config::ShmWriter writer_mode_{e3config::ShmWriter::Controller};

    /* One-shot geometry check. geometry_ok_ latches false on mismatch so we
     * refuse to publish rather than emit rows the dApp would misread. */
    bool geometry_checked_{false};
    bool geometry_ok_{true};

    /* One-shot warning when the gNB helper reports it published nothing, so a
     * persistent misconfiguration does not flood the log at slot rate. */
    bool truncated_warned_{false};

    /* One-shot: controller mode configured but the codelet only sends descriptors. */
    bool writer_mode_warned_{false};

    /* Increments for every slot that reaches the traced region, and keys that
     * slot's stage records across the whole E3 path including libe3's own -- it
     * is published to the library with latrec_ctx_set() before emitting and
     * comes back as the outbound leg's origin_seq. Only meaningful within this
     * ring; every producer numbers from 1.
     *
     * Atomic because the shutdown summary reads it from the main thread while
     * on_sample increments it on the worker. */
    std::atomic<uint64_t> slot_publish_seq_{0};

    /* Drop accounting -- see enum Drop. */
    std::atomic<uint64_t> drops_[kDropCount]{};
    std::ofstream         drops_log_;
    /* Throttles both the live stderr line and the CSV row to <=1/s, so a
     * pathological run cannot turn drop reporting into the bottleneck. */
    std::chrono::steady_clock::time_point drops_last_report_{};
    uint64_t                              drops_last_total_{0};
    /* Set when the drop CSV is opened, so uptime_s starts at 0 in the file. */
    std::chrono::steady_clock::time_point drops_log_start_{};

    /* Encoded indication payload (one encoding per the agent config).
     * Capacity grows on first use, reused across slots. */
    std::vector<uint8_t> encoded_buf_;
};

#endif /* E3_SM_LAYER1_H */
