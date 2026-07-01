/*
 * SlotIqPipeline implementation - structurally mirrors iq_pipeline.cpp
 * but with no BFP decompression and slot-granularity per fire. See
 * slot_iq_pipeline.h for the architectural rationale and the contract
 * with the uplink_slot_samples codelet.
 */

#include "slot_iq_pipeline.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

extern "C" {
#include "jbpf_lcm_ipc.h"
#include "jbpf_mem_mgmt.h"
}

namespace e3sm_pipeline {

SlotIqPipeline::SlotIqPipeline(JbpfDispatcher& dispatcher,
                               jbpf_io_ctx*    io_ctx,
                               Config          cfg)
    : dispatcher_(dispatcher),
      io_ctx_(io_ctx),
      config_(std::move(cfg)),
      queue_(QUEUE_CAPACITY)
{}

SlotIqPipeline::~SlotIqPipeline() {
    stop();
}

void SlotIqPipeline::register_consumer(SlotConsumer c) {
    consumers_.push_back(std::move(c));
}

bool SlotIqPipeline::load_codelets() {
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, config_.lcm_socket_path.c_str(),
                 JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_load_req_s load_req = {};
    std::strncpy(load_req.codeletset_id.name, "uplink_slot_samples",
                 JBPF_CODELETSET_NAME_LEN - 1);

    load_req.num_codelet_descriptors = 1;
    auto& desc = load_req.codelet_descriptor[0];

    std::strncpy(desc.codelet_name, "collector", JBPF_CODELET_NAME_LEN - 1);
    std::strncpy(desc.hook_name, "capture_uplink_slot",
                 sizeof(desc.hook_name) - 1);

    const std::string full_path =
        config_.codelet_base_path + "/uplink_slot_samples/uplink_slot_collect.o";
    std::strncpy(desc.codelet_path, full_path.c_str(), JBPF_PATH_LEN - 1);

    desc.priority           = 1;
    desc.runtime_threshold  = 0;
    desc.num_in_io_channel  = 0;   /* no control input channel - codelet
                                    * has no runtime config map; every UL
                                    * slot fires a complete output. */
    desc.num_linked_maps    = 0;

    desc.num_out_io_channel = 1;
    std::strncpy(desc.out_io_channel[0].name, "output_map",
                 sizeof(desc.out_io_channel[0].name) - 1);
    std::memcpy(&desc.out_io_channel[0].stream_id, &slot_iq_stream_id_,
                sizeof(jbpf_io_stream_id_t));
    desc.out_io_channel[0].has_serde = false;

    int rc = jbpf_lcm_ipc_send_codeletset_load_req(&address, &load_req);
    if (rc != 0) {
        std::fprintf(stderr,
            "[SlotIqPipeline] Failed to load codeletset 'uplink_slot_samples' "
            "via LCM IPC (rc=%d)\n", rc);
        return false;
    }
    std::printf("[SlotIqPipeline] Codeletset 'uplink_slot_samples' loaded successfully\n");
    return true;
}

void SlotIqPipeline::unload_codelets() {
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, config_.lcm_socket_path.c_str(),
                 JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_unload_req_s unload_req = {};
    std::strncpy(unload_req.codeletset_id.name, "uplink_slot_samples",
                 JBPF_CODELETSET_NAME_LEN - 1);

    int rc = jbpf_lcm_ipc_send_codeletset_unload_req(&address, &unload_req);
    if (rc != 0) {
        std::fprintf(stderr,
            "[SlotIqPipeline] Failed to unload codeletset 'uplink_slot_samples' (rc=%d)\n",
            rc);
    } else {
        std::printf("[SlotIqPipeline] Codeletset 'uplink_slot_samples' unloaded successfully\n");
    }
}

libe3::ErrorCode SlotIqPipeline::start() {
    if (running_.load(std::memory_order_acquire)) {
        return libe3::ErrorCode::SUCCESS;
    }

    if (!config_.codelet_base_path.empty()) {
        if (!load_codelets()) {
            return libe3::ErrorCode::INTERNAL_ERROR;
        }
        codelets_loaded_ = true;
    }

    running_.store(true, std::memory_order_release);

    worker_ = std::thread([this] { worker_loop(); });
    if (config_.worker_core >= 0) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(config_.worker_core, &mask);
        if (pthread_setaffinity_np(worker_.native_handle(), sizeof(mask), &mask) != 0) {
            std::fprintf(stderr,
                "[SlotIqPipeline] WARNING: failed to pin worker thread to core %d (%s)\n",
                config_.worker_core, std::strerror(errno));
        } else {
            std::printf("[SlotIqPipeline] Worker thread pinned to core %d\n",
                        config_.worker_core);
        }
    }

    /* Register the dispatcher stream LAST so we only start receiving
     * once the worker is alive. */
    dispatcher_.register_stream(
        slot_iq_stream_id_,
        [this](struct jbpf_io_stream_id* sid, void** bufs, int n) {
            process_buffers(sid, bufs, n);
        },
        /*defer_release=*/true);   // Option A: we own the buffers; the worker
                                   // releases each after the consumer fan-out.

    return libe3::ErrorCode::SUCCESS;
}

