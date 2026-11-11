# Known Limitations

## Single encoding per process

The controller serves exactly **one** wire encoding at a time, fixed at startup
by `e3.encoding` (libe3 is built with both encoders, so this is a runtime choice
— see [What the normal build steps do](build.md#what-the-normal-build-steps-do)). To serve both an ASN.1 dApp and a
JSON dApp simultaneously, run two controller instances on different port triples.
This is the deliberate simplification from the earlier dual-channel design: with
a single encoding, `ServiceModel::ran_function_data()` and the indication
fan-out simply read `E3Agent::config().encoding` — no per-dApp encoding lookup,
no race during simultaneous setup.

Note that **RF=1's legacy IQ indication is ASN.1/APER-only** — there is no JSON
encoder for it, so it is only useful under `e3.encoding: asn1` (under `json` the
SM warns once and drops indications). The RF=1 PRB block control works in both. **RF=2 L1-KPM**
supports both encodings.

## PRB block: data channels only

With `prb_block.enabled`, a dApp's PRB block reaches the gNB's scheduler (see
[Usage](usage.md#blocking-the-prbs-a-dapp-detected-prb_block)). Against OAI's gNB,
which applies the same control:

- **Only data allocations avoid the blocked PRBs**: PDSCH and PUSCH, including
  SIB1, RAR, Msg3, Msg4 and paging. PUCCH, PDCCH, SSB, PRACH, SRS and CSI-RS
  keep their PRBs, even inside a blocked range. OAI also keeps SSB, PRACH and
  SRS there; it additionally avoids PDCCH CCEs, drops SR and CSI PUCCH
  occasions, skips CSI-RS on blocked PRBs and moves PUCCH and CSI-RS away with
  an RRC reconfiguration.
- **Attach.** Connected UEs keep their PUCCH and PDCCH. A block that leaves no
  room for SIB1, RAR, Msg4 or paging inside CORESET#0's PRBs stops new attaches
  until it shrinks.
- **Grants decided before the block's first slot still go on air**: DL up to
  `max_processing_delay_slots` later (2.5 ms at 30 kHz with ocudu's default of
  5), UL up to k2 (at most 11) slots after that.
- **UE grants are contiguous**, so a block in mid-band leaves a UE one side of
  it. A HARQ retransmission that no longer fits waits until HARQ gives up.
- **Msg3 retransmissions reuse their RBs.** ocudu sends a Msg3 retransmission
  on the RBs of the first transmission when no other channel uses them, even if
  a block covers them by then: Msg3 avoids a block only on its first
  transmission.
- **No expiry**, as on OAI: `validityPeriod` and `samplingThreshold` are
  ignored. A killed controller leaves the codelet loaded, and the last block in
  force, until the gNB restarts.
- **The dApp learns only the ACK.** When each block took effect is in the
  controller's `[PrbBlockRelay]` line, the gNB log and the stage records.

## Slot buffer size is compile-time in the codelet

The radio geometry is runtime configuration on the controller side (`radio:` in the YAML),
but `MAX_SLOT_IQ_BYTES` in
[`codelets/uplink_slot_samples/uplink_slot_data.h`](../codelets/uplink_slot_samples/uplink_slot_data.h)
is still a compile-time constant — currently `733824`, i.e. 4 ports x 14 symbols x 3276
subcarriers x 4 bytes.

This one is **not** an oversight and cannot be made runtime: it sizes the codelet's
jbpf output-map struct, and the eBPF verifier requires compile-time-known struct sizes.
`src/e3sm/slot_iq_pipeline.h` includes that header, so the controller's view of the sample
is sized by it too.

Consequence: raising the antenna count **above** what the constant covers (e.g. 8x8) needs
the constant changed and the codelet rebuilt and re-verified, even though nothing on the
controller side needs recompiling. Lowering it is fine — a 2x2 config just uses less of the
row.

This does not fail silently. The controller validates its configured geometry against what
the RAN reports in the first slot and refuses to publish on a mismatch, and the gNB-side
publish helper refuses a slot larger than the row rather than writing a prefix.

## No verification at codelet load time

Codelets are verified **offline**, at build time (see [Codelets](../codelets/README.md)). The gNB does
not re-verify on load: jbpf JIT-compiles with ubpf, which emits no memory bounds checks. So
an object that bypasses `make` — hand-copied onto a pod, say — is loaded unchecked.

Mitigations in place: `make` refuses to replace a codelet object that does not verify, and
the gNB-side publish helper range-checks its source pointer at runtime against a window the
hook publishes, independently of whether the codelet was verified. A load-time
`jbpf_verify()` call before the LCM request is the remaining gap.
