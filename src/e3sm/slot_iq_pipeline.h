/*
 * SlotIqPipeline - controller-side data plane for slot-level IQ.
 *
 * Sibling to IqPipeline. Both expose the same public API
 * (register_consumer, start, stop, is_running) so a service model can
 * subscribe to either pipeline without changing its callback shape;
 * the difference is what each pipeline produces:
 *
 *  - IqPipeline:    codelet ecpri_iq_samples / hook capture_xran_packet.
 *                   Per-section, BFP-9 compressed on the wire; controller
 *                   decompresses on the worker. Sample = one OFDM section.
 *
 *  - SlotIqPipeline: codelet uplink_slot_samples / hook capture_uplink_slot.
 *                   Per-slot, cbf16_t already assembled by ocudu's
 *                   upper_phy_rx_symbol_handler_impl. Controller does no
 *                   decompression, no slot accumulation - it just gets
 *                   one ready-to-publish slot per fire.
 *
 * The two pipelines coexist in e3_controller.cpp and are started
 * independently on first-subscriber transitions of the SMs that consume
 * them. Today only E3SMLayer1 (RF=2) consumes SlotIqPipeline; the older
 * IqPipeline stays available for any future per-section consumer (see
 * report/design_slot_level_hook.md for the rationale).
 */
#pragma once

#include "../../codelets/uplink_slot_samples/uplink_slot_data.h"
#include "jbpf_dispatcher.h"
#include <libe3/libe3.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "jbpf_io.h"
#include "jbpf_io_channel.h"
#include "jbpf_common.h"
}

namespace e3sm_pipeline {

/*
 * One assembled UL slot ready for downstream encoders / SHM publish.
 *
 * Buffer lifetime is the consumer callback only - the worker reuses
 * the same backing storage for the next slot once the callback returns.
 * Consumers that need to retain bytes across slots must copy them
 * (E3SMLayer1's ShmIqWriter publish covers this for us).
 */
struct SlotSample {
    /* Slot identification (parsed by the ocudu hook). */
    uint16_t sfn{0};
    uint16_t subframe_id{0};
    uint16_t slot_id{0};
    uint16_t sector_id{0};

    /* Resource grid shape for this slot. iq below holds
     *   nof_ports * nof_symbols * nof_subcarriers cbf16_t
     * = iq_size_bytes total bytes, laid out [port][symbol][subc]. */
    uint16_t nof_ports{0};
    uint16_t nof_symbols{0};
    uint16_t nof_subcarriers{0};

    /* Pointer into the worker's backing buffer. Valid for the callback
     * duration only. cbf16_t = 4 bytes per complex sample.
     *
     * NULL in `writer: gnb` mode: there the gNB-side helper has already
     * converted the slot straight into /e3_ran_buffers, so no IQ crosses the
     * jbpf ring and the descriptor below says where the row landed. */
    const uint8_t* iq{nullptr};
    uint32_t       iq_size_bytes{0};

    /* Row the gNB-side helper wrote, in `writer: gnb` mode -- the
     * (fh_buffer_index, fh_write_index) pair the Indication carries.
     *
     * Meaningless in `writer: controller` mode, where the controller writes the
     * row itself and derives these from its own ring cursor. Exactly one writer
     * may own that cursor, which is what e3config::ShmWriter enforces. */
    uint8_t  fh_buffer_index{0};
    uint32_t fh_write_index{0};

    /* RAN anchor (CLOCK_REALTIME ns) stamped by the ocudu hook caller
     * at hand-off. Same domain as jbpf_time_get_ns() and the dApp's
     * time.time_ns(), so it subtracts cleanly on both sides for true
     * RAN -> dApp end-to-end latency. */
    uint64_t gnb_ts_ns{0};

    /* Codelet-entry timestamp (CLOCK_REALTIME ns). Subtract gnb_ts_ns
     * to get the jbpf invocation overhead. Used by the SM to log the
     * gnb_to_codelet stage in statistics_layer1.log. */
    uint64_t codelet_ts_ns{0};

    /* Controller-side timestamps captured when the slot arrived at
     * the dispatcher poll. dispatch_ts_ns = CLOCK_REALTIME ns at
     * dispatcher entry (for stage timing vs. codelet_ts_ns).
     * recv_us = wall-clock microseconds at dispatcher entry (legacy,
     * kept for backwards-compatible stats consumers).
     * sample_id = monotonic count since startup. */
    uint64_t dispatch_ts_ns{0};
    uint32_t recv_us{0};
    uint32_t sample_id{0};
};

// Distinct from IqPipeline::Consumer (different sample type and the
// two typedefs would otherwise collide in the same namespace).
using SlotConsumer = std::function<void(const SlotSample&)>;

class SlotIqPipeline {
public:
    struct Config {
        /* LCM IPC socket the gnb exposes for codelet loading. Same
         * socket as IqPipeline uses (both codelets land in the same
         * gnb jbpf agent). */
        std::string lcm_socket_path{"/tmp/jbpf/jbpf_lcm_ipc"};

