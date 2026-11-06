/*
 * PrbBlockRelay — see prb_block_relay.h.
 */

#include "prb_block_relay.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "jbpf_lcm_ipc.h"
}

namespace e3sm_pipeline {

static constexpr const char* kCodeletSetName = "prb_block";

// The gNB-side ring holds one record per applied block: 2^16 is hours of them.
static constexpr unsigned kGnbRingEntriesLog2 = 16;

PrbBlockRelay::PrbBlockRelay(jbpf_io_ctx* io_ctx, JbpfDispatcher& dispatcher, Config cfg)
    : io_ctx_(io_ctx), dispatcher_(dispatcher), config_(std::move(cfg))
{
    // Registered before the codelet exists, so that its first report is not
    // lost. The dispatcher releases the buffers after the handler returns.
    dispatcher_.register_stream(applied_out_stream_id_,
                                [this](struct jbpf_io_stream_id*, void** bufs, int num_bufs) {
                                    on_applied(bufs, num_bufs);
                                });

#ifdef LIBE3_ENABLE_LATREC
    const std::string ring = std::string(kRingRole) + "_gnb";
    if (latrec_open_in(&gnb_ring_, config_.latrec_dir.empty() ? nullptr : config_.latrec_dir.c_str(),
                       ring.c_str(), kGnbRingEntriesLog2) != 1) {
        std::fprintf(stderr, "[PrbBlockRelay] WARNING: cannot open the stage ring %s; no LIVE_ON_AIR records\n",
                     ring.c_str());
    }
#endif
}

PrbBlockRelay::~PrbBlockRelay()
{
    stop();
    dispatcher_.unregister_stream(applied_out_stream_id_);
#ifdef LIBE3_ENABLE_LATREC
    latrec_close(&gnb_ring_);
#endif
}

// ---------------------------------------------------------------------------
// libe3's threads
// ---------------------------------------------------------------------------

bool PrbBlockRelay::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!load_codelet()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        codelet_loaded_ = true;
    }
    running_.store(true, std::memory_order_release);
    return true;
}

void PrbBlockRelay::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (codelet_loaded_) {
        unload_codelet();
        codelet_loaded_ = false;
    }
}

bool PrbBlockRelay::submit(uint32_t id, const std::vector<int>& prbs, uint64_t rx_ns)
{
    received_.fetch_add(1, std::memory_order_relaxed);
    if (!running_.load(std::memory_order_acquire)) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const struct e3_prb_block_msg msg = make_prb_block_msg(id, prbs, rx_ns);

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= kQueueDepth) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    queue_.push_back(msg);
    queued_.store(queue_.size(), std::memory_order_release);
    return true;
}

bool PrbBlockRelay::load_codelet()
{
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, config_.lcm_socket_path.c_str(), JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_load_req_s load_req = {};
    std::strncpy(load_req.codeletset_id.name, kCodeletSetName, JBPF_CODELETSET_NAME_LEN - 1);
    load_req.num_codelet_descriptors = 1;

    auto& desc = load_req.codelet_descriptor[0];
    std::strncpy(desc.codelet_name, "block", JBPF_CODELET_NAME_LEN - 1);
    std::strncpy(desc.hook_name, "sched_prb_block", sizeof(desc.hook_name) - 1);

    const std::string path = config_.codelet_base_path + "/prb_block/prb_block.o";
    std::strncpy(desc.codelet_path, path.c_str(), JBPF_PATH_LEN - 1);

    desc.priority          = 1;
    desc.runtime_threshold = 0;
    desc.num_linked_maps   = 0;

    // One control-input channel, the blocks, and one output channel, the slots
    // that applied them.
    desc.num_in_io_channel = 1;
    std::strncpy(desc.in_io_channel[0].name, "block_in", sizeof(desc.in_io_channel[0].name) - 1);
    std::memcpy(&desc.in_io_channel[0].stream_id, &block_in_stream_id_, sizeof(jbpf_io_stream_id_t));
    desc.in_io_channel[0].has_serde = false;
    desc.num_out_io_channel = 1;
    std::strncpy(desc.out_io_channel[0].name, "applied_out", sizeof(desc.out_io_channel[0].name) - 1);
    std::memcpy(&desc.out_io_channel[0].stream_id, &applied_out_stream_id_, sizeof(jbpf_io_stream_id_t));
    desc.out_io_channel[0].has_serde = false;

    const int rc = jbpf_lcm_ipc_send_codeletset_load_req(&address, &load_req);
    if (rc != 0) {
        std::fprintf(stderr,
                     "[PrbBlockRelay] Failed to load codeletset '%s' (rc=%d). Is the gNB running, built with the "
                     "sched_prb_block hook?\n",
                     kCodeletSetName, rc);
        return false;
    }
    std::printf("[PrbBlockRelay] Codeletset '%s' loaded: dApp PRB blocks go to the gNB's scheduler\n",
                kCodeletSetName);
    return true;
}

