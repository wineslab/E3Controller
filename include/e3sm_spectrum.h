#ifndef E3_SM_SPECTRUM_H
#define E3_SM_SPECTRUM_H

#include "ecpri_iq_data.h"
#include <libe3/libe3.hpp>
#include <iostream>
#include <cstring>
#include "e3sm/e3sm_spect_wrapper.h"
#include "jbpf_dispatcher.h"

extern "C" {
#include "jbpf_io.h"
#include "jbpf_io_channel.h"
#include "jbpf_common.h"
#include "jbpf_mem_mgmt.h"
}


class E3SMSpectrum : public libe3::ServiceModel {
public:
    E3SMSpectrum(JbpfDispatcher& dispatcher) : dispatcher_(dispatcher) {};
    static constexpr uint32_t RAN_FUNCTION_ID = 1;

    std::string name() const override { return "Spectrum Service Model"; }
    uint32_t version() const override { return 1; }

    uint32_t ran_function_id() const override {
        return RAN_FUNCTION_ID;
    }

    std::vector<uint32_t> telemetry_ids() const override {
        return {1};
    }

    std::vector<uint32_t> control_ids() const override {
        return {1};
    }

    libe3::ErrorCode init() override { return libe3::ErrorCode::SUCCESS; }

    void destroy() override {
        stop();
    }

    libe3::ErrorCode start() override;
    
    std::vector<uint8_t> ran_function_data() const override;
    
    void stop() override;

    bool is_running() const override { return running_; }

    libe3::ErrorCode handle_control_action(uint32_t request_message_id, const libe3::DAppControlAction& action) override;

   

private:
    uint32_t seq_{0};
    int total_samples_received_{0};
    std::atomic<bool> running_{false};

    // Dispatcher reference — shared across all SMs
    JbpfDispatcher& dispatcher_;

    // Stream ID for the eCPRI I/Q codelet output
    struct jbpf_io_stream_id ecpri_iq_stream_id_ = {
        .id = {0xF1, 0xF2, 0x29, 0x0F, 0xD2, 0x68, 0x5D, 0x17,
               0xEC, 0xC9, 0x90, 0x2C, 0x4C, 0x9F, 0x05, 0xFE}
    };

    // Process buffers routed by the dispatcher
    void process_buffers(struct jbpf_io_stream_id* stream_id, void** bufs, int num_bufs);
};

#endif /* E3_SM_SPECTRUM_H */