/*
 * IqPipeline — shared data plane between SMs that consume codelet IQ samples.
 *
 * Owns the parts of the old monolithic E3SMSpectrum that have nothing to do
 * with wire encoding: codelet load/unload via LCM IPC, dispatcher stream
 * registration, the producer-consumer SPSC queue, the worker thread, and
 * BFP-9 decompression. SMs register a callback via register_consumer() to
 * receive each decompressed UL section.
 *
 * The pipeline serves N service-model consumers from a SINGLE worker, by
 * calling each consumer's callback in registration order on the same
 * DecompressedSample (passed by const reference — no copies). It does NOT
 * serve N dApps; that's the SM's job (each SM fans out to its subscribers
 * via emit_outbound). See the README's Known Limitations section for the
 * multi-consumer / multi-dApp explanation.
 *
 * Lifecycle: construct → register all consumers → start() → … → stop().
 * register_consumer() while running is undefined; consumers can't be
 * removed (matches the current dispatcher contract — SMs are created at
 * startup and live for the process lifetime).
 */
#pragma once

#include "ecpri_iq_data.h"
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

// One BFP-decompressed UL section, ready for downstream encoders.
//
// Buffer lifetime is the callback only — the next sample reuses the same
// internal storage. Consumers that need to retain the bytes (e.g. for slot
// accumulation across multiple symbols) must copy into their own buffer
// inside the callback.
struct DecompressedSample {
    // Pointer to the per-section metadata the codelet produced. Fields used
    // downstream: frame_id, subframe_id, slot_id, symbol_id, start_prbu,
    // num_prbu, direction, payload_size, timestamp.
    const struct iq_sample_data* raw{nullptr};

    // I/Q int16 pairs, length = num_prbu * 12 (subcarriers) * 2 (I, Q).
    const int16_t* decompressed_iq{nullptr};
    std::size_t    decompressed_size{0};

    // Controller-side timestamps captured at codelet→pipeline arrival.
    uint32_t recv_us{0};
    uint32_t sample_id{0};

    // RAN-side stage timestamps (CLOCK_REALTIME ns) forwarded verbatim
    // from the codelet's iq_sample_data. Consumers subtract these to
    // report the same three-stage schema as the SlotIqPipeline
    // (gnb_to_codelet_us / codelet_to_dispatch_us / dispatch_to_handler_us).
    //   gnb_ts_ns      — stamped at hook fire in ofh_message_receiver_impl
    //   codelet_ts_ns  — stamped by the codelet right before ringbuf output
    //   dispatch_ts_ns — stamped by IqPipeline::process_buffers on the
    //                    dispatcher poll thread (batch-shared)
    uint64_t gnb_ts_ns{0};
    uint64_t codelet_ts_ns{0};
    uint64_t dispatch_ts_ns{0};

    // BFP decompression duration in nanoseconds — used by SMs to log
    // per-slot data-plane cost in their statistics_*.log files.
    uint64_t decompress_ns{0};
};

using Consumer = std::function<void(const DecompressedSample&)>;

class IqPipeline {
public:
    struct Config {
        // LCM IPC socket the gNB exposes for codelet loading.
        std::string lcm_socket_path{"/tmp/jbpf/jbpf_lcm_ipc"};
        // Directory containing the ecpri_iq_samples codeletset .o files.
        // Empty disables auto-loading (useful for tests / pre-loaded codelets).
        std::string codelet_base_path;
        // PRB-filter config pushed to the codelet on first sample arrival.
        // 0 = no filter; sections with num_prbu != this are dropped on the
        // codelet side, before they hit our SPSC queue.
        uint16_t expected_num_prbu{106};
        // CPU core to pin the worker thread on. -1 = no pinning.
        int worker_core{-1};
    };

    IqPipeline(JbpfDispatcher& dispatcher, jbpf_io_ctx* io_ctx, Config cfg);
    ~IqPipeline();

    IqPipeline(const IqPipeline&) = delete;
    IqPipeline& operator=(const IqPipeline&) = delete;

