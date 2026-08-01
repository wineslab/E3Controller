/*
 * jbpf_e3_slot_api.h — wire contract for the per-slot L1 path (RAN function 2).
 *
 * Shared by FOUR consumers, which is why it lives here rather than in a
 * codelet directory:
 *
 *   1. codelets/uplink_slot_samples/uplink_slot_collect.c  (producer)
 *   2. the gNB-side helper in ocudu-e3                     (executes the copy)
 *   3. E3Controller's SlotIqPipeline                       (reads descriptors)
 *   4. codelets/verifier/e3_verifier_cli.cpp               (sizes the sel arg)
 *
 * The IQ payload is NOT carried here. The helper writes it straight into the
 * E3Controller-owned /e3_ran_buffers region in the dApp's fp16 format, and the
 * codelet publishes only the ~64 B descriptor below. See
 * report/plan_codelet_verification_and_config_scaling.md.
 */

#ifndef JBPF_E3_SLOT_API_H
#define JBPF_E3_SLOT_API_H

#include <stdint.h>

/* ---- Per-config slot sizes ----
 *
 * ports * 14 symbols * 3276 subcarriers * 4 B/sample (cbf16 = bf16 I + bf16 Q).
 * 3276 = 273 PRB * 12, i.e. 100 MHz at 30 kHz SCS.
 *
 * These exist as separate compile-time constants, not a computed value,
 * because the verifier requires the length passed to the publish helper to be
 * a LITERAL at the call site — see the ladder in uplink_slot_collect.c.
 */
#define E3_SLOT_BYTES_2P 366912u  /* 2 ports */
#define E3_SLOT_BYTES_4P 733824u  /* 4 ports — today's deployment */
#define E3_SLOT_BYTES_8P 1467648u /* 8 ports */

/* ---- Selector: what to publish (control input, RAN function 2) ----
 *
 * TEMPORAL ONLY in this phase. Spatial selection (port/PRB/symbol subsets) is
 * deliberately deferred; `reserved` is where it lands, so adding it does not
 * break the ABI. See the plan's Out-of-scope section for why temporal is safe
 * for multiple dApps and spatial is not.
 *
 * Delivered by the E3Controller over a jbpf control-input channel. That is a
 * CHANNEL, not a one-shot: the codelet caches the latest value in an array map
 * and re-reads the cache on every invocation, so the controller can retune the
 * selection at any time as subscriptions come and go.
 */
struct e3_slot_sel {
    uint16_t version; /* E3_SLOT_SEL_VERSION */

    /* Slots per radio frame for the live numerology, used as a sanity check
     * against slot_mask's width. 20 at 30 kHz SCS. */
    uint16_t nof_slots_per_frame;

    /* Bit i set => publish slots whose slot_index == i.
     *
     * ZERO MEANS PUBLISH EVERYTHING. That is the default when no dApp has
     * asked for a filter, and it makes behaviour identical to the unfiltered
     * system. Filtering is strictly opt-in and can only ever reduce what the
     * RAN publishes.
     *
     * slot_index is the index within the RADIO FRAME
     * (ocudu slot_point::slot_index() == count % nof_slots_per_frame), so at
     * 30 kHz SCS the valid range is 0..19 — NOT 0..1. Under a 7DS2U TDD
     * pattern only {8, 9, 18, 19} are UL data slots, so a mask selecting
     * anything else yields no indications at all; the controller validates
     * this at subscription time.
     *
     * 64 bits covers numerology <= 2 (40 slots/frame). Widen for mu > 2. */
    uint64_t slot_mask;

    /* Frame-level decimation: publish only when (sfn % sfn_mod) == sfn_offset.
     * sfn_mod 0 or 1 means every frame. Cheapest lever for a dApp that needs
     * only occasional slots, and the cheapest way to bound RX-thread cost. */
    uint16_t sfn_mod;
    uint16_t sfn_offset;

