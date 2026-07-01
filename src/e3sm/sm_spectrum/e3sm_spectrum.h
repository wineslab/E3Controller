/*
 * E3SMSpectrum — Spectrum Service Model (RAN function ID 1).
 *
 * Telemetry + control:
 *   - IQ telemetry (telemetry id 1): consumes the ecpri_iq_samples codelet via
 *     IqPipeline and emits Spectrum-IQDataIndication (APER) to subscribed dApps
 *     — this is what the public Spectrum Sharing dApp subscribes to.
 *   - PRB blacklist control (control id 1): decodes Spectrum-PRBBlacklistControl
 *     and ACKs (application to the RAN is still a stub — see README).
 *   - SetupResponse RAN function descriptor (Spectrum-RanFunctionData).
 *
 * IQ-consuming SM pattern (mirrors E3SMLayer1): register a pipeline consumer at
 * init(), then start/stop the pipeline lazily on first/last dApp subscription.
 * The IqPipeline owns codelet load, the SPSC queue, the worker thread, and
 * BFP-9 decompression; this SM only FFT zero-pads, APER-encodes, and fans out.
 *
 * NOTE: handle_control_action currently ACKs and logs but does not apply the
 * blacklist to the RAN. See E3Controller/README.md "Known Limitations".
 */
#ifndef E3_SM_SPECTRUM_H
#define E3_SM_SPECTRUM_H

#include "e3sm_spect_wrapper.h"
#include "e3sm/iq_pipeline.h"

#include <libe3/libe3.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class E3SMSpectrum : public libe3::ServiceModel {
public:
    static constexpr uint32_t RAN_FUNCTION_ID = 1;

    E3SMSpectrum(e3sm_pipeline::IqPipeline& pipeline, libe3::E3Agent& agent)
        : pipeline_(pipeline), agent_(&agent) {}

    std::string name() const override { return "Spectrum Service Model"; }
    uint32_t    version() const override { return 1; }
    uint32_t    ran_function_id() const override { return RAN_FUNCTION_ID; }
    std::vector<uint32_t> telemetry_ids() const override { return {1}; }
    std::vector<uint32_t> control_ids()   const override { return {1}; }

    libe3::ErrorCode init() override;
    void             destroy() override;
    libe3::ErrorCode start() override;
    void             stop() override;
    bool             is_running() const override { return running_.load(std::memory_order_acquire); }

    // Descriptive RAN function data (Spectrum-RanFunctionData, APER).
    std::vector<uint8_t> ran_function_data() const override;

    libe3::ErrorCode handle_control_action(
        uint32_t request_message_id,
        const libe3::DAppControlAction& action) override;

private:
    // IqPipeline consumer callback (worker thread): one BFP-decompressed UL
    // section. FFT zero-pads, APER-encodes a Spectrum-IQDataIndication, and
    // fans out one indication per subscriber.
    void on_sample(const e3sm_pipeline::DecompressedSample& s);

    e3sm_pipeline::IqPipeline& pipeline_;
    libe3::E3Agent*            agent_;
    std::atomic<bool>          running_{false};

    // Worker-thread scratch (single consumer — no synchronization needed).
    std::vector<int16_t>                padded_buf_;
    std::vector<uint8_t>                encoded_buf_;
    e3sm_spectrum::SpectrumIQIndication indication_;
};

#endif /* E3_SM_SPECTRUM_H */
