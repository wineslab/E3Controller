/*
 * DAppReportRelay — passes dApp reports on to the gNB, for an xApp.
 *
 * A dApp sends E3 DAppReports, for example the PRBs where it detected an
 * incumbent. OAI's gNB hands such a report to its E2 agent, which sends it to
 * an xApp as an E2SM-DAPP indication. On OCUDU the E3 agent is this process,
 * so the report has to cross into the gNB first. jbpf is the only way in:
 *
 *   1. libe3 delivers the report to on_report(), on its report worker thread.
 *      The relay copies it into a queue. On the first report it also loads the
 *      codelet dapp_report_relay into the gNB.
 *   2. The main loop calls poll(), which sends the queued reports to that
 *      codelet as control input. jbpf requires the thread that owns the io
 *      context for that, which is why the sending happens there and not in
 *      step 1.
 *   3. In the gNB, the E2SM-DAPP bridge fires the codelet's hook every 0.5 ms
 *      while an xApp is subscribed; the codelet moves the reports into the
 *      bridge, which has them sent to the xApp right away.
 *
 * Nothing in the gNB reads the codelet's queue while no xApp is subscribed, so
 * the relay must not fill it. The codelet tells it how many xApps are
 * subscribed (XAppPresence), and while there are none the relay drops the
 * reports itself, counted as no_xapp. The dApp does not know about xApps and
 * keeps reporting, as it does with OAI.
 *
 * The report is not decoded on the way. The message format is in
 * codelets/include/jbpf_e3_dapp_api.h, shared with ocudu.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <libe3/libe3.hpp>

extern "C" {
#include "jbpf_io.h"
#include "jbpf_io_channel.h"
#include "jbpf_common.h"
}

#include "../../codelets/include/jbpf_e3_dapp_api.h"
#include "jbpf_dispatcher.h"
#include "xapp_presence.h"

namespace e3sm_pipeline {

class DAppReportRelay {
public:
    struct Config {
        std::string lcm_socket_path;    // jbpf LCM socket of the gNB
        std::string codelet_base_path;  // the codelets/ directory
        std::size_t queue_depth = 256;  // reports waiting for poll()
    };

    /// Registers the codelet's presence stream with the dispatcher.
    DAppReportRelay(jbpf_io_ctx* io_ctx, JbpfDispatcher& dispatcher, Config cfg);
    ~DAppReportRelay();

    DAppReportRelay(const DAppReportRelay&)            = delete;
    DAppReportRelay& operator=(const DAppReportRelay&) = delete;

    // libe3's report worker thread. Queues the report.
    void on_report(const libe3::DAppReport& report);

    // Main loop, the thread that owns io_ctx. Sends the queued reports.
    void poll();

    // Unloads the codelet and unregisters the presence stream. Main thread,
    // after the agent has stopped, so that on_report() is not called any more.
    void stop();

    // One line with the counters, for the shutdown summary.
    std::string summary() const;

private:
    bool load_codelet();
    void unload_codelet();
    void maybe_print_counters();

    // Main loop (the dispatcher): a presence message from the codelet.
    void on_presence(void** bufs, int num_bufs);
    // Main loop: logs when forwarding starts or stops.
    void log_presence_change(uint64_t now_ns);

    jbpf_io_ctx*    io_ctx_;
    JbpfDispatcher& dispatcher_;
    Config          config_;

    XAppPresence presence_;
    bool         forwarding_ = false;  // main loop only, for log_presence_change()

    // Filled by on_report(), drained by poll(). `queued_` mirrors its size so
    // that poll(), which runs in a busy loop, can skip the lock when it is empty.
    std::mutex                           queue_mutex_;
    std::deque<struct e3_dapp_report_msg> queue_;
    std::atomic<std::size_t>             queued_{0};

    // Set by on_report() once the codelet is in the gNB; read by poll() and stop().
    std::atomic<bool> codelet_loaded_{false};

    // on_report() only. A failed load is retried at most every kLoadRetry.
    std::chrono::steady_clock::time_point last_load_attempt_{};
    bool                                  first_report_seen_ = false;

    // Counters, cumulative. Each report ends up in exactly one of the last six.
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> forwarded_{0};     // sent to the codelet
    std::atomic<uint64_t> no_xapp_{0};       // no xApp subscribed: nobody would read it
    std::atomic<uint64_t> too_big_{0};       // reportData larger than E3_DAPP_REPORT_MAX (or empty)
    std::atomic<uint64_t> no_codelet_{0};    // the codelet could not be loaded (gNB down?)
    std::atomic<uint64_t> queue_full_{0};    // our queue was full
    std::atomic<uint64_t> send_failed_{0};   // the gNB's queue was full: nothing drains it

    // poll() only: the counters line is printed at most once a second.
    std::chrono::steady_clock::time_point last_print_{};
    uint64_t                              printed_total_ = 0;

    // Must match codelets/dapp_report_relay/dapp_report_relay.yaml.
    struct jbpf_io_stream_id report_in_stream_id_ {
        0x75, 0x31, 0xab, 0xcd, 0x12, 0x34, 0x56, 0x78,
        0x90, 0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x6a
    };
    struct jbpf_io_stream_id presence_out_stream_id_ {
        0x75, 0x31, 0xab, 0xcd, 0x12, 0x34, 0x56, 0x78,
        0x90, 0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x6b
    };
};

}  // namespace e3sm_pipeline
