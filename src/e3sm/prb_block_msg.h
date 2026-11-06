/*
 * The message PrbBlockRelay sends the prb_block codelet, built from a decoded
 * Spectrum PRB blacklist control. On its own, without jbpf, so that the unit
 * test can check the bits.
 */
#pragma once

#include "../../codelets/include/jbpf_e3_prb_block_api.h"

#include <cstdint>
#include <vector>

namespace e3sm_pipeline {

/* The block of control `id`, received at rx_ns (CLOCK_MONOTONIC): bit p of mask
 * is carrier PRB p. A PRB listed twice counts once; one outside the mask is
 * dropped (the Spectrum SM rejects controls that name one). An empty list is a
 * clear (n_prbs == 0). */
inline struct e3_prb_block_msg make_prb_block_msg(uint32_t id, const std::vector<int>& prbs, uint64_t rx_ns)
{
    struct e3_prb_block_msg msg = {};
    msg.id         = id;
    msg.version    = E3_PRB_BLOCK_VERSION;
    msg.ctrl_rx_ns = rx_ns;
    for (const int prb : prbs) {
        if (prb < 0 || static_cast<unsigned>(prb) >= E3_PRB_BLOCK_MAX_PRBS) {
            continue;
        }
        const uint64_t bit  = 1ULL << (prb % 64);
        uint64_t&      word = msg.mask[prb / 64];
        if ((word & bit) == 0) {
            word |= bit;
            msg.n_prbs++;
        }
    }
    return msg;
}

}  // namespace e3sm_pipeline
