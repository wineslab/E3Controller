/*
 * jbpf_e3_prb_block_api.h — wire contract for the PRB block (RAN function 1,
 * Spectrum SM control 1).
 *
 * A dApp names the PRBs where it detected an incumbent, and the gNB keeps every
 * data allocation off them, in every slot, UL and DL, as OAI's gNB does. Shared
 * by:
 *
 *   1. codelets/prb_block/prb_block.c            (keeps the block, answers the hook)
 *   2. the scheduler in ocudu-e3                 (fires the hook, applies the mask)
 *   3. E3Controller's PrbBlockRelay              (sends the block, reads the reports)
 *   4. codelets/verifier/e3_verifier_cli.cpp     (program type of the codelet)
 *
 * The path:
 *
 *   E3Controller --e3_prb_block_msg on block_in--> codelet
 *   scheduler, every slot of every cell, before any allocation:
 *     ctrl hook sched_prb_block(e3_prb_block_slot) --> codelet fills the mask,
 *     returns E3_PRB_BLOCK_APPLY, or E3_PRB_BLOCK_NONE for no block
 *   codelet --e3_prb_block_applied on applied_out--> E3Controller, once per new
 *     block, from the slot that applies it
 *
 * The hook falls back to E3_PRB_BLOCK_NONE when no codelet is loaded, so
 * unloading the codelet clears the block.
 */

#ifndef JBPF_E3_PRB_BLOCK_API_H
#define JBPF_E3_PRB_BLOCK_API_H

#include <stdint.h>

/* ELF section of the codelet: ocudu's JBPF_PROG_TYPE_RAN_MAC_SCHED, whose ctx is
 * struct jbpf_mac_sched_ctx. ctx->data .. ctx->data_end is the hook's
 * e3_prb_block_slot. Only the verifier reads the section name. */
#define JBPF_E3_SEC_RAN_MAC_SCHED "jbpf_ran_mac_sched"

/* PRB indices are relative to the carrier start, as in the Spectrum SM's
 * blacklistedPRBs (0..272) and in the IQ the dApp reads. 5 x 64 bits. */
#define E3_PRB_BLOCK_MAX_PRBS   275u
#define E3_PRB_BLOCK_MASK_WORDS 5u

#define E3_PRB_BLOCK_VERSION 1u

/* What the hook returns. 0 is also jbpf's default when no codelet is loaded. */
#define E3_PRB_BLOCK_NONE  0u
#define E3_PRB_BLOCK_APPLY 1u

/* E3Controller -> codelet, on the control input block_in.
 *
 * Each message replaces the whole set, as a Spectrum PRB blacklist control
 * does; n_prbs == 0 clears it. Bit p of mask (word p / 64, bit p % 64) is
 * carrier PRB p. */
struct e3_prb_block_msg {
    uint32_t id;          /* E3 message id of the dApp's control */
    uint16_t version;     /* E3_PRB_BLOCK_VERSION */
    uint16_t n_prbs;      /* bits set in mask */
    uint64_t ctrl_rx_ns;  /* CLOCK_MONOTONIC, the controller received the control */
    uint64_t mask[E3_PRB_BLOCK_MASK_WORDS];
};

/* The hook's buffer, owned by the scheduler. The scheduler writes the first
 * part before firing the hook; the codelet writes the second part when it
 * returns E3_PRB_BLOCK_APPLY. */
struct e3_prb_block_slot {
    /* Written by the scheduler. */
    uint16_t sfn;
    uint8_t  slot;        /* slot index within the frame */
    uint8_t  reserved0;
    uint16_t cell;        /* DU cell index */
    uint16_t nof_prbs;    /* carrier bandwidth in PRBs */

    /* Written by the codelet. */
    uint32_t id;
    uint16_t n_prbs;
    uint16_t reserved1;
    uint64_t mask[E3_PRB_BLOCK_MASK_WORDS];
};

/* Codelet -> E3Controller, on the output applied_out: the first scheduler slot
 * that applies a new block. */
struct e3_prb_block_applied {
    uint32_t id;          /* the block now in force; older ones taken in the same slot never applied */
    uint16_t n_prbs;
    uint16_t coalesced;   /* messages taken in this slot, 1 normally */
    uint16_t sfn;         /* the scheduler slot that applies it */
    uint8_t  slot;
    uint8_t  reserved0;
    uint16_t cell;
    uint16_t reserved1;
    uint64_t applied_ns;  /* jbpf_time_get_ns() in that slot: CLOCK_MONOTONIC */
    uint64_t ctrl_rx_ns;  /* echoed from the message */
};

#endif /* JBPF_E3_PRB_BLOCK_API_H */
