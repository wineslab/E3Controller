/*
 * E3SMSpectrum — Spectrum Service Model (RAN function ID 1).
 *
 * The grammar is src/e3sm/asn/e3sm_spectrum.asn.
 *
 * Telemetry + control:
 *   - PRB blacklist control (control id 1): decodes Spectrum-DAppControlData in
 *     the agent's encoding: APER, or in a JSON run the same fields as a JSON
 *     object, {"controlType":"prbBlacklist","controlPayload":
 *     {"prbBlacklistControl":{"blacklistedPRBs":[...]}}} (the full rules are in
 *     e3sm_spect_wrapper.h). With prb_block.enabled the block goes to the gNB's
 *     scheduler through PrbBlockRelay, which ACKs it once it is in the RAN; the
 *     scheduler keeps every data allocation off those PRBs, in every slot, UL
 *     and DL. Without it the control is decoded and ACKed but not applied.
 *   - IQ telemetry (telemetry id 1), LEGACY, only without prb_block.enabled:
 *     consumes the ecpri_iq_samples codelet via IqPipeline and emits
 *     Spectrum-IQDataIndication (APER) to subscribed dApps. Obsolete; kept
 *     until that codelet is removed.
 *   - SetupResponse RAN function descriptor (Spectrum-RanFunctionData), in the
 *     agent's encoding.
 *
 * The SM starts on the first dApp subscription and stops on the last: start()
 * loads the prb_block codelet (or boots the legacy IqPipeline), stop() unloads
 * it, which clears the block in the gNB.
 */
#ifndef E3_SM_SPECTRUM_H
#define E3_SM_SPECTRUM_H

#include "e3sm_spect_wrapper.h"
#include "e3sm/iq_pipeline.h"
#include "e3sm/prb_block_relay.h"

#include <libe3/libe3.hpp>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

class E3SMSpectrum : public libe3::ServiceModel {
public:
    static constexpr uint32_t RAN_FUNCTION_ID = 1;
    // Control ids of the Spectrum SM. Only the first is handled.
    static constexpr uint32_t CONTROL_ID_PRB_BLACKLIST = 1;

    // prb_block: the relay into the gNB's scheduler, or nullptr when
    // prb_block.enabled is false. nof_prbs: the carrier width, the bound of the
    // PRB indices a block may name.
    E3SMSpectrum(e3sm_pipeline::IqPipeline& pipeline,
                 libe3::E3Agent& agent,
                 std::string stats_log_path = "",
                 e3sm_pipeline::PrbBlockRelay* prb_block = nullptr,
                 uint32_t nof_prbs = 273)
        : pipeline_(pipeline),
          agent_(&agent),
          prb_block_(prb_block),
          nof_prbs_(nof_prbs),
          stats_log_path_(std::move(stats_log_path)) {}

    std::string name() const override { return "Spectrum Service Model"; }
    uint32_t    version() const override { return 1; }
    uint32_t    ran_function_id() const override { return RAN_FUNCTION_ID; }
    std::vector<uint32_t> telemetry_ids() const override { return {1}; }
    std::vector<uint32_t> control_ids()   const override { return {CONTROL_ID_PRB_BLACKLIST}; }

    libe3::ErrorCode init() override;
    void             destroy() override;
    libe3::ErrorCode start() override;
    void             stop() override;
    bool             is_running() const override { return running_.load(std::memory_order_acquire); }

    // Descriptive RAN function data (Spectrum-RanFunctionData), APER or JSON.
    std::vector<uint8_t> ran_function_data() const override;

    libe3::ErrorCode handle_control_action(
        uint32_t request_message_id,
        const libe3::DAppControlAction& action) override;

private:
    // IqPipeline consumer callback (worker thread): one BFP-decompressed UL
    // section. FFT zero-pads, APER-encodes a Spectrum-IQDataIndication, and
    // fans out one indication per subscriber.
    void on_sample(const e3sm_pipeline::DecompressedSample& s);

    // Sends the MessageAck of control `id` to the dApp.
    void send_ack(uint32_t id, bool ok);

    e3sm_pipeline::IqPipeline&     pipeline_;
    libe3::E3Agent*                agent_;
    e3sm_pipeline::PrbBlockRelay*  prb_block_;
    uint32_t                       nof_prbs_;
    std::atomic<bool>              running_{false};

    // Worker-thread scratch (single consumer — no synchronization needed).
    std::vector<int16_t>                padded_buf_;
    std::vector<uint8_t>                encoded_buf_;
    e3sm_spectrum::SpectrumIQIndication indication_;

    // Per-slot RAN-side stage CSV. Mirrors E3SMLayer1's statistics_layer1.log
    // schema (see on_sample() in the .cpp for column order). Disabled unless
    // constructed with a non-empty path; opened lazily on first published
    // sample so we don't create an empty file on runs that never receive one.
    std::string   stats_log_path_;
    std::ofstream stats_log_;
    uint64_t      sample_publish_seq_{0};
};

#endif /* E3_SM_SPECTRUM_H */