        /* Directory containing the uplink_slot_samples codeletset.
         * Empty disables auto-loading (useful for tests). */
        std::string codelet_base_path;

        /* CPU core to pin the worker thread on. -1 = no pinning. */
        int worker_core{-1};
    };

    SlotIqPipeline(JbpfDispatcher& dispatcher, jbpf_io_ctx* io_ctx, Config cfg);
    ~SlotIqPipeline();

    SlotIqPipeline(const SlotIqPipeline&)            = delete;
    SlotIqPipeline& operator=(const SlotIqPipeline&) = delete;

    /* Add a consumer. Callbacks fire on the worker thread, in
     * registration order. Must be called BEFORE start(). Consumers
     * cannot be removed.
     */
    void register_consumer(SlotConsumer c);

    /* Load codelets (if codelet_base_path is set), register the
     * dispatcher stream, spawn the worker thread. Idempotent.
     * Lazy-fired from E3SMLayer1::start() on first dApp subscription
     * to RF=2 - same pattern as IqPipeline. */
    libe3::ErrorCode start();

    /* Unregister dispatcher stream, join worker, unload codelets.
     * Idempotent. */
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    /* Number of slots dropped because the SPSC queue was full when
     * the dispatcher tried to enqueue. Should stay at 0 in steady
     * state - 32 slot capacity is ~80 ms of UL traffic at 30 kHz SCS. */
    uint64_t dropped_samples() const { return dropped_.load(std::memory_order_relaxed); }

private:
    /* SPSC ring capacity. At most 32 jbpf buffers can be un-released at once, so
     * the queue never needs more entries than the ring has slots. Power of 2
     * for cheap masking.  */
    static constexpr std::size_t QUEUE_CAPACITY = 32;

    struct QueueEntry {
        /* Pointer to the jbpf ring buffer (a uplink_slot_sample) the codelet
         * wrote. Option A: we enqueue the POINTER (zero-copy) instead of
         * memcpy'ing the ~733 KB slot on the poll thread. The dispatcher stream
         * is registered with defer_release=true, so the dispatcher does NOT free
         * this buffer; the worker releases it (jbpf_io_channel_release_buf) after
         * the consumer fan-out. Valid from enqueue until that release. */
        void*    buf;
        /* CLOCK_REALTIME ns at dispatcher poll entry. Used by the SM to
         * derive the codelet -> dispatcher stage cost. */
        uint64_t dispatch_ts_ns;
        uint32_t recv_us;
        uint32_t sample_id;
    };

    /* Dynamic codelet load via jbpf LCM IPC. Pulls
     * uplink_slot_samples/uplink_slot_collect.o off codelet_base_path.
     * Bool return separates "codelet missing" (warn + continue) from
     * a true error. */
    bool load_codelets();
    void unload_codelets();

    /* Dispatcher callback (registered defer_release=true): enqueues each
     * incoming slot's buffer POINTER into the SPSC queue - no memcpy, runs ~free
     * on the poll thread. On a full queue it releases the dropped buffer itself
     * (the worker won't see it); the worker releases the rest after fan-out. */
    void process_buffers(struct jbpf_io_stream_id* sid, void** bufs, int n);

    /* Worker-thread loop: drains the SPSC queue, fans out to each
     * registered consumer via the SlotSample callback. */
    void worker_loop();

    /* Build a SlotSample from one queue entry and call every consumer. */
    void dispatch_sample(const QueueEntry& entry);

    JbpfDispatcher&            dispatcher_;
    jbpf_io_ctx*               io_ctx_;
    Config                     config_;
    std::vector<SlotConsumer>  consumers_;

    std::atomic<bool>      running_{false};
    bool                   codelets_loaded_{false};
    std::thread            worker_;

    /* SPSC ring between poll thread (producer) and worker (consumer).
     * Cache-line aligned to avoid false sharing of head/tail counters. */
    alignas(64) std::atomic<uint64_t> queue_head_{0};
    alignas(64) std::atomic<uint64_t> queue_tail_{0};
    std::vector<QueueEntry>           queue_;
    std::atomic<uint64_t>             dropped_{0};
    uint32_t                          total_samples_received_{0};

    /* Stream ID matches the codelet binary's compile-time UUID -
     * keep in sync with codelets/uplink_slot_samples/uplink_slot_samples.yaml
     * (stream_id: "7531abcd1234567890fedcba0987654a"). */
    struct jbpf_io_stream_id slot_iq_stream_id_ {
        .id = {0x75, 0x31, 0xab, 0xcd, 0x12, 0x34, 0x56, 0x78,
               0x90, 0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x4a}
    };
};

}  // namespace e3sm_pipeline
