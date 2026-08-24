#ifndef UPLINK_SLOT_DATA_H
#define UPLINK_SLOT_DATA_H

#include <stdint.h>

/*
 * uplink_slot_data.h
 *
 * Output schema for the uplink_slot_samples codelet (attached to the
 * capture_uplink_slot ocudu hook). Shared header between the codelet
 * (compiled to eBPF and loaded into the gnb) and the E3Controller's
 * SlotIqPipeline that consumes the codelet's output.
 *
 * Each fire produces one struct uplink_slot_sample, carrying:
 *   - slot/sector metadata parsed by the ocudu hook (no eCPRI / Open
 *     Fronthaul header walk needed on the codelet side)
 *   - grid dimensions (nof_ports/symbols/subcarriers) describing the
 *     IQ blob's layout
 *   - inline cbf16_t IQ payload, laid out [port][symbol][subcarrier]
 *     in row-major order (= ocudu's resource_grid_reader_impl tensor
 *     layout)
 *
 * Sizing:
 *   The inline `iq` array is fixed-size (eBPF verifier requires
 *   compile-time-known struct sizes). MAX_SLOT_IQ_BYTES is the ceiling
 *   for the runtime payload; the actual bytes used per fire are
 *   recorded in iq_size_bytes. Sizing at 733 824 bytes covers every
 *   NR BWP at 30 kHz SCS for 4 antenna ports (4T4R MIMO):
 *     273 PRBs * 12 subc * 14 symbols * 4 ports * 4 bytes = 733 824 B
 *   (exactly; no headroom needed — the grid is fixed for 100 MHz/30 kHz).
 */

/* Largest cbf16_t blob a single slot can carry through this codelet.
 * Compile-time constant - sizes the output ring slot, the verifier's
 * bounded copy loop, and the in-struct array length.
 * 4 ports * 14 symbols * 3276 subc * 4 bytes = 733824. */
#define MAX_SLOT_IQ_BYTES 733824

/* LEGACY. The codelet publishes struct e3_slot_desc (codelets/include/
 * jbpf_e3_slot_api.h), not this. Nothing writes this struct any more; the
 * header survives for MAX_SLOT_IQ_BYTES. Kept as the record of the full-IQ
 * wire format.
 */
struct uplink_slot_sample {
    /* A1 entry: the gNB hook caller stamps this immediately before the hook
     * fires, on the LAST symbol of the slot, gated on is_valid - so the
     * resource grid is complete and nothing has been copied yet. The gNB hands
     * the codelet the grid's raw storage pointer and never memcpys it, so this
     * is the true "data exists, nothing has moved" instant.
     *
     * CLOCK_MONOTONIC ns. (The gNB moved off CLOCK_REALTIME; the codelet's
     * jbpf_time_get_ns() follows via jbpf_patches/jbpf_monotonic_time.patch,
     * and so does the controller's dispatcher poll. All three have to agree or
     * the subtractions below silently mix epochs.) */
    uint64_t gnb_ts_ns;

    /* A1 exit / A2 entry, stamped LAST - just before jbpf_send_output(), after
     * the codelet has finished moving the slot's data. Same clock as above.
     *
     *   codelet_ts_ns - gnb_ts_ns = A1, the data recording itself.
     *
     * This is emphatically NOT "hook -> codelet entry latency", and the cost it
     * covers is not jbpf plumbing: dispatch is a few microseconds and the data
     * movement is tens. Nor is there any verifier cost in it - the gNB loads
     * through ubpf, whose JIT emits no bounds checks, and verification is an
     * offline build-time gate (see codelets/Makefile). The descriptor path
     * splits this into codelet_entry_ts_ns and codelet_ts_ns for exactly that
     * reason; this legacy struct keeps the single stamp. */
    uint64_t codelet_ts_ns;

    /* 3GPP slot identification (parsed by the ocudu hook). */
    uint16_t sfn;
    uint16_t subframe_id;
    uint16_t slot_id;
    uint16_t sector_id;

    /* Resource grid shape for this slot. The IQ blob below holds
     * exactly nof_ports * nof_symbols * nof_subcarriers cbf16_t
     * samples = nof_ports * nof_symbols * nof_subcarriers * 4 bytes,
     * laid out [port][symbol][subcarrier] row-major. */
    uint16_t nof_ports;
    uint16_t nof_symbols;
    uint16_t nof_subcarriers;

    /* 0 reserved / future use (padding to align iq_size_bytes). */
    uint16_t reserved;

    /* Actual number of bytes used in iq below. Always
     *   = nof_ports * nof_symbols * nof_subcarriers * sizeof(cbf16_t)
     * unless the codelet had to clamp to MAX_SLOT_IQ_BYTES. */
    uint32_t iq_size_bytes;

    /* Inline cbf16_t IQ blob. Layout matches the gnb resource grid:
     * each cbf16_t is 4 bytes (bf16 real + bf16 imag), elements
     * indexed by (port, symbol, subcarrier) in row-major order. */
    uint8_t iq[MAX_SLOT_IQ_BYTES];
};

#endif /* UPLINK_SLOT_DATA_H */
