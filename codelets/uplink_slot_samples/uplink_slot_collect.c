/*
 * uplink_slot_collect.c
 *
 * Codelet attached to the ocudu hook `capture_uplink_slot` (defined in
 * upper_phy_rx_symbol_handler_impl.cpp). Fires once per UL slot, on the
 * last symbol, with the resource grid's contiguous cbf16_t storage
 * already pointed at via the hook ctx.
 *
 * Compared to ecpri_iq_samples, this codelet does NOT walk Ethernet /
 * eCPRI / Open Fronthaul U-plane headers, NOT BFP-decompress, and NOT
 * accumulate per-symbol state. The ocudu hook already hands us the
 * fully-assembled slot in resource-grid order. We do one memcpy from
 * the gnb-owned grid storage into the output ring slot (jbpf SHM the
 * E3Controller polls), stamp metadata, and submit.
 *
 * Output layout: see uplink_slot_data.h.
 */

#include "jbpf_defs.h"
#include "jbpf_helper.h"
#include "jbpf_srsran_contexts.h"
#include "uplink_slot_data.h"

/* Codelet return codes - mirror the macros in jrtc-apps/codelets/utils/
 * misc_utils.h so we don't take an external dependency on the utils
 * tree just for these two constants. */
#ifndef JBPF_CODELET_SUCCESS
#define JBPF_CODELET_SUCCESS (0)
#endif
#ifndef JBPF_CODELET_FAILURE
#define JBPF_CODELET_FAILURE (-1)
#endif

/* ---- Maps ----
 * One output map, zero-copy via jbpf_get_output_buf / jbpf_send_output.
 * Using jbpf_output_map rather than jbpf_ringbuf_map so the codelet
 * writes the 200 KB struct directly into the ring slot without going
 * through a temp-map intermediate first. One memcpy on the codelet
 * side instead of two.
 */
jbpf_output_map(output_map, struct uplink_slot_sample, 32);

/* ---- Constants ---- */

/* Chunked copy size. 16 bytes = 4 cbf16_t samples. Picked so that:
 *  - Constant size lets __builtin_memcpy lower to a single vmov sequence
 *    the eBPF verifier handles cheaply.
 *  - 16 divides nof_subc * sizeof(cbf16_t) for every NR PRB count we
 *    deploy (106 -> 5088 bytes/symbol, 273 -> 13104 bytes/symbol; both
 *    exact multiples of 16). So we never leave a trailing tail.
 *  - Outer loop bound = MAX_SLOT_IQ_BYTES / 16 = 12 500 iterations max,
 *    well within the verifier's complexity budget.
 */
#define COPY_CHUNK_BYTES 16
#define MAX_COPY_CHUNKS  (MAX_SLOT_IQ_BYTES / COPY_CHUNK_BYTES)

/* ---- Main codelet entry ---- */

SEC("jbpf_ran_ofh")
uint64_t jbpf_main(void *state)
{
    struct jbpf_ran_slot_ctx *ctx = (struct jbpf_ran_slot_ctx *)state;

    /* Reserve a ring slot directly in jbpf SHM. Writing in-place
     * here is the one-and-only memcpy on the codelet side - no temp
     * map, no double copy. */
    struct uplink_slot_sample *out =
        (struct uplink_slot_sample *)jbpf_get_output_buf(&output_map);
    if (!out) {
        return JBPF_CODELET_FAILURE;
    }

    /* Two timestamps:
     *   - gnb_ts_ns    : stamped by the ocudu hook caller at hand-off,
     *                    the RAN anchor for RAN -> dApp latency.
     *   - codelet_ts_ns: captured here at codelet entry, lets the
     *                    controller measure ocudu -> codelet (jbpf
     *                    invocation) overhead as a separate stage.
     * Both use CLOCK_REALTIME ns so they subtract cleanly. */
    out->gnb_ts_ns       = ctx->gnb_ts_ns;
    out->codelet_ts_ns   = jbpf_time_get_ns();
    out->sfn             = ctx->sfn;
    out->subframe_id     = ctx->subframe_id;
    out->slot_id         = ctx->slot_id;
    out->sector_id       = ctx->ctx_id;
    out->nof_ports       = ctx->nof_ports;
    out->nof_symbols     = ctx->nof_symbols;
    out->nof_subcarriers = ctx->nof_subcarriers;
    out->reserved        = 0;

    /* Compute the available payload size from the verifier-tracked
     * (ctx->data, ctx->data_end) range, clamp to the static struct
     * ceiling. The verifier requires both ends to be visible packet
     * pointers; the cast through uintptr is to make the subtraction
     * the verifier sees as a plain integer. */
    uint64_t avail = (uint64_t)ctx->data_end - (uint64_t)ctx->data;
    if (avail > MAX_SLOT_IQ_BYTES) {
        avail = MAX_SLOT_IQ_BYTES;
    }
    uint32_t copy_size = (uint32_t)avail;
    out->iq_size_bytes = copy_size;

    /* Chunked memcpy. The outer loop is bounded by MAX_COPY_CHUNKS at
     * compile time; each chunk is a fixed-size memcpy the verifier
     * lowers to a small constant instruction sequence. Bounds check
     * per chunk against ctx->data_end (required - the verifier
     * treats ctx->data as a packet pointer and demands per-access
     * bounds) and against copy_size for early termination. */
    const uint8_t *src     = (const uint8_t *)(uintptr_t)ctx->data;
    const uint8_t *src_end = (const uint8_t *)(uintptr_t)ctx->data_end;

    for (uint32_t i = 0; i < MAX_COPY_CHUNKS; i++) {
        uint32_t off = i * COPY_CHUNK_BYTES;
        /* Reached the end of the actual payload - leave the rest of
         * out->iq untouched (jbpf zeros the ring slot on reserve, so
         * untouched bytes are deterministic zeros). */
        if (off >= copy_size) {
            break;
        }
        /* Verifier safety: confirm the next full chunk is in the
         * packet-bounded source range before reading. */
        if (src + off + COPY_CHUNK_BYTES > src_end) {
            break;
        }
        __builtin_memcpy(&out->iq[off], src + off, COPY_CHUNK_BYTES);
    }

    /* Submit the slot to the output ring. The E3Controller's
     * SlotIqPipeline will see this slot on its next jbpf poll. */
    if (jbpf_send_output(&output_map) < 0) {
        return JBPF_CODELET_FAILURE;
    }

    return JBPF_CODELET_SUCCESS;
}
