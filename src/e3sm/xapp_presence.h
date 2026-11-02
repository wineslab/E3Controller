/*
 * xapp_presence.h — whether an xApp is listening for dApp reports.
 *
 * The gNB's relay codelet sends the number of E2SM-DAPP data subscriptions
 * (struct e3_dapp_presence_msg) when it changes and every 100 ms otherwise.
 * DAppReportRelay forwards dApp reports only while this says an xApp is there.
 *
 * "There" means: the last message counted at least one subscription, and it is
 * recent. The second condition covers the cases where the messages simply stop:
 * the gNB stopped or restarted, or the codelet was unloaded.
 *
 * Times are the E3Controller's own CLOCK_MONOTONIC when a message was received;
 * the gNB's timestamp in the message is not compared with them.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace e3sm_pipeline {

class XAppPresence {
public:
    /// Three presence periods: one late message is not a missing xApp.
    static constexpr uint64_t kTimeoutNs = 300ULL * 1000 * 1000;

    /// A presence message arrived at now_ns. Main thread.
    void on_message(uint16_t nof_subscriptions, uint64_t now_ns)
    {
        subscriptions_.store(nof_subscriptions, std::memory_order_relaxed);
        last_message_ns_.store(now_ns, std::memory_order_release);
    }

    /// True if an xApp is subscribed, as of now_ns. Any thread.
    bool present(uint64_t now_ns) const
    {
        const uint64_t last = last_message_ns_.load(std::memory_order_acquire);
        if (last == 0) {
            return false;  // never heard from the gNB
        }
        if (now_ns > last && now_ns - last > kTimeoutNs) {
            return false;  // the messages stopped
        }
        return subscriptions_.load(std::memory_order_relaxed) > 0;
    }

    /// The number in the last message, for the logs.
    uint16_t subscriptions() const { return subscriptions_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint16_t> subscriptions_{0};
    std::atomic<uint64_t> last_message_ns_{0};
};

}  // namespace e3sm_pipeline
