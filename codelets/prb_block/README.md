# prb_block

Keeps the gNB's data allocations off the PRBs a dApp asked to block with a
Spectrum PRB blacklist control (RAN function 1, control 1), in every slot, DL
and UL.

| | |
|---|---|
| Hook | `sched_prb_block`, a jbpf **control** hook. ocudu's scheduler fires it at the start of every slot of every cell, before any allocation |
| Program type | `jbpf_ran_mac_sched` (ocudu's `JBPF_PROG_TYPE_RAN_MAC_SCHED`, ctx `jbpf_mac_sched_ctx`) |
| Input | `block_in`: `e3_prb_block_msg`, the whole set of blocked PRBs; no PRBs clears it |
| Output | `applied_out`: `e3_prb_block_applied`, once per new block, from the first slot that applies it |
| Contract | `../include/jbpf_e3_prb_block_api.h`, byte-identical in ocudu's `ocudu_janus/e3/` |

The hook's ctx points at a buffer the scheduler owns (`e3_prb_block_slot`). The
codelet writes the mask there and returns `E3_PRB_BLOCK_APPLY`, or returns
`E3_PRB_BLOCK_NONE`. The scheduler then reports the blocked PRBs as used to every
data allocator: UE PDSCH/PUSCH, SIB1, RAR, Msg3, Msg4 and paging. PUCCH, PDCCH,
SSB, PRACH, SRS and CSI-RS keep their PRBs.

With no codelet loaded the hook returns jbpf's default, 0, which is
`E3_PRB_BLOCK_NONE`: unloading the codelet clears the block.

The E3Controller loads it when the first dApp subscribes to RAN function 1 with
`prb_block.enabled: true`, and unloads it when the last one leaves. See
`src/e3sm/prb_block_relay.h`.
