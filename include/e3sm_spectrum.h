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
#include "jbpf_lcm_ipc.h"
}


class E3SMSpectrum : public libe3::ServiceModel {
public:
    E3SMSpectrum(JbpfDispatcher& dispatcher, jbpf_io_ctx* io_ctx,
                 uint16_t expected_num_prbu = 106,
                 std::string lcm_socket_path = "/tmp/jbpf/jbpf_lcm_ipc",
                 std::string codelet_base_path = "")
        : dispatcher_(dispatcher), io_ctx_(io_ctx),
          expected_num_prbu_(expected_num_prbu),
          lcm_socket_path_(std::move(lcm_socket_path)),
          codelet_base_path_(std::move(codelet_base_path)) {};
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
    bool codelets_loaded_{false};
    bool prb_config_sent_{false};

    // Dispatcher reference — shared across all SMs
    JbpfDispatcher& dispatcher_;

    // jbpf IO context — needed for sending control input to codelets
    jbpf_io_ctx* io_ctx_;

    // Expected number of PRBs per symbol (used to filter out PRACH/SRS/control symbols)
    uint16_t expected_num_prbu_;

    // LCM IPC configuration for dynamic codelet loading
    std::string lcm_socket_path_;
    std::string codelet_base_path_;

    // Stream ID for the eCPRI I/Q codelet output
    struct jbpf_io_stream_id ecpri_iq_stream_id_ = {
        .id = {0xF1, 0xF2, 0x29, 0x0F, 0xD2, 0x68, 0x5D, 0x17,
               0xEC, 0xC9, 0x90, 0x2C, 0x4C, 0x9F, 0x05, 0xFE}
    };

    // Stream ID for the PRB config control input channel
    struct jbpf_io_stream_id prb_config_stream_id_ = {
        .id = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
               0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90}
    };

    // Dynamic codelet loading/unloading via jbpf LCM IPC
    bool load_codelets();
    void unload_codelets();

    // Process buffers routed by the dispatcher
    void process_buffers(struct jbpf_io_stream_id* stream_id, void** bufs, int num_bufs);
};

#endif /* E3_SM_SPECTRUM_H */