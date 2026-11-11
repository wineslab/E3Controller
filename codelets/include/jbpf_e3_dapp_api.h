/*
 * jbpf_e3_dapp_api.h — how the E3Controller hands dApp reports to the gNB.
 *
 * SHARED CONTRACT, kept byte-identical in
 *   E3Controller  codelets/include/jbpf_e3_dapp_api.h
 *   ocudu         ocudu_janus/e3/jbpf_e3_dapp_api.h
 * ocudu's configure compares the two copies and stops on any difference.
 *
 * The path of one report:
 *
 *   dApp          sends an E3 DAppReport (for example the PRBs where it
 *                 detected an incumbent) to the E3Controller.
 *   E3Controller  copies it into a struct e3_dapp_report_msg and sends that as
 *                 control input to the codelet dapp_report_relay.
 *   gNB           the E2SM-DAPP bridge fires the hook e3_dapp_report_poll every
 *                 0.5 ms while an xApp has an E2SM-DAPP data subscription. The
 *                 codelet hands the queued messages to the bridge with the
 *                 helper below.
 *   bridge        sends each one to the xApp right away, as an E2SM-DAPP
 *                 indication.
 *
 * Nobody decodes the report on the way: data[] is the E3 reportData, and it
 * becomes the indication's data unchanged. The xApp decodes it with the Service
 * Model named by ran_function_id (1 = Spectrum).
 */

#ifndef JBPF_E3_DAPP_API_H
#define JBPF_E3_DAPP_API_H

#include <stdint.h>

#include "jbpf_helper_api_defs_ext.h"

/* ---- IDs (jbpf_e3_ids.h has helpers +0, +1 and program types +0..+4) ---- */

/*
 * long jbpf_e3_dapp_report_push(const struct e3_dapp_report_msg* msg, uint64_t msg_len)
 *
 * Copies one report into the gNB's E2SM-DAPP bridge. Returns 0, or a negative
 * value when the message is not valid (size, version, data_len) or when no
 * bridge exists (E2SM-DAPP disabled in the gNB).
 */
#define JBPF_E3_DAPP_REPORT_PUSH_ID (CUSTOM_HELPER_START_ID + 2)

/* Program type of the codelets at e3_dapp_report_poll, and the ELF section the
 * verifier maps to it. */
#define JBPF_E3_PROG_TYPE_E2_DAPP (CUSTOM_PROGRAM_START_ID + 5)
#define JBPF_E3_SEC_E2_DAPP       "jbpf_e2_dapp"

/* ---- The report message (E3Controller -> codelet, control input) ---- */

#define E3_DAPP_REPORT_VERSION 1

/* The largest report the channel carries. E3 allows 32768 bytes and an
 * E2SM-DAPP indication about 16 KB; a PRB report for all 273 PRBs is about
 * 310 bytes. The E3Controller counts and drops anything larger. */
#define E3_DAPP_REPORT_MAX 2048

struct e3_dapp_report_msg {
    uint16_t version;          /* E3_DAPP_REPORT_VERSION */
    uint16_t data_len;         /* bytes of data[] in use, 1..E3_DAPP_REPORT_MAX */
    uint32_t dapp_id;          /* E3 dAppIdentifier of the sender */
    uint32_t ran_function_id;  /* E3 RAN function of the report (1 = Spectrum) */
    uint32_t sequence_id;      /* E3 sequenceId, set by the dApp */
    /* CLOCK_MONOTONIC ns when the E3Controller received the report; both ends
     * are on one host. The codelet's queue keeps filling while no xApp is
     * subscribed, so when one subscribes the bridge drops what is older than a
     * second rather than sending it as news. */
    uint64_t received_ts_ns;
    uint8_t  data[E3_DAPP_REPORT_MAX];  /* E3 reportData, unchanged */
};

/* ---- Hook context of e3_dapp_report_poll ---- */

/* The hook passes no data of its own, so data and data_end are 0: the codelet
 * only drains its control input. The layout follows the other RAN hooks, with
 * data, data_end and meta_data first. */
struct jbpf_e2_dapp_ctx {
    uint64_t data;               /* 0 */
    uint64_t data_end;           /* 0 */
    uint64_t meta_data;          /* 0 */
    uint16_t ctx_id;             /* 0 */
    /* E2SM-DAPP data subscriptions (report style 1) in the gNB right now. The
     * bridge fires the hook only while there is one, and once more with 0 when
     * the last one ends. */
    uint16_t nof_subscriptions;
};

/* ---- Presence (codelet -> E3Controller, output channel) ---- */

/*
 * jbpf gives the gNB no other way to tell the E3Controller anything, so the
 * codelet reports on its output channel how many xApps are subscribed: when the
 * number changes, and every E3_DAPP_PRESENCE_PERIOD_NS while it stays the same.
 * The E3Controller forwards dApp reports only while that number is above 0 and
 * a message came recently; otherwise it drops them itself, so that nothing
 * piles up in the codelet's queue.
 */
#define E3_DAPP_PRESENCE_VERSION   1
#define E3_DAPP_PRESENCE_PERIOD_NS (100ULL * 1000 * 1000)

struct e3_dapp_presence_msg {
    uint16_t version;            /* E3_DAPP_PRESENCE_VERSION */
    uint16_t nof_subscriptions;  /* as in struct jbpf_e2_dapp_ctx */
    uint32_t reserved;           /* 0 */
    uint64_t gnb_ts_ns;          /* jbpf_time_get_ns() when the codelet sent it */
};

#endif /* JBPF_E3_DAPP_API_H */