    /* Reserved for spatial selection (port_mask, symbol range, PRB range). */
    uint8_t reserved[20];
};

#define E3_SLOT_SEL_VERSION 1u

/* ---- Attach config: where to publish ----
 *
 * The E3Controller OWNS /e3_ran_buffers — it creates, sizes and tears down the
 * region, and writes the actual grid dimensions into its SharedMemoryHeader.
 * The helper only attaches. Delivered via control input so ownership stays with
 * the controller and the gNB needs no duplicate configuration.
 */
struct e3_shm_cfg {
    uint16_t version; /* E3_SHM_CFG_VERSION */
    uint16_t epoch;   /* bumped when the controller recreates the region; lets
                       * the helper detect and refuse a stale mapping */
    char     name[64]; /* POSIX shm name, e.g. "/e3_ran_buffers" */

    /* Row geometry. The controller owns the region and therefore the geometry;
     * the helper cross-checks these against SharedMemoryHeader::num_fh_samples
     * on attach and refuses a mismatch rather than writing at the wrong stride.
     *
     * Carried explicitly because the NVIDIA header advertises only
     * num_fh_samples (the whole row's uint16 count), from which symbols and
     * subcarriers cannot be recovered independently. */
    uint16_t nof_symbols;     /* 14 */
    uint16_t nof_subcarriers; /* 3276 */

    /* bf16 -> fp16 scale, as applied by ShmIqWriter (E3_CBF16_SCALE).
     *
     * Pushed down rather than read from the environment on the gNB side. The
     * controller owns this calibration, and if the two writers ever disagreed
     * on it the rows would differ with no error — bf16 and fp16 are both 2
     * bytes, so a wrong scale is silently wrong data, not a failure. Making it
     * part of the contract is what keeps the byte-identical invariant true. */
    float scale;

    uint8_t reserved[16];
};

#define E3_SHM_CFG_VERSION 1u

/* ---- Descriptor: what was published ----
 *
 * One per published slot, through the codelet's jbpf output map. Replaces the
 * old ~733 KB `struct uplink_slot_sample`, so the jbpf ring drops from tens of
 * MiB to a couple of KiB and stops being a per-config sizing problem.
 */
struct e3_slot_desc {
    /* gNB hand-off timestamp (CLOCK_REALTIME ns), stamped by the hook caller
     * immediately before the hook fires. RAN anchor for end-to-end latency. */
    uint64_t gnb_ts_ns;

    /* jbpf_time_get_ns() just before submit; same clock, subtracts cleanly. */
    uint64_t codelet_ts_ns;

    /* Bytes the helper wrote into the row. 0 with E3_SLOT_FLAG_TRUNCATED set
     * means it refused rather than writing a prefix. */
    uint32_t bytes_written;

    /* Where the row is: (fh_buffer_index, fh_write_index) into
     * /e3_ran_buffers, exactly as the Indication already carries them. */
    uint32_t fh_write_index;
    uint8_t  fh_buffer_index;

    uint8_t  flags;
    uint16_t sfn;
    uint16_t subframe_id;
    uint16_t slot_id; /* index within the radio frame; see slot_mask above */
    uint16_t sector_id;

    /* Grid shape as OBSERVED, so the consumer can cross-check
     * nof_ports * nof_symbols * nof_subcarriers * 4 == bytes_written. */
    uint16_t nof_ports;
    uint16_t nof_symbols;
    uint16_t nof_subcarriers;

    uint8_t  reserved[6];
};

/* No ladder rung matched the observed grid, so nothing was published. Set
 * rather than silently writing a prefix: publishing 4 of 8 antennas with no
 * signal is a silently wrong measurement, which is worse than a gap. */
#define E3_SLOT_FLAG_TRUNCATED 0x01u

/* The selector was active and this slot passed the filter (diagnostic). */
#define E3_SLOT_FLAG_FILTERED 0x02u

#endif /* JBPF_E3_SLOT_API_H */