    // Add a consumer. Callbacks fire on the worker thread, in registration
    // order. Must be called BEFORE start(). Consumers cannot be removed.
    void register_consumer(Consumer c);

    // Load codelets (if codelet_base_path is set), register the dispatcher
    // stream, spawn the worker thread. Idempotent.
    libe3::ErrorCode start();

    // Unregister dispatcher stream, join worker, unload codelets. Idempotent.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // Number of samples dropped by the SPSC queue since startup (full ring).
    // Mostly useful for stats logging by consumers.
    uint64_t dropped_samples() const { return dropped_.load(std::memory_order_relaxed); }

private:
    // At most the codelet's jbpf ring depth (64, see ecpri_iq_collect.c
    // `jbpf_ringbuf_map(output_map, ..., 64)`) can be un-released at once,
    // so a matching queue never needs more entries. Power of 2 for cheap
    // masking; larger than the ring is dead weight.
    static constexpr std::size_t QUEUE_CAPACITY = 64;

    struct QueueEntry {
        // Pointer to the jbpf ring buffer (an iq_sample_data) the codelet
        // wrote. Option A: enqueue the POINTER (zero-copy) instead of
        // memcpy'ing the ~8 KB iq_sample_data on the poll thread. The
        // dispatcher stream is registered with defer_release=true, so the
        // dispatcher does NOT free this buffer; the worker releases it
        // (jbpf_io_channel_release_buf) after the consumer fan-out.
        // Valid from enqueue until that release.
        void*    buf;
        // Dispatcher-thread stamp (CLOCK_REALTIME ns), shared across the
        // whole poll batch. Consumers subtract sample.codelet_ts_ns from
        // this to get codelet_to_dispatch_us, matching SlotIqPipeline.
        uint64_t dispatch_ts_ns;
        uint32_t recv_us;
        uint32_t sample_id;
    };

    // Dynamic codelet load via jbpf LCM IPC. Pulls
    // ecpri_iq_samples/ecpri_iq_collect.o off codelet_base_path. Bool return
    // separates "codelet missing" (warn + continue) from a true error.
    bool load_codelets();
    void unload_codelets();

    // Dispatcher callback: filters direction/num_prbu and enqueues into the
    // SPSC ring. Runs on the dispatcher's poll thread — must stay cheap.
    void process_buffers(struct jbpf_io_stream_id* sid, void** bufs, int n);

    // Worker-thread loop: drains the SPSC queue and dispatches each entry.
    void worker_loop();

    // Decompress + fan out to all registered consumers.
    void dispatch_sample(const QueueEntry& entry);

    JbpfDispatcher& dispatcher_;
    jbpf_io_ctx* io_ctx_;
    Config config_;
    std::vector<Consumer> consumers_;

    std::atomic<bool> running_{false};
    bool codelets_loaded_{false};
    bool prb_config_sent_{false};
    std::thread worker_;

    // SPSC ring between poll thread (producer) and worker thread (consumer).
    // Cache-line aligned to avoid false sharing.
    alignas(64) std::atomic<uint64_t> queue_head_{0};
    alignas(64) std::atomic<uint64_t> queue_tail_{0};
    std::vector<QueueEntry> queue_;
    std::atomic<uint64_t> dropped_{0};
    uint32_t total_samples_received_{0};

    // Worker-thread-only scratch space for BFP decompression. Sized for the
    // largest plausible BWP (273 PRBs * 12 SCs * 2 = 6552 int16) on first use.
    std::vector<int16_t> decompressed_buf_;

    // Stream IDs match the codelet binary's compile-time UUIDs — keep in
    // sync with codelets/ecpri_iq_samples/*.c.
    struct jbpf_io_stream_id ecpri_iq_stream_id_ {
        .id = {0xF1, 0xF2, 0x29, 0x0F, 0xD2, 0x68, 0x5D, 0x17,
               0xEC, 0xC9, 0x90, 0x2C, 0x4C, 0x9F, 0x05, 0xFE}
    };
    struct jbpf_io_stream_id prb_config_stream_id_ {
        .id = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
               0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90}
    };
};

}  // namespace e3sm_pipeline
