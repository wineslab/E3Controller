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

struct uplink_slot_sample {
    /* gNB-side hand-off timestamp (CLOCK_REALTIME ns) captured by the
     * ocudu hook caller, right before the hook fires. This is the RAN
     * anchor for end-to-end latency; the dApp subtracts its own
     * CLOCK_REALTIME receipt time from it. Same clock domain as
     * jbpf_time_get_ns() so codelet_ts_ns below subtracts cleanly. */
    uint64_t gnb_ts_ns;

    /* NOTE: the descriptor path (`writer: gnb`) does not use this struct; it
     * publishes struct e3_slot_desc, which carries the entry/exit split. This
     * legacy full-IQ struct keeps a single stamp.
     *
     * Codelet entry timestamp (jbpf_time_get_ns(), CLOCK_MONOTONIC ns --
     * see jbpf_patches/jbpf_monotonic_time.patch). Subtract gnb_ts_ns for the
     * ocudu hook -> codelet latency. */
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