void SlotIqPipeline::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    dispatcher_.unregister_stream(slot_iq_stream_id_);

    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }

    /* The worker may have exited (running_=false) with buffers still queued.
     * We own them (defer_release=true), so release any it didn't reach BEFORE
     * unload_codelets() tears down the channel/mempool below. The worker has
     * joined, so this is the only thread touching the queue now. */
    {
        const uint64_t head = queue_head_.load(std::memory_order_relaxed);
        uint64_t       tail = queue_tail_.load(std::memory_order_relaxed);
        for (; tail != head; ++tail) {
            jbpf_io_channel_release_buf(queue_[tail & (QUEUE_CAPACITY - 1)].buf);
        }
        queue_tail_.store(head, std::memory_order_relaxed);
    }

    const uint64_t dropped = dropped_.load(std::memory_order_relaxed);
    if (dropped > 0) {
        std::printf("[SlotIqPipeline] Dropped %lu slots due to full SPSC queue\n",
                    static_cast<unsigned long>(dropped));
    }

    if (codelets_loaded_) {
        unload_codelets();
        codelets_loaded_ = false;
    }
}

void SlotIqPipeline::process_buffers(struct jbpf_io_stream_id* /*stream_id*/,
                                     void** bufs, int num_bufs) {
    /* CLOCK_REALTIME ns at poll entry. Same clock domain as the
     * codelet's timestamps so we can subtract directly. One read per
     * batch - the dispatcher polls in tight bursts and per-buf clock
     * reads aren't worth the syscall cost. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t dispatch_ts_ns =
        static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
        static_cast<uint64_t>(ts.tv_nsec);
    const uint32_t now_us = static_cast<uint32_t>(
        (dispatch_ts_ns / 1000ULL) & 0x7FFFFFFFULL);

    for (int i = 0; i < num_bufs; i++) {
        auto* sample = static_cast<struct uplink_slot_sample*>(bufs[i]);

        /* SPSC enqueue. Single producer here (this thread), single
         * consumer (the worker). Head is relaxed; tail load uses
         * acquire so the worker's progress is visible. */
        const uint64_t head = queue_head_.load(std::memory_order_relaxed);
        const uint64_t tail = queue_tail_.load(std::memory_order_acquire);
        if (head - tail >= QUEUE_CAPACITY) {
            /* Queue full: the worker will never see this buffer, so release it
             * here ourselves (we own it - defer_release=true) to avoid leaking
             * the jbpf ring slot. */
            dropped_.fetch_add(1, std::memory_order_relaxed);
            jbpf_io_channel_release_buf(bufs[i]);
            continue;
        }

        /* Option A: enqueue the buffer POINTER (zero-copy) - no 733 KB memcpy.
         * The worker reads the slot straight from the jbpf ring and releases it
         * after the consumer fan-out. */
        QueueEntry& slot = queue_[head & (QUEUE_CAPACITY - 1)];
        slot.buf = bufs[i];

        /* recv_us = wall-clock delta between codelet entry and dispatcher
         * poll. Captures codelet -> controller poll lag. Saturating
         * subtraction to keep the value positive. */
        const uint32_t codelet_us = static_cast<uint32_t>(
            (sample->codelet_ts_ns / 1000ULL) & 0x7FFFFFFFULL);
        slot.recv_us        = (codelet_us > 0) ? ((now_us - codelet_us) & 0x7FFFFFFFu) : 0;
        slot.dispatch_ts_ns = dispatch_ts_ns;
        slot.sample_id      = ++total_samples_received_;

        queue_head_.store(head + 1, std::memory_order_release);
    }
    /* We own the buffers (defer_release=true): enqueued ones are released by the
     * worker after fan-out, dropped ones were released above. So the dispatcher
     * must NOT release here - and it won't (defer_release). */
}

void SlotIqPipeline::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        const uint64_t tail = queue_tail_.load(std::memory_order_relaxed);
        const uint64_t head = queue_head_.load(std::memory_order_acquire);
        if (head == tail) {
#if defined(__x86_64__) || defined(__i386__)
            _mm_pause();
#endif
            continue;
        }

        QueueEntry& entry = queue_[tail & (QUEUE_CAPACITY - 1)];
        dispatch_sample(entry);
        /* Consumers are synchronous (dispatch_sample fans out and returns once
         * they have all run), so the jbpf buffer is safe to release now. We own
         * it (defer_release=true) - release exactly once per enqueued slot. */
        jbpf_io_channel_release_buf(entry.buf);

        queue_tail_.store(tail + 1, std::memory_order_release);
    }
}

void SlotIqPipeline::dispatch_sample(const QueueEntry& entry) {
    /* entry.buf points straight at the jbpf ring buffer (Option A, zero-copy).
     * Valid until the worker releases it right after this fan-out returns. */
    const auto& s = *static_cast<const struct uplink_slot_sample*>(entry.buf);

    SlotSample sample;
    sample.sfn             = s.sfn;
    sample.subframe_id     = s.subframe_id;
    sample.slot_id         = s.slot_id;
    sample.sector_id       = s.sector_id;
    sample.nof_ports       = s.nof_ports;
    sample.nof_symbols     = s.nof_symbols;
    sample.nof_subcarriers = s.nof_subcarriers;
    sample.iq              = s.iq;
    sample.iq_size_bytes   = s.iq_size_bytes;
    sample.gnb_ts_ns       = s.gnb_ts_ns;
    sample.codelet_ts_ns   = s.codelet_ts_ns;
    sample.dispatch_ts_ns  = entry.dispatch_ts_ns;
    sample.recv_us         = entry.recv_us;
    sample.sample_id       = entry.sample_id;

    /* Fan out by const-ref. Each consumer must finish its callback
     * before the worker overwrites the queue slot on the next
     * iteration. */
    for (auto& c : consumers_) {
        c(sample);
    }
}

}  // namespace e3sm_pipeline
