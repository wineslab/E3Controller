/*
 * DAppReportRelay — see dapp_report_relay.h.
 */

#include "dapp_report_relay.h"

#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" {
#include "jbpf_lcm_ipc.h"
}

namespace e3sm_pipeline {

// After a failed codelet load, the next report tries again only this much later,
// so a gNB that is down is not asked once per report.
static constexpr std::chrono::seconds kLoadRetry{2};

static constexpr const char* kCodeletSetName = "dapp_report_relay";

// CLOCK_MONOTONIC in ns, the clock of e3_dapp_report_msg.received_ts_ns.
static uint64_t monotonic_now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

DAppReportRelay::DAppReportRelay(jbpf_io_ctx* io_ctx, JbpfDispatcher& dispatcher, Config cfg)
    : io_ctx_(io_ctx), dispatcher_(dispatcher), config_(std::move(cfg))
{
    // Registered before the codelet exists, so that its first message is not
    // lost. The dispatcher releases the buffers after the handler returns.
    dispatcher_.register_stream(presence_out_stream_id_,
                                [this](struct jbpf_io_stream_id*, void** bufs, int num_bufs) {
                                    on_presence(bufs, num_bufs);
                                });
}

DAppReportRelay::~DAppReportRelay()
{
    stop();
}

// ---------------------------------------------------------------------------
// libe3's report worker thread
// ---------------------------------------------------------------------------

void DAppReportRelay::on_report(const libe3::DAppReport& report)
{
    received_.fetch_add(1, std::memory_order_relaxed);

    if (!first_report_seen_) {
        first_report_seen_ = true;
        std::printf("[DAppReportRelay] first report: dApp %u, RAN function %u, sequence %u, %zu bytes\n",
                    report.dapp_identifier, report.ran_function_identifier, report.sequence_id,
                    report.report_data.size());
    }

    if (report.report_data.empty() || report.report_data.size() > E3_DAPP_REPORT_MAX) {
        too_big_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // The codelet is loaded on the first report rather than at startup: the
    // controller starts before the gNB, and a deployment without dApp reports
    // never needs it.
    if (!codelet_loaded_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        const bool tried_recently = last_load_attempt_.time_since_epoch().count() != 0 &&
                                    now - last_load_attempt_ < kLoadRetry;
        if (tried_recently) {
            no_codelet_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        last_load_attempt_ = now;
        if (!load_codelet()) {
            no_codelet_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        codelet_loaded_.store(true, std::memory_order_release);
    }

    // The codelet is loaded even without an xApp: it is what tells us when one
    // subscribes. Until then the report goes nowhere.
    if (!presence_.present(monotonic_now_ns())) {
        no_xapp_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    struct e3_dapp_report_msg msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.version         = E3_DAPP_REPORT_VERSION;
    msg.data_len        = static_cast<uint16_t>(report.report_data.size());
    msg.dapp_id         = report.dapp_identifier;
    msg.ran_function_id = report.ran_function_identifier;
    msg.sequence_id     = report.sequence_id;
    msg.received_ts_ns  = monotonic_now_ns();
    std::memcpy(msg.data, report.report_data.data(), report.report_data.size());

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= config_.queue_depth) {
        queue_full_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue_.push_back(msg);
    queued_.store(queue_.size(), std::memory_order_release);
}

bool DAppReportRelay::load_codelet()
{
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, config_.lcm_socket_path.c_str(), JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_load_req_s load_req = {};
    std::strncpy(load_req.codeletset_id.name, kCodeletSetName, JBPF_CODELETSET_NAME_LEN - 1);
    load_req.num_codelet_descriptors = 1;

    auto& desc = load_req.codelet_descriptor[0];
    std::strncpy(desc.codelet_name, "relay", JBPF_CODELET_NAME_LEN - 1);
    std::strncpy(desc.hook_name, "e3_dapp_report_poll", sizeof(desc.hook_name) - 1);

    const std::string path = config_.codelet_base_path + "/dapp_report_relay/dapp_report_relay.o";
    std::strncpy(desc.codelet_path, path.c_str(), JBPF_PATH_LEN - 1);

    desc.priority          = 1;
    desc.runtime_threshold = 0;
    desc.num_linked_maps   = 0;

    // One control-input channel, the reports, and one output channel, the
    // presence messages.
    desc.num_in_io_channel = 1;
    std::strncpy(desc.in_io_channel[0].name, "report_in", sizeof(desc.in_io_channel[0].name) - 1);
    std::memcpy(&desc.in_io_channel[0].stream_id, &report_in_stream_id_, sizeof(jbpf_io_stream_id_t));
    desc.in_io_channel[0].has_serde = false;
    desc.num_out_io_channel = 1;
    std::strncpy(desc.out_io_channel[0].name, "presence_out", sizeof(desc.out_io_channel[0].name) - 1);
    std::memcpy(&desc.out_io_channel[0].stream_id, &presence_out_stream_id_, sizeof(jbpf_io_stream_id_t));
    desc.out_io_channel[0].has_serde = false;

    const int rc = jbpf_lcm_ipc_send_codeletset_load_req(&address, &load_req);
    if (rc != 0) {
        std::fprintf(stderr,
                     "[DAppReportRelay] Failed to load codeletset '%s' (rc=%d). Is the gNB running, built "
                     "with the E2SM-DAPP bridge? Retrying on a later report.\n",
                     kCodeletSetName, rc);
        return false;
    }
    std::printf("[DAppReportRelay] Codeletset '%s' loaded; dApp reports go to the gNB while an xApp is "
                "subscribed\n",
                kCodeletSetName);
    return true;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void DAppReportRelay::poll()
{
    const uint64_t now_ns = monotonic_now_ns();
    log_presence_change(now_ns);

    if (queued_.load(std::memory_order_acquire) > 0) {
        // Take the whole queue at once, then send outside the lock so that
        // on_report() never waits on a send.
        std::deque<struct e3_dapp_report_msg> batch;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            batch.swap(queue_);
            queued_.store(0, std::memory_order_release);
        }

        const bool xapp_present = presence_.present(now_ns);
        for (struct e3_dapp_report_msg& msg : batch) {
            // The xApp may have gone while the report waited here.
            if (!xapp_present) {
                no_xapp_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Fails when the codelet's queue in the gNB is full: the bridge
            // stopped draining it before the presence messages said so.
            const int rc = jbpf_io_channel_send_msg(io_ctx_, &report_in_stream_id_, &msg, sizeof(msg));
            if (rc == 0) {
                forwarded_.fetch_add(1, std::memory_order_relaxed);
            } else {
                send_failed_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    maybe_print_counters();
}

void DAppReportRelay::on_presence(void** bufs, int num_bufs)
{
    const uint64_t now_ns = monotonic_now_ns();
    for (int i = 0; i < num_bufs; i++) {
        const auto* msg = static_cast<const struct e3_dapp_presence_msg*>(bufs[i]);
        if (msg == nullptr || msg->version != E3_DAPP_PRESENCE_VERSION) {
            continue;
        }
        presence_.on_message(msg->nof_subscriptions, now_ns);
    }
}

void DAppReportRelay::log_presence_change(uint64_t now_ns)
{
    const bool present = presence_.present(now_ns);
    if (present == forwarding_) {
        return;
    }
    forwarding_ = present;
    if (present) {
        std::printf("[DAppReportRelay] xApp subscribed (%u data subscription(s) in the gNB): forwarding dApp "
                    "reports\n",
                    presence_.subscriptions());
    } else {
        std::printf("[DAppReportRelay] no xApp subscribed any more: dropping dApp reports\n");
    }
}

void DAppReportRelay::maybe_print_counters()
{
    const uint64_t total = received_.load(std::memory_order_relaxed);
    if (total == printed_total_) {
        return;  // nothing new
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_print_ < std::chrono::seconds(1)) {
        return;
    }
    last_print_    = now;
    printed_total_ = total;

    std::printf("%s\n", summary().c_str());
}

std::string DAppReportRelay::summary() const
{
    char line[288];
    std::snprintf(line, sizeof(line),
                  "[DAppReportRelay] reports received %lu, forwarded %lu, dropped: no_xapp %lu, too_big %lu, "
                  "no_codelet %lu, queue_full %lu, send_failed %lu",
                  static_cast<unsigned long>(received_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(forwarded_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(no_xapp_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(too_big_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(no_codelet_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(queue_full_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(send_failed_.load(std::memory_order_relaxed)));
    return line;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void DAppReportRelay::stop()
{
    dispatcher_.unregister_stream(presence_out_stream_id_);
    if (!codelet_loaded_.exchange(false)) {
        return;
    }
    unload_codelet();
}

void DAppReportRelay::unload_codelet()
{
    jbpf_lcm_ipc_address_t address = {};
    std::strncpy(address.path, config_.lcm_socket_path.c_str(), JBPF_LCM_IPC_ADDRESS_LEN - 1);

    jbpf_codeletset_unload_req_s unload_req = {};
    std::strncpy(unload_req.codeletset_id.name, kCodeletSetName, JBPF_CODELETSET_NAME_LEN - 1);

    const int rc = jbpf_lcm_ipc_send_codeletset_unload_req(&address, &unload_req);
    if (rc != 0) {
        std::fprintf(stderr, "[DAppReportRelay] Failed to unload codeletset '%s' (rc=%d)\n", kCodeletSetName, rc);
    } else {
        std::printf("[DAppReportRelay] Codeletset '%s' unloaded\n", kCodeletSetName);
    }
}

}  // namespace e3sm_pipeline
