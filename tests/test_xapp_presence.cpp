/*
 * Tests of XAppPresence (src/e3sm/xapp_presence.h): when DAppReportRelay
 * forwards dApp reports to the gNB, and when it drops them.
 *
 * Build with -DE3C_BUILD_TESTS=ON and run with ctest, or directly:
 *   out/bin/test_xapp_presence
 */

#include "e3sm/xapp_presence.h"

#include <cstdint>
#include <cstdio>
#include <string>

using e3sm_pipeline::XAppPresence;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
    if (condition) {
        std::printf("  ok    %s\n", what.c_str());
    } else {
        std::printf("  FAIL  %s\n", what.c_str());
        g_failures++;
    }
}

static constexpr uint64_t kMs = 1000ULL * 1000;

// An arbitrary CLOCK_MONOTONIC reading; 0 means "never" to XAppPresence.
static constexpr uint64_t kStart = 5000 * kMs;

static void test_nothing_heard()
{
    std::printf("no message from the gNB yet\n");
    XAppPresence presence;
    check(!presence.present(kStart), "absent: the codelet has said nothing");
}

static void test_subscriptions_count()
{
    std::printf("the number in the message\n");
    XAppPresence presence;

    presence.on_message(0, kStart);
    check(!presence.present(kStart), "0 subscriptions: absent");

    presence.on_message(2, kStart + 10 * kMs);
    check(presence.present(kStart + 10 * kMs), "2 subscriptions: present");
    check(presence.subscriptions() == 2, "keeps the number for the logs");

    // The last xApp went: the bridge fires the hook once more with 0.
    presence.on_message(0, kStart + 20 * kMs);
    check(!presence.present(kStart + 20 * kMs), "back to 0: absent at once, without waiting for the timeout");
}

static void test_messages_stop()
{
    std::printf("the messages stop (gNB stopped or restarted, codelet unloaded)\n");
    XAppPresence presence;
    presence.on_message(1, kStart);

    check(presence.present(kStart + 100 * kMs), "one period later: present");
    check(presence.present(kStart + 250 * kMs), "a late message is not a missing xApp");
    check(presence.present(kStart + XAppPresence::kTimeoutNs), "at the timeout: still present");
    check(!presence.present(kStart + XAppPresence::kTimeoutNs + 1), "past the timeout: absent");

    presence.on_message(1, kStart + 400 * kMs);
    check(presence.present(kStart + 400 * kMs), "messages again: present again");
}

static void test_clock_order()
{
    std::printf("a reader whose clock reading is older than the message\n");
    XAppPresence presence;
    presence.on_message(1, kStart);
    // on_report() takes its time before the main loop stores a newer message.
    check(presence.present(kStart - 1 * kMs), "not treated as a timeout");
}

int main()
{
    test_nothing_heard();
    test_subscriptions_count();
    test_messages_stop();
    test_clock_order();

    if (g_failures > 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
