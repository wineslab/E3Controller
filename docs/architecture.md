# Architecture

Two RAN functions, each fed by its own jbpf codelet through its own data-plane
pipeline. The `JbpfDispatcher` runs a single poll loop and routes each jbpf
buffer to the right pipeline by `stream_id`. The slot path (RF=2) carries the
IQ; the section path (RF=1, dashed) is obsolete. RF=1 now carries the PRB block
control into the gNB's scheduler (see [The PRB block path](#the-prb-block-path-rf1-control)).

```
┌─────────────────────────────────── ocudu gNB + jbpf (IPC secondary) ───────────────────────────────────┐
│ hook capture_xran_packet  (OBSOLETE)                       hook capture_uplink_slot  (PHY RX thread)   │
│ codelet ecpri_iq_samples                                   codelet uplink_slot_samples                 │
│ - copies each section's BFP-9 IQ                           - jbpf_e3_publish_slot(): cbf16 → fp16,     │
│   into the jbpf ring                                         row written into /e3_ran_buffers          │
│                                                            - sends a ~64 B e3_slot_desc (which row)    │
└────────────────────────────────────────────────────┬───────────────────────────────────────────────────┘
                                          jbpf IPC shared memory
                                                     ▼
                                E3Controller (IPC primary): JbpfDispatcher
                                    (single poll loop; routes each jbpf
                                     buffer to a pipeline by stream_id)
                                                     │
                       ┌─────────────────────────────┴──────────────────────────────┐
         stream: ecpri_iq  (OBSOLETE)                                        stream: slot_iq
                       ▼                                                            ▼
  ┌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌┐                  ┌──────────────────────────────────────────┐
  ╎ IqPipeline (iq_pipeline.{h,cpp})       ╎                  │ SlotIqPipeline (slot_iq_pipeline.*)      │
  ╎ - LCM IPC codelet load/unload          ╎                  │ - LCM IPC codelet load/unload            │
  ╎ - pushes prb_config → codelet          ╎                  │ - sends e3_shm_cfg + e3_slot_sel         │
  ╎ - SPSC queue + worker thread           ╎                  │ - SPSC queue + worker thread             │
  ╎ - BFP-9 decompression (per section)    ╎                  │ - queues each descriptor by pointer:     │
  ╎                                        ╎                  │   no IQ to copy or convert               │
  └╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌┘                  └──────────────────────────────────────────┘
           const DecompressedSample&                                       const SlotSample&
                       ▼                                                            ▼
  ┌────────────────────────────────────────┐                  ┌──────────────────────────────────────────┐
  │ E3SMSpectrum  (RF=1, sm_spectrum/)     │                  │ E3SMLayer1   (RF=2, l1_kpm/)             │
  │ - PRB-blacklist control: decode, check │                  │ - owns /e3_ran_buffers: creates, sizes   │
  │   it, PrbBlockRelay -> the gNB's       │                  │   and headers it (ShmIqWriter)           │
  │   scheduler (prb_block.enabled, below) │                  │ - checks the geometry on the 1st slot    │
  │ - else the legacy IQ: FFT zero-pad,    │                  │ - emit L1KPM-Indication with the row,    │
  │   APER Spectrum-IQDataIndication       │                  │   in config().encoding (JSON or APER)    │
  │ - RanFuncData, in config().encoding    │                  │ - no controls                            │
  │ - fan out to RF=1 subscribers          │                  │ - fan out to RF=2 subscribers            │
  └────────────────────────────────────────┘                  └──────────────────────────────────────────┘
     → spectrum-sharing dApps                                    → NVIDIA-Aerial L1-KPM dApps, which
                                                                   read the row from /e3_ran_buffers
```

**The slot path (RF=2).** The `uplink_slot_samples` codelet runs in the gNB's
PHY RX thread on every completed uplink slot. It calls the gNB-side helper
`jbpf_e3_publish_slot()`, which converts the resource grid cbf16 → fp16 and
writes it as one row of `/e3_ran_buffers`, then publishes a ~64 B
`e3_slot_desc` that says which row. `SlotIqPipeline` loads the codelet over LCM
IPC, sends it the region's shape (`e3_shm_cfg`) and the slot selection
(`e3_slot_sel`, every slot by default), and queues each descriptor by pointer
to its worker thread. `E3SMLayer1` owns the region (it creates, sizes and
headers it; the gNB helper only attaches) and sends every subscribed dApp an
`L1KPM-Indication` naming the row, in JSON or APER. The dApp reads the IQ from
that row: no IQ crosses the jbpf ring or passes through the controller.

**The section path (RF=1) is obsolete.** `IqPipeline` and `E3SMSpectrum` are
still built and registered, but the Spectrum Service Model has not been updated
with the rest of the controller, and it needs that update before this path is
used again. It is the older design: the `ecpri_iq_samples` codelet copies each
section's BFP-9 IQ through the jbpf ring, the controller decompresses it, and
the SM sends the IQ in-band, APER only.

Service models register a callback with their pipeline and receive each sample
by `const&` (no copies). Both pipelines are constructed eagerly but
**lazy-started**: libe3 calls the SM's `start()` (which boots its pipeline —
codelet load + worker thread) only on the first dApp subscription to that RAN
function.

## Dispatcher Model

The `JbpfDispatcher` is the central routing component:

1. **Single poll loop** — the main thread calls `dispatcher.poll()` which invokes `jbpf_io_channel_handle_out_bufs` once
2. **Stream routing** — each pipeline registers its `stream_id` + callback (`IqPipeline` on `ecpri_iq`, `SlotIqPipeline` on `slot_iq`); the dispatcher routes each jbpf buffer to the pipeline whose stream_id matches
3. **Buffer release** — by default the dispatcher releases each buffer after the callback returns. Both pipelines register with `defer_release` instead: they queue the buffer pointer (zero-copy) and their worker releases it after the fan-out

