/*
 * uplink_slot_collect.c
 *
 * Codelet attached to the ocudu hook `capture_uplink_slot` (defined in
 * upper_phy_rx_symbol_handler_impl.cpp). Fires once per UL slot, on the last
 * symbol, with the resource grid's contiguous cbf16_t storage pointed at via
 * the hook ctx.
 *
 * WHAT THIS CODELET DOES *NOT* DO ANY MORE: copy the grid.
 *
 * It previously moved ~733 KB into a jbpf output-map slot. eBPF cannot do that
 * job well — the in-VM copy ran at ~8.5 GB/s, and the ISA has no
 * floating-point or SIMD instructions at all (114 integer opcodes), so it also
 * cannot perform the cbf16 -> fp16 conversion the dApp's row format needs.
 *
 * So the split is now:
 *   - this codelet decides *whether* to publish (verified bytecode, policy),
 *   - a native SIMD helper does the convert + row write (mechanism).
 *
 * The codelet never holds a pointer into the destination: it passes a selector
 * and receives back the (buffer, row) indices the helper used. Only the SOURCE
 * needs verifier-proven bounds, which shrinks the trusted surface considerably.
 *
 * Output: one ~64 B `struct e3_slot_desc` per published slot (see
 * codelets/include/jbpf_e3_slot_api.h), which the E3Controller turns into an
 * E3 Indication. The IQ itself goes straight to /e3_ran_buffers.
 */

#include "jbpf_defs.h"
#include "jbpf_helper.h"
#include "jbpf_srsran_contexts.h"

#include "jbpf_e3_ids.h"
#include "jbpf_e3_slot_api.h"

/* Codelet return codes. Defined locally so this codelet takes no dependency on
 * an external SDK utils tree for two constants. */
#ifndef JBPF_CODELET_SUCCESS
#define JBPF_CODELET_SUCCESS (0)
#endif
#ifndef JBPF_CODELET_FAILURE
#define JBPF_CODELET_FAILURE (-1)
#endif

/* ---- Helper stubs ----
 *
 * IDs come from jbpf_e3_ids.h, which the gNB-side registration also includes —
 * that shared header is what stops a codelet from verifying cleanly and then
 * failing to load with "call to nonexistent function".
 *
 * IDs are in jbpf's documented CUSTOM_HELPER_START_ID range. The previous
 * version of this file hardcoded `(void *)19`, inside jbpf's built-in range,
 * where upstream could later allocate a different function.
 */
static long (*jbpf_e3_attach)(const struct e3_shm_cfg* cfg, uint64_t cfg_len) = (void*)JBPF_E3_ATTACH_ID;

static long (*jbpf_e3_publish_slot)(
    const void* src, uint64_t src_len, const struct e3_slot_sel* sel, uint64_t sel_len) =
    (void*)JBPF_E3_PUBLISH_SLOT_ID;

/* ---- Maps ---- */

/* Descriptors only. Channel name stays "output_map": that is a contract with
 * uplink_slot_samples.yaml and slot_iq_pipeline.cpp:75. Only the ELEMENT TYPE
 * changes, from the ~733 KB uplink_slot_sample to a ~64 B descriptor — ~64 B each instead of the old 733 KB. */
jbpf_output_map(output_map, struct e3_slot_desc, 32);

/* Control input is a CHANNEL: jbpf_control_input_receive() returns 1 only when
 * a new message is waiting. So each channel needs a companion array map to hold
 * the current value across invocations (eBPF has no writable statics). Same
 * pattern as ecpri_iq_collect.c. */
jbpf_control_input_map(sel_in, struct e3_slot_sel, 1);
jbpf_control_input_map(shm_in, struct e3_shm_cfg, 1);

struct jbpf_load_map_def SEC("maps") sel_state = {
    .type = JBPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(int),
    .value_size = sizeof(struct e3_slot_sel),
    .max_entries = 1,
};

/* ---- Main codelet entry ---- */

