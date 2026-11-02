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

Note that **RF=1 Spectrum is ASN.1/APER-only** — there is no JSON encoder for its
in-band IQ indication, so it is only useful under `e3.encoding: asn1` (under
`json` the SM warns once and drops indications). **RF=2 L1-KPM**
supports both encodings.

## Missing PRB blacklist control

`E3SMSpectrum::handle_control_action()` (RF=1) decodes the dApp's
`Spectrum-DAppControlData`, in the agent's encoding, logs the requested PRB list,
and sends a POSITIVE ACK back to the dApp. The actual application of the
blacklist to the RAN scheduler is not implemented: the
`// TODO: Apply the PRB blacklist to the RAN` line in
[`src/e3sm/sm_spectrum/e3sm_spectrum.cpp`](../src/e3sm/sm_spectrum/e3sm_spectrum.cpp)
marks the spot. dApps that rely on the control side-effect (rather than just the
ACK) will not see scheduler behaviour change.

## dApp reports reach the xApp, nothing comes back

dApp reports go to an xApp through the gNB's E2SM-DAPP bridge
([Service models](service-models.md#dapp-reports-to-an-xapp-e2sm-dapp)), but:

- xApp controls are not relayed to the dApp yet: the gNB answers them with a
  failure ("no E3 link").
- E2SM-DAPP report style 2 (the map of dApps and their E3 RAN functions) never
  sends an indication.
- A report larger than 2048 bytes is dropped (`too_big`). A full 273-PRB report is
  about 310 bytes.
- While no xApp has a data subscription, the controller drops the dApp's
  reports (`no_xapp`); the dApp keeps sending them. When an xApp subscribes,
  forwarding starts with the codelet's first presence message (within 0.5 ms);
  the reports dropped before are not sent later.
- After a gNB restart the relay codelet is gone and the controller does not load
  it again: the presence messages stop, so reports are dropped (`no_xapp`) until
  the controller is restarted too.
- The bridge keeps the last 64 reports for each subscription. A subscription whose
  indications cannot be sent fast enough loses the oldest ones, counted as
  `overrun` in the gNB's `[e3_dapp]` line.

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
