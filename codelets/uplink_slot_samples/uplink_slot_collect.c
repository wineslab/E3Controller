/*
 * uplink_slot_collect.c
 *
 * Codelet attached to the ocudu hook `capture_uplink_slot` (defined in
 * upper_phy_rx_symbol_handler_impl.cpp). Fires once per UL slot, on the
 * last symbol, with the resource grid's contiguous cbf16_t storage
 * already pointed at via the hook ctx.
 *
 * The ocudu hook already hands us the
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

/* Inline stub for the host-side native memcpy helper added to
 * ocudu-wineslab/external/jbpf (enum JBPF_NATIVE_MEMCPY = 19, default
 * registration in JBPF_DEFAULT_HELPER_FUNCS). Declared inline here
 * rather than via the SDK's jbpf_helper.h so the codelet builds with
 * the stock srs-jbpf-sdk Docker image, with the new helper ID only
 * needing to exist on the gNB-side jbpf at codelet-load time. */
static long (*jbpf_native_memcpy)(void *dst, const void *src, uint64_t len)
    = (void *)19;

/* ---- Maps ----
 * One output map, zero-copy via jbpf_get_output_buf / jbpf_send_output.
 * Using jbpf_output_map rather than jbpf_ringbuf_map so the codelet
 * writes the full ~733 KB slot struct (4 antennas x 14 sym x 3276 subc x
 * cbf16_t) directly into the ring slot without going through a temp-map
 * intermediate first. One memcpy on the codelet side instead of two.
 */
jbpf_output_map(output_map, struct uplink_slot_sample, 32);

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
     *   - codelet_ts_ns: captured below, just before jbpf_send_output(),
     *                    so codelet_to_dispatch_us reflects only the
     *                    dispatcher's busy-poll latency rather than the
     *                    codelet's own copy/send cost. The codelet's own
     *                    runtime is then attributed to gnb_to_codelet_us.
     * Both use CLOCK_REALTIME ns so they subtract cleanly. */
    out->gnb_ts_ns       = ctx->gnb_ts_ns;
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

    /* One host-native memcpy through the JBPF_NATIVE_MEMCPY helper,
     * replacing the previous MAX_COPY_CHUNKS x 128 B __builtin_memcpy
     * loop. The verifier already proved (ctx->data, ctx->data_end)
     * forms a valid packet range above; the host-side helper
     * additionally clamps len to JBPF_NATIVE_MEMCPY_MAX_LEN. */
    const void *src = (const void *)(uintptr_t)ctx->data;
    if (copy_size > 0) {
        jbpf_native_memcpy(out->iq, src, (uint64_t)copy_size);
    }

    /* Stamp the codelet timestamp here so codelet_to_dispatch_us
     * captures only the dispatcher's busy-poll pickup latency, not the
     * memcpy + send_output cost above. The memcpy + send-cost shows up
     * in gnb_to_codelet_us instead. */
    out->codelet_ts_ns   = jbpf_time_get_ns();

    /* Submit the slot to the output ring. The E3Controller's
     * SlotIqPipeline will see this slot on its next jbpf poll. */
    if (jbpf_send_output(&output_map) < 0) {
        return JBPF_CODELET_FAILURE;
    }

    return JBPF_CODELET_SUCCESS;
}
