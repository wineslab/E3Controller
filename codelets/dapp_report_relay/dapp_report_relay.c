/*
 * dapp_report_relay.c
 *
 * Codelet attached to the gNB hook `e3_dapp_report_poll`, which the gNB's
 * E2SM-DAPP bridge fires every 0.5 ms while an xApp has a data subscription.
 *
 * The E3Controller queues the dApp reports it receives as control input
 * (struct e3_dapp_report_msg). Each time the hook fires, this codelet moves the
 * queued ones into the bridge with the helper e3_dapp_report_push, and the
 * bridge sends them to the xApp as E2SM-DAPP indications.
 *
 * It also tells the E3Controller how many xApps are subscribed (struct
 * e3_dapp_presence_msg on its output channel), so that the E3Controller sends
 * reports only while someone will read them.
 *
 * The codelet does not look inside a report; the helper checks the message.
 * See codelets/include/jbpf_e3_dapp_api.h for the whole path.
 */

#include "jbpf_defs.h"
#include "jbpf_helper.h"

#include "jbpf_e3_dapp_api.h"

#ifndef JBPF_CODELET_SUCCESS
#define JBPF_CODELET_SUCCESS (0)
#endif
#ifndef JBPF_CODELET_FAILURE
#define JBPF_CODELET_FAILURE (-1)
#endif

/* The most reports one call moves. The loop has to be bounded for the
 * verifier, and 8 is plenty: the bridge fires the hook every 0.5 ms, and a dApp
 * reports a few tens of times per second. Reports left over wait for the next
 * call. */
#define REPORTS_PER_CALL 8

/* ---- Helper stub (ID from jbpf_e3_dapp_api.h, shared with the gNB) ---- */

static long (*jbpf_e3_dapp_report_push)(const struct e3_dapp_report_msg* msg, uint64_t msg_len) =
    (void*)JBPF_E3_DAPP_REPORT_PUSH_ID;

/* ---- Maps ---- */

/* The reports the E3Controller sends. Up to 64 can wait between two polls; the
 * E3Controller counts a send that finds the queue full. The name and the stream
 * id are a contract with dapp_report_relay.yaml and src/e3sm/dapp_report_relay.cpp. */
jbpf_control_input_map(report_in, struct e3_dapp_report_msg, 64);

/* Room for one message. At 2 KB it does not fit in the 512-byte eBPF stack, so
 * it lives in a one-entry array map. */
struct jbpf_load_map_def SEC("maps") scratch = {
    .type = JBPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(int),
    .value_size = sizeof(struct e3_dapp_report_msg),
    .max_entries = 1,
};

/* The presence messages for the E3Controller. Same contract as report_in. */
jbpf_output_map(presence_out, struct e3_dapp_presence_msg, 8);

/* What the last presence message said, and when it went out. */
struct presence_state {
    uint64_t last_sent_ns;
    uint16_t last_nof_subscriptions;
    uint16_t sent;  /* 0 until the first message */
    uint32_t reserved;
};

struct jbpf_load_map_def SEC("maps") presence = {
    .type = JBPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(int),
    .value_size = sizeof(struct presence_state),
    .max_entries = 1,
};

/* Sends a presence message when the number of subscriptions changed, and every
 * E3_DAPP_PRESENCE_PERIOD_NS otherwise. Inlined, so the program has no
 * BPF-to-BPF call. */
static inline __attribute__((always_inline)) void
report_presence(uint16_t nof_subscriptions)
{
    int zero = 0;

    struct presence_state* last = (struct presence_state*)jbpf_map_lookup_elem(&presence, &zero);
    if (!last) {
        return;
    }

    uint64_t now     = jbpf_time_get_ns();
    int      changed = !last->sent || nof_subscriptions != last->last_nof_subscriptions;
    if (!changed && now - last->last_sent_ns < E3_DAPP_PRESENCE_PERIOD_NS) {
        return;
    }

    struct e3_dapp_presence_msg* out = (struct e3_dapp_presence_msg*)jbpf_get_output_buf(&presence_out);
    if (!out) {
        return;
    }
    out->version           = E3_DAPP_PRESENCE_VERSION;
    out->nof_subscriptions = nof_subscriptions;
    out->reserved          = 0;
    out->gnb_ts_ns         = now;
    if (jbpf_send_output(&presence_out) < 0) {
        return;  /* tried again on the next call */
    }

    last->last_sent_ns           = now;
    last->last_nof_subscriptions = nof_subscriptions;
    last->sent                   = 1;
}

/* ---- Main codelet entry ---- */

SEC(JBPF_E3_SEC_E2_DAPP)
uint64_t
jbpf_main(void* state)
{
    struct jbpf_e2_dapp_ctx* ctx = (struct jbpf_e2_dapp_ctx*)state;
    int                      zero = 0;

    struct e3_dapp_report_msg* msg = (struct e3_dapp_report_msg*)jbpf_map_lookup_elem(&scratch, &zero);
    if (!msg) {
        return JBPF_CODELET_FAILURE;
    }

    for (int i = 0; i < REPORTS_PER_CALL; i++) {
        /* 1 means a message was waiting and is now in *msg. */
        if (jbpf_control_input_receive(&report_in, msg, sizeof(*msg)) != 1) {
            break;
        }
        jbpf_e3_dapp_report_push(msg, sizeof(*msg));
    }

    report_presence(ctx->nof_subscriptions);

    return JBPF_CODELET_SUCCESS;
}
