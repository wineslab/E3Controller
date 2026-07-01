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
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

class E3SMLayer1 : public libe3::ServiceModel {
public:
    static constexpr uint32_t RAN_FUNCTION_ID = 2;

    E3SMLayer1(e3sm_pipeline::SlotIqPipeline& pipeline,
               libe3::E3Agent& agent,
               std::string shm_name = "/e3_ran_buffers",
               std::size_t shm_size = static_cast<std::size_t>(1) << 30,
               int target_slot = -1,
               std::string stats_log_path = "")
        : pipeline_(pipeline),
          agent_(&agent),
          shm_name_(std::move(shm_name)),
          shm_size_(shm_size),
          target_slot_(target_slot),
          stats_log_path_(std::move(stats_log_path))
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

private:
    /* SlotIqPipeline consumer callback (worker thread). Receives one
     * fully-assembled UL slot from ocudu's resource grid. */
    void on_sample(const e3sm_pipeline::SlotSample& s);

    e3sm_pipeline::SlotIqPipeline& pipeline_;
    libe3::E3Agent*                agent_;

    std::string shm_name_;
    std::size_t shm_size_;
    /* Optional single-slot filter; absolute slot within a 10 ms frame
     * (subframe_id*2 + slot_id at 30 kHz SCS). -1 disables. */
    int         target_slot_;
    /* Path for the per-slot stage CSV. Empty disables stats logging. */
    std::string stats_log_path_;

    e3sm_spectrum::ShmIqWriter shm_writer_;
    bool                       running_{false};

    /* Per-slot stats. slot_publish_seq_ increments for every slot
     * we publish; counts indications emitted to dApps. stats_log_ is
     * opened lazily on the first published slot when stats_log_path_
     * is non-empty. */
    uint64_t      slot_publish_seq_{0};
    std::ofstream stats_log_;

    /* Encoded indication payload (one encoding per the agent config).
     * Capacity grows on first use, reused across slots. */
    std::vector<uint8_t> encoded_buf_;
};

#endif /* E3_SM_LAYER1_H */