## Key Components

## The PRB block path (RF=1 control)

A spectrum-sharing dApp sends a Spectrum PRB blacklist control (RF=1, control 1)
with the PRBs where it detected an incumbent. With `prb_block.enabled` the gNB
keeps every data allocation off them, in every slot, UL and DL, as OAI's gNB
does:

```
dApp ──E3 DAppControlAction (RF=1, control 1)──► E3SMSpectrum, libe3 inbound thread
                                                 decode, check the PRBs are on the carrier
                                                          │ PrbBlockRelay::submit()
                                                          ▼
                                                 PrbBlockRelay, main loop: newest block
                                                 → control input block_in, then ACK
                                                          │ jbpf IPC
                                                          ▼
ocudu scheduler, every slot of every cell, before any allocation:
  ctrl hook sched_prb_block → codelet prb_block returns the block in force
  → the grid reports the blocked PRBs as used to PDSCH and PUSCH allocators
  first slot with a new block → applied_out ──► PrbBlockRelay: LIVE_ON_AIR stage
```

The ACK means what OAI's does: the block is in the RAN, not yet applied. With
no codelet loaded the hook returns "no block", so unloading it, on the last
RF=1 unsubscribe or at shutdown, clears the block. The stage records of the
whole loop are in [Stage records](stage-records.md#the-prb-block-loop).

Service-model code is grouped under `src/e3sm/` by component: `sm_spectrum/`
(RF=1), `l1_kpm/` (RF=2), `utils/` (writers + decompression), with the shared
data-plane pipelines at the `src/e3sm/` root and the ASN.1 grammars in `asn/`.

| File | Description |
|---|---|
| `src/e3_controller.cpp` | Main daemon — jbpf IO init, E3 agent, dispatcher poll loop, both pipelines + SM wiring |
| `include/jbpf_dispatcher.h` | Central buffer dispatcher — routes by stream_id, releases buffers |
| `src/e3sm/iq_pipeline.{h,cpp}` | **Obsolete.** RF=1 data plane — `ecpri_iq_samples` codelet load, SPSC queue, worker, BFP-9 decompression → `DecompressedSample` |
| `src/e3sm/slot_iq_pipeline.{h,cpp}` | RF=2 data plane — `uplink_slot_samples` codelet load, `e3_shm_cfg` / `e3_slot_sel` to the codelet, SPSC queue, worker; each `e3_slot_desc` → `SlotSample` (row indices, no IQ) |
| `src/e3sm/sm_spectrum/e3sm_spectrum.{h,cpp}` | Spectrum SM (RF=1) — PRB blacklist control, applied in the gNB's scheduler through PrbBlockRelay (`prb_block.enabled`), + RanFunctionData; without `prb_block.enabled`, the **obsolete** ecpri_iq IQ telemetry (APER `Spectrum-IQDataIndication`, ASN.1 only) via IqPipeline |
| `src/e3sm/prb_block_relay.{h,cpp}` | The PRB block's way into the gNB — `prb_block` codelet load/unload, block → control input `block_in` from the main loop, ACK, the codelet's `applied_out` reports and their stage records |
| `src/e3sm/prb_block_msg.h` | Builds the `e3_prb_block_msg` (275-bit mask) from a decoded control |
| `src/e3sm/sm_spectrum/e3sm_spect_wrapper.{h,cpp}` | C++ wrappers around the Spectrum ASN.1 types (incl. PRB blacklist control decode) |
| `src/e3sm/l1_kpm/e3sm_layer_1.{h,cpp}` | L1-KPM SM (RF=2) — SHM-pointer IQ indications in the configured encoding (JSON or APER) via SlotIqPipeline; writes the optional drop-accounting CSV |
| `src/e3sm/l1_kpm/e3sm_layer1_json.{h,cpp}` | JSON encoder for the L1-KPM (RF=2) indication — `e3sm_layer1::encode_iq_indication_json`, NVIDIA-Aerial-conformant `protocolData` |
| `src/e3sm/l1_kpm/e3sm_layer1_wrapper.{h,cpp}` | APER encoder for `L1KPM-Indication`, and the L1-KPM RanFunctionData: `L1KPM-RanFunctionData` with one stream descriptor per telemetry identifier, over APER, or the same fields as a JSON object over JSON |
| `src/e3sm/l1_kpm/e3sm_layer1_streams.h` | The telemetry identifiers of E3SM-L1-KPM version 1 (stream name, data type), and the ones the controller produces |
| `src/e3sm/utils/e3sm_shm_writer.{h,cpp}` | Creates, sizes and headers the POSIX shm region `/e3_ran_buffers` that the gNB writes the rows into — owned by `E3SMLayer1` |
| `src/e3sm/utils/bfp_decompress.{h,cpp}` | BFP-9 → int16 IQ decompression — called from `IqPipeline::dispatch_sample` |
| `src/e3sm/asn/e3sm_spectrum.asn` | ASN.1 definitions for Spectrum SM — OAI's gNB grammar (`Spectrum-DAppControlData` with `Spectrum-PRBBlacklistControl`, `Spectrum-RanFunctionData`, reports, xApp controls, ...) plus the controller's legacy `Spectrum-IQDataIndication` |
| `src/e3sm/asn/e3sm_layer1.asn` | ASN.1 definitions for L1-KPM SM — module `E3SM-L1-KPM-IEs`, the grammar of the E3SM-L1-KPM specification version 1 (`L1KPM-Indication`, `L1KPM-CellData`, `L1KPM-UeData`, `L1KPM-RanFunctionData`, ...); derived from NVIDIA's E3 schema v1.2.0 |