void PrbBlockRelay::unload_codelet()
{
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, config_.lcm_socket_path.c_str(), JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_unload_req_s unload_req = {};
    std::strncpy(unload_req.codeletset_id.name, kCodeletSetName, JBPF_CODELETSET_NAME_LEN - 1);

    const int rc = jbpf_lcm_ipc_send_codeletset_unload_req(&address, &unload_req);
    if (rc != 0) {
        std::fprintf(stderr, "[PrbBlockRelay] Failed to unload codeletset '%s' (rc=%d)\n", kCodeletSetName, rc);
    } else {
        std::printf("[PrbBlockRelay] Codeletset '%s' unloaded: no PRB block in the gNB\n", kCodeletSetName);
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void PrbBlockRelay::poll()
{
    if (queued_.load(std::memory_order_acquire) > 0) {
        // Take the whole queue at once, then send outside the lock so that
        // submit() never waits on a send.
        std::deque<struct e3_prb_block_msg> batch;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            batch.swap(queue_);
            queued_.store(0, std::memory_order_release);
        }
        if (!batch.empty()) {
            hand_off(batch);
        }
    }

    maybe_print_counters();
}

void PrbBlockRelay::hand_off(std::deque<struct e3_prb_block_msg>& batch)
{
    // Every control replaces the whole set, so only the newest goes to the gNB.
    struct e3_prb_block_msg& newest     = batch.back();  // jbpf's send takes a non-const pointer
    const uint64_t           superseded = batch.size() - 1;
    latrec_tstamp(newest.id, LATREC_DEQUEUE, superseded, 0);

    int rc = -1;
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (codelet_loaded_) {
            // Fails when the codelet's queue in the gNB is full: nothing drains it,
            // i.e. the scheduler stopped firing the hook.
            rc = jbpf_io_channel_send_msg(io_ctx_, &block_in_stream_id_, &newest, sizeof(newest));
        }
    }
    const bool ok = (rc == 0);
    latrec_tstamp(newest.id, LATREC_APPLY_CONTROL_DONE, ok ? 1 : 0, 0);

    if (ok) {
        sent_.fetch_add(1, std::memory_order_relaxed);
        superseded_.fetch_add(superseded, std::memory_order_relaxed);
    } else {
        rejected_.fetch_add(batch.size(), std::memory_order_relaxed);
    }

    // ACK them all. A superseded control was accepted: the newer one carries
    // the whole set.
    for (const struct e3_prb_block_msg& msg : batch) {
        latrec_ctx_set(msg.id);  // libe3's EMIT_ENTER of the ACK carries the id
        if (ack_) {
            ack_(msg.id, ok);
        }
        latrec_tstamp(msg.id, LATREC_ACK_SENT, ok ? 0 : 1, 0);
    }
}

void PrbBlockRelay::on_applied(void** bufs, int num_bufs)
{
    for (int i = 0; i < num_bufs; i++) {
        const auto* msg = static_cast<const struct e3_prb_block_applied*>(bufs[i]);
        if (msg == nullptr) {
            continue;
        }
        applied_.fetch_add(1, std::memory_order_relaxed);
        last_id_               = msg->id;
        last_n_prbs_           = msg->n_prbs;
        last_sfn_              = msg->sfn;
        last_slot_             = msg->slot;
        last_rx_to_applied_ns_ = msg->applied_ns > msg->ctrl_rx_ns ? msg->applied_ns - msg->ctrl_rx_ns : 0;

#ifdef LIBE3_ENABLE_LATREC
        // Back-dated to the scheduler slot; a ring's times must ascend, and a
        // second cell's slots could interleave, so enforce it.
        uint64_t t_ns = msg->applied_ns;
        if (t_ns <= gnb_floor_ns_) {
            t_ns = gnb_floor_ns_ + 1;
        }
        gnb_floor_ns_ = t_ns;
        latrec_stamp_at(&gnb_ring_, msg->id, LATREC_LIVE_ON_AIR,
                        (static_cast<uint64_t>(msg->sfn) << 16) | msg->slot, msg->coalesced, t_ns);
#endif
    }
}

void PrbBlockRelay::maybe_print_counters()
{
    const uint64_t total = received_.load(std::memory_order_relaxed) + applied_.load(std::memory_order_relaxed);
    if (total == printed_total_) {
        return;  // nothing new
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_print_ < std::chrono::seconds(1)) {
        return;
    }
    last_print_    = now;
    printed_total_ = total;

#ifdef LIBE3_ENABLE_LATREC
    latrec_heartbeat(&gnb_ring_);
#endif
    std::printf("%s\n", summary().c_str());
}

std::string PrbBlockRelay::summary() const
{
    char line[320];
    std::snprintf(line, sizeof(line),
                  "[PrbBlockRelay] PRB blocks received %lu, sent %lu, superseded %lu, rejected %lu, applied %lu"
                  " (last: id %u, %u PRBs, slot %u.%u, controller rx -> applied %.1f us)",
                  static_cast<unsigned long>(received_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(sent_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(superseded_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(rejected_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(applied_.load(std::memory_order_relaxed)), last_id_,
                  static_cast<unsigned>(last_n_prbs_), static_cast<unsigned>(last_sfn_),
                  static_cast<unsigned>(last_slot_), static_cast<double>(last_rx_to_applied_ns_) / 1000.0);
    return line;
}

}  // namespace e3sm_pipeline
