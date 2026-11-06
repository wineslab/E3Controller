/*
 * prb_block.c
 *
 * Codelet attached to the ocudu control hook `sched_prb_block`, which the
 * scheduler fires at the start of every slot of every cell, before any
 * allocation. It holds the PRB block the E3Controller sent, the PRBs a dApp
 * asked to keep data allocations off, and hands it to the scheduler on every
 * call:
 *
 *   - E3_PRB_BLOCK_APPLY with the mask in the hook's buffer: the scheduler
 *     keeps PDSCH and PUSCH (UE data, SIB1, RAR, Msg3, Msg4, paging) off those
 *     PRBs in this slot, DL and UL;
 *   - E3_PRB_BLOCK_NONE: no block.
 *
 * A new block from the E3Controller replaces the old one whole, as OAI's
 * Spectrum SM does; one with no PRBs clears it. The first slot that applies a
 * new block reports it on applied_out, which is how the E3Controller learns
 * when the block took effect.
 *
 * With the codelet unloaded the hook returns jbpf's default, 0 =
 * E3_PRB_BLOCK_NONE, so unloading clears the block.
 *
 * Contract: codelets/include/jbpf_e3_prb_block_api.h, shared with ocudu.
 */

#include "jbpf_defs.h"
#include "jbpf_helper.h"
#include "jbpf_srsran_contexts.h"

#include "jbpf_e3_prb_block_api.h"

/* The most messages one call takes. The loop has to be bounded for the
 * verifier; a dApp sends a block only when its list changes, and the hook fires
 * every slot. Messages left over wait for the next slot. */
#define BLOCKS_PER_CALL 8

/* ---- Maps ---- */

/* The blocks the E3Controller sends. The name and the stream id are a contract
 * with prb_block.yaml and src/e3sm/prb_block_relay.cpp. */
jbpf_control_input_map(block_in, struct e3_prb_block_msg, 16);

/* The block in force. Zeroed at load, i.e. no block. */
struct jbpf_load_map_def SEC("maps") block_state = {
    .type = JBPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(int),
    .value_size = sizeof(struct e3_prb_block_msg),
    .max_entries = 1,
};

/* One report per new block. Same contract as block_in. */
jbpf_output_map(applied_out, struct e3_prb_block_applied, 16);

/* ---- Main codelet entry ---- */

SEC(JBPF_E3_SEC_RAN_MAC_SCHED)
uint64_t
jbpf_main(void* state)
{
    struct jbpf_mac_sched_ctx* ctx = (struct jbpf_mac_sched_ctx*)state;
    int                        zero = 0;

    struct e3_prb_block_slot* io  = (struct e3_prb_block_slot*)(uintptr_t)ctx->data;
    const uint8_t*            end = (const uint8_t*)(uintptr_t)ctx->data_end;
    if ((const uint8_t*)io + sizeof(*io) > end) {
        return E3_PRB_BLOCK_NONE;
    }

    struct e3_prb_block_msg* cur = (struct e3_prb_block_msg*)jbpf_map_lookup_elem(&block_state, &zero);
    if (!cur) {
        return E3_PRB_BLOCK_NONE;
    }

    /* The newest message wins: each one replaces the whole set. */
    struct e3_prb_block_msg msg;
    uint16_t                taken = 0;
    for (int i = 0; i < BLOCKS_PER_CALL; i++) {
        /* 1 means a message was waiting and is now in msg. */
        if (jbpf_control_input_receive(&block_in, &msg, sizeof(msg)) != 1) {
            break;
        }
        if (msg.version != E3_PRB_BLOCK_VERSION) {
            continue;
        }
        *cur = msg;
        taken++;
    }

    if (taken > 0) {
        /* This slot is the first to apply it. */
        struct e3_prb_block_applied* out = (struct e3_prb_block_applied*)jbpf_get_output_buf(&applied_out);
        if (out) {
            out->id         = cur->id;
            out->n_prbs     = cur->n_prbs;
            out->coalesced  = taken;
            out->sfn        = io->sfn;
            out->slot       = io->slot;
            out->reserved0  = 0;
            out->cell       = io->cell;
            out->reserved1  = 0;
            out->applied_ns = jbpf_time_get_ns();
            out->ctrl_rx_ns = cur->ctrl_rx_ns;
            jbpf_send_output(&applied_out);
        }
    }

    if (cur->n_prbs == 0) {
        return E3_PRB_BLOCK_NONE;
    }

    io->id        = cur->id;
    io->n_prbs    = cur->n_prbs;
    io->reserved1 = 0;
    for (int w = 0; w < E3_PRB_BLOCK_MASK_WORDS; w++) {
        io->mask[w] = cur->mask[w];
    }
    return E3_PRB_BLOCK_APPLY;
}
