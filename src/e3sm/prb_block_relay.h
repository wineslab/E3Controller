/*
 * PrbBlockRelay — carries a dApp's PRB block into the gNB's scheduler.
 *
 * A dApp sends a Spectrum SM PRB blacklist control (RAN function 1, control 1):
 * the PRBs where it detected an incumbent. OAI's gNB keeps every data
 * allocation off them, in every slot, UL and DL, until the next control
 * replaces the list. On OCUDU the E3 agent is this process, so the block has to
 * cross into the gNB, through jbpf:
 *
 *   1. E3SMSpectrum decodes the control on libe3's inbound thread and calls
 *      submit(), which queues it.
 *   2. The main loop calls poll(), which sends the newest queued block to the
 *      prb_block codelet as control input and then ACKs every queued control.
 *      jbpf requires the thread that owns the io context for the send, which is
 *      why it happens there and not in step 1. The ACK means what OAI's does:
 *      the block is in the RAN, not yet applied.
 *   3. ocudu's scheduler fires the codelet's hook at the start of every slot.
 *      The codelet hands it the block in force, and the scheduler reports the
 *      blocked PRBs as used to every data allocator. The first slot that applies
 *      a new block sends it back on applied_out.
 *
 * Each control replaces the whole set, so only the newest one queued is sent;
 * the others are ACKed too, as superseded. The codelet is loaded on the first
 * RAN function 1 subscription and unloaded on the last; with no codelet the
 * scheduler's hook returns "no block", so unloading clears the block, as OAI
 * clears it when its Spectrum SM stops.
 *
 * Stage records (libe3 latrec, seq = the control's E3 message id):
 *   ring e3controller.prb_block, the main loop:
 *     DEQUEUE (aux = superseded), APPLY_CONTROL_DONE (handed to the gNB,
 *     aux = ok), ACK_SENT (aux = response code)
 *   ring e3controller.prb_block_gnb, the gNB's time:
 *     LIVE_ON_AIR at the codelet's time in the first scheduler slot that applies
 *     the block, aux = sfn << 16 | slot, aux2 = controls coalesced into it
 *
 * The message formats are in codelets/include/jbpf_e3_prb_block_api.h, shared
 * with ocudu.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <libe3/latrec.h>

extern "C" {
#include "jbpf_io.h"
#include "jbpf_io_channel.h"
#include "jbpf_common.h"
}

#include "../../codelets/include/jbpf_e3_prb_block_api.h"
#include "jbpf_dispatcher.h"
#include "prb_block_msg.h"

namespace e3sm_pipeline {

class PrbBlockRelay {
public:
    struct Config {
        std::string lcm_socket_path;    // jbpf LCM socket of the gNB
        std::string codelet_base_path;  // the codelets/ directory
        std::string latrec_dir;         // stage records; empty = latrec's default
    };

    // Ring of the main loop's stages. The main thread must open it before it
    // first emits an ACK, or libe3 opens one under its own name.
    static constexpr const char* kRingRole = "e3controller.prb_block";

    // Sends the dApp the ACK of control `id`: true = handed to the gNB.
    using AckHandler = std::function<void(uint32_t id, bool ok)>;

    PrbBlockRelay(jbpf_io_ctx* io_ctx, JbpfDispatcher& dispatcher, Config cfg);
    ~PrbBlockRelay();

    PrbBlockRelay(const PrbBlockRelay&)            = delete;
    PrbBlockRelay& operator=(const PrbBlockRelay&) = delete;

    // E3SMSpectrum, before the agent starts.
    void set_ack_handler(AckHandler handler) { ack_ = std::move(handler); }

    // E3SMSpectrum::start()/stop(), on libe3's thread. start() loads the
    // codelet; stop() unloads it, which clears the block in the gNB.
    bool start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // E3SMSpectrum, libe3's inbound thread. Queues the block: control `id`,
    // carrier PRB indices, received at rx_ns (CLOCK_MONOTONIC). False when it
    // cannot be handed to the gNB (no codelet, queue full): NACK it.
    bool submit(uint32_t id, const std::vector<int>& prbs, uint64_t rx_ns);

    // Main loop, the thread that owns io_ctx.
    void poll();

    // One line with the counters, for the shutdown summary.
    std::string summary() const;

private:
    bool load_codelet();
    void unload_codelet();

    // Main loop: sends the newest block of `batch` and ACKs all of them.
    void hand_off(std::deque<struct e3_prb_block_msg>& batch);
    // Main loop (the dispatcher): the codelet's report of an applied block.
    void on_applied(void** bufs, int num_bufs);
    void maybe_print_counters();

    jbpf_io_ctx*    io_ctx_;
    JbpfDispatcher& dispatcher_;
    Config          config_;
    AckHandler      ack_;

    // Filled by submit(), drained by poll(). `queued_` mirrors its size so that
    // poll(), which runs in a busy loop, skips the lock when it is empty.
    static constexpr std::size_t           kQueueDepth = 64;
    std::mutex                             queue_mutex_;
    std::deque<struct e3_prb_block_msg>    queue_;
    std::atomic<std::size_t>               queued_{0};

    // A send and the unload never overlap: poll() sends and stop() unloads
    // under this lock, on different threads.
    std::mutex        io_mutex_;
    bool              codelet_loaded_ = false;  // under io_mutex_
    std::atomic<bool> running_{false};

    // Counters, cumulative.
    std::atomic<uint64_t> received_{0};    // controls submitted
    std::atomic<uint64_t> sent_{0};        // blocks handed to the gNB
    std::atomic<uint64_t> superseded_{0};  // replaced in our queue before the send
    std::atomic<uint64_t> rejected_{0};    // NACKed: no codelet, queue full, or the send failed
    std::atomic<uint64_t> applied_{0};     // reports from the codelet

    // on_applied() only, read by the counters line on the same thread.
    uint32_t last_id_       = 0;
    uint16_t last_n_prbs_   = 0;
    uint16_t last_sfn_      = 0;
    uint8_t  last_slot_     = 0;
    uint64_t last_rx_to_applied_ns_ = 0;

    // poll() only: the counters line is printed at most once a second.
    std::chrono::steady_clock::time_point last_print_{};
    uint64_t                              printed_total_ = 0;

#ifdef LIBE3_ENABLE_LATREC
    // The gNB-side stamps, written by the main loop at the codelet's time. A
    // ring of their own because they are back-dated, and a ring's times must
    // ascend; the codelet's reports arrive in slot order.
    latrec_t gnb_ring_{};
    uint64_t gnb_floor_ns_ = 0;
#endif

    // Must match codelets/prb_block/prb_block.yaml.
    struct jbpf_io_stream_id block_in_stream_id_ {
        0x75, 0x31, 0xab, 0xcd, 0x12, 0x34, 0x56, 0x78,
        0x90, 0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x7a
    };
    struct jbpf_io_stream_id applied_out_stream_id_ {
        0x75, 0x31, 0xab, 0xcd, 0x12, 0x34, 0x56, 0x78,
        0x90, 0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x7b
    };
};

}  // namespace e3sm_pipeline