SEC(JBPF_E3_SEC_RAN_SLOT)
uint64_t
jbpf_main(void* state)
{
    struct jbpf_ran_slot_ctx* ctx = (struct jbpf_ran_slot_ctx*)state;
    int zero = 0;

    /* FIRST statement: everything after this belongs to the codelet, not to
     * jbpf delivery. Paired with the exit stamp further down; see the comment on
     * codelet_entry_ts_ns in uplink_slot_data.h for why the two are split. */
    uint64_t entry_ts_ns = jbpf_time_get_ns();

    /* Region config: forwarded straight to the helper, which attaches lazily.
     * The E3Controller owns /e3_ran_buffers; the gNB only ever attaches, so no
     * duplicate shm configuration lives on the RAN side. */
    struct e3_shm_cfg cfg;
    if (jbpf_control_input_receive(&shm_in, (void*)&cfg, sizeof(cfg)) == 1) {
        jbpf_e3_attach(&cfg, sizeof(cfg));
    }

    /* Selector: cache on change, read the cache every fire. */
    struct e3_slot_sel incoming;
    if (jbpf_control_input_receive(&sel_in, (void*)&incoming, sizeof(incoming)) == 1) {
        struct e3_slot_sel* slot = (struct e3_slot_sel*)jbpf_map_lookup_elem(&sel_state, &zero);
        if (slot) {
            *slot = incoming;
        }
    }

    struct e3_slot_sel* sel = (struct e3_slot_sel*)jbpf_map_lookup_elem(&sel_state, &zero);
    if (!sel) {
        return JBPF_CODELET_FAILURE;
    }

    /* ---- Temporal filter, BEFORE any data movement ----
     *
     * A non-selected slot costs only these comparisons: no helper call, no
     * bytes touched, nothing in the PHY RX thread. That is what makes slot
     * filtering a real lever on RAN-side cost rather than just a delivery
     * filter.
     *
     * slot_mask == 0 means publish everything — the default, and identical to
     * the unfiltered system. */
    uint8_t filtered = 0;

    if (sel->sfn_mod > 1) {
        if ((uint16_t)(ctx->sfn % sel->sfn_mod) != sel->sfn_offset) {
            return JBPF_CODELET_SUCCESS;
        }
        filtered = E3_SLOT_FLAG_FILTERED;
    }

    if (sel->slot_mask != 0) {
        uint16_t sid = ctx->slot_id;
        if (sid >= 64) {
            /* Outside the mask's width; cannot have been selected. */
            return JBPF_CODELET_SUCCESS;
        }
        if (((sel->slot_mask >> sid) & 1ULL) == 0) {
            return JBPF_CODELET_SUCCESS;
        }
        filtered = E3_SLOT_FLAG_FILTERED;
    }

    /* ---- Publish ----
     *
     * Ladder of per-config CONSTANT sizes, largest first. Two properties are
     * load-bearing and were established by running the verifier, not by
     * reasoning:
     *
     *   1. The length must be a LITERAL at the call site. Hoisting it into a
     *      variable and calling once reintroduces a branch join that loses the
     *      bound proof.
     *   2. Each bound check must immediately precede its own call.
     *
     * The obvious form
     *      avail = ctx->data_end - ctx->data; clamp; publish(src, avail)
     * is REJECTED, even keeping avail in uint64:
     *      "Upper bound must be at most packet_size
     *       (valid_access(r1.offset, width=r2) for read)"
     * PREVAIL does compute ptr-minus-ptr relationally, but the relation does
     * not survive into the ValidAccess assertion.
     *
     * One rung per supported port count is the cost of accepting 2/4/8 ports
     * from a single binary.
     */
    const uint8_t* src = (const uint8_t*)(uintptr_t)ctx->data;
    const uint8_t* end = (const uint8_t*)(uintptr_t)ctx->data_end;

    long     rc = -1;
    uint32_t published = 0;
    uint8_t  flags = filtered;

    if (src + E3_SLOT_BYTES_8P <= end) {
        published = E3_SLOT_BYTES_8P;
        rc = jbpf_e3_publish_slot(src, E3_SLOT_BYTES_8P, sel, sizeof(*sel));
    } else if (src + E3_SLOT_BYTES_4P <= end) {
        published = E3_SLOT_BYTES_4P;
        rc = jbpf_e3_publish_slot(src, E3_SLOT_BYTES_4P, sel, sizeof(*sel));
    } else if (src + E3_SLOT_BYTES_2P <= end) {
        published = E3_SLOT_BYTES_2P;
        rc = jbpf_e3_publish_slot(src, E3_SLOT_BYTES_2P, sel, sizeof(*sel));
    } else {
        /* No rung matched: the grid is smaller than the smallest config we know
         * how to publish. Emit a descriptor with TRUNCATED set and nothing
         * written, so the condition is visible to the controller instead of
         * looking like a dead hook. Never publish a prefix — a partial antenna
         * set with no signal is a silently wrong measurement. */
        published = 0;
        flags |= E3_SLOT_FLAG_TRUNCATED;
    }

    if (rc < 0 && (flags & E3_SLOT_FLAG_TRUNCATED) == 0) {
        /* Helper refused: not attached, stale epoch, or the selector implied
         * more bytes than the row holds. Report rather than retry. */
        published = 0;
        flags |= E3_SLOT_FLAG_TRUNCATED;
        rc = 0;
    }

    /* ---- Descriptor ---- */
    struct e3_slot_desc* out = (struct e3_slot_desc*)jbpf_get_output_buf(&output_map);
    if (!out) {
        return JBPF_CODELET_FAILURE;
    }

    out->gnb_ts_ns       = ctx->gnb_ts_ns;
    out->bytes_written   = published;
    out->fh_buffer_index = (uint8_t)(((uint64_t)rc >> 32) & 0xffULL);
    out->fh_write_index  = (uint32_t)((uint64_t)rc & 0xffffffffULL);
    out->flags           = flags;
    out->sfn             = ctx->sfn;
    out->subframe_id     = ctx->subframe_id;
    out->slot_id         = ctx->slot_id;
    out->sector_id       = ctx->ctx_id;
    out->nof_ports       = ctx->nof_ports;
    out->nof_symbols     = ctx->nof_symbols;
    out->nof_subcarriers = ctx->nof_subcarriers;

    /* Stamped last so the controller's codelet_to_dispatch measurement reflects
     * only the dispatcher's pickup latency. */
    out->codelet_entry_ts_ns = entry_ts_ns;
    out->codelet_ts_ns       = jbpf_time_get_ns();

    if (jbpf_send_output(&output_map) < 0) {
        return JBPF_CODELET_FAILURE;
    }

    return JBPF_CODELET_SUCCESS;
}
