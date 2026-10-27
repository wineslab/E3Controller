# Service models

The controller serves two RAN functions, one Service Model each:

| RAN function | Service model | Code | For | Encodings |
|---|---|---|---|---|
| RF=1 | Spectrum (OAI's grammar) | `src/e3sm/sm_spectrum/` | dApps that report PRBs to an xApp; the legacy IQ path | JSON or ASN.1 (APER); the legacy IQ indication APER only |
| RF=2 | L1-KPM | `src/e3sm/l1_kpm/` | L1 spectrum sensing | JSON or ASN.1 (APER) |

How they are fed is in [Architecture](architecture.md); what they do not do yet
is in [Known limitations](known-limitations.md).

## L1-KPM (RF=2): E3SM-L1-KPM version 2

The grammar in `src/e3sm/asn/e3sm_layer1.asn` is the one of the E3SM-L1-KPM
specification, version 2 (libe3 `docs/e3sm/l1-kpm/`), copied unchanged. Its JSON
keys are those of NVIDIA Aerial's public E3 message schema v1.2.0 (see
[Attribution](../README.md#attribution)).
- **Slot-shared values at the root.** One indication carries `timestamp`, `sfn`,
  `slot` and `n_cells` there.
- **Cell values in `cells`.** Everything of the one cell this RAN node serves is
  in the single entry of `cells`.

```json
{"timestamp": 1234567890123, "sfn": 512, "slot": 9, "n_cells": 1,
 "cells": [{"iq_samples": {"shm_name": "/e3_ran_buffers", "fh_buffer_index": 0, "fh_write_index": 17},
            "n_rx_ant": 4, "valid_symbol_mask": 16383}]}
```

APER carries the same fields in `L1KPM-Indication` / `L1KPM-CellData`. The
controller produces telemetry IDs 1 (`iq_samples`), 4, 5, 6, 8 (`n_rx_ant`), 10
(`n_cells`) and 1000 (`valid_symbol_mask`), and sends all of them to every
subscriber, whichever IDs it asked for.

**Uplink symbols (`validSymbolMask`).** Bit s is 1 if symbol s of the slot is
uplink, 0 for the DL and guard symbols of a mixed slot, and all bits are set on
FDD.
- **Where it comes from.** The gNB computes it from its TDD pattern when the slot
  hook fires, with the same call it uses to ask the RU for uplink symbols. The
  codelet passes it through (see
  [`codelets/uplink_slot_samples/README.md`](../codelets/uplink_slot_samples/README.md)).
- **How the dApp uses it.** The shared-memory row still holds all 14 symbols; the
  dApp uses the mask to skip the ones that carry no uplink. A dApp should treat a
  missing mask as `0x3FFF` (all symbols), which is what an older RAN sends.

**Not sent:**
- `cell_id` (ID 7): the codelet does not surface the PCI.
- `n_ue` (ID 34) and `ues`: `n_ue` counts the `ues` entries an indication
  carries, and the controller carries none yet.

**Breaking change from version 1.** Version 1 had a flat root (`iq_samples`,
`n_rx_ant`, `valid_symbol_mask` next to `sfn`), and that layout is gone in both
encodings. A dApp reads `cells[0].iq_samples` now, as NVIDIA's own dApps do since
schema v1.2.0. An ASN.1 dApp needs the version 2 grammar. The slot hook context
(`jbpf_ran_slot_ctx`) also changed, so build the gNB and the codelets from
matching trees.

**Indications for slots without PUSCH.** The specification's PUSCH path runs
only for slots in which a cell receives PUSCH. This controller sends an
indication for every slot the gNB publishes, including the slots no UE
transmits in (`slot_filter.ue_free_only`, see [Usage](usage.md)). That case is
an open item of the specification.

## RAN function data (RF=2)

The setup response describes RF=2 with `L1KPM-RanFunctionData`: `name`
`L1-KPM`, `version` 2, `description`, and one stream descriptor for every
telemetry identifier of the specification
([`e3sm_layer1_streams.h`](../src/e3sm/l1_kpm/e3sm_layer1_streams.h)).
- **IDs the controller produces** are `available`:
  - ID 1 goes through the shared memory region. The descriptor gives its
    `memory_key` and, for one of the two buffers, `memory_size_bytes` and
    `num_rows`, computed from `shm.size_bytes` and the row size.
  - The other produced IDs are inline (`protocolData`, keyword = the JSON key).
- **Every other ID** is `unavailable`, with no delivery.

| Encoding | `ranFunctionData` bytes |
|---|---|
| `asn1` | APER `L1KPM-RanFunctionData`, about 3 kB |
| `json` | `{"name":"L1-KPM","version":2,"description":...,"streams":[{"telemetryIdentifier":1,"stream_id":"iq_samples","data_type":"array(int16)","status":"available","delivery_method":{...}}, ...]}`, about 10 kB |

libe3 0.2.x writes `ranFunctionData` as a hex string of these bytes in a JSON
setup response. With [wineslab/libe3#118](https://github.com/wineslab/libe3/pull/118)
it nests the object as JSON.

## Spectrum (RF=1)

The grammar, `src/e3sm/asn/e3sm_spectrum.asn`, is the Spectrum Service Model of
OAI's E2SM-DAPP bridge, copied unchanged from wineslab/spear-flexric
(`src/sm/dapp_sm/e3/service_models/spectrum_sm/asn/defs/e3sm_spectrum.asn`).
Keep it identical to that file: the dApp, the controller and the xApp all encode
its types. The adaptive_cpu dApp's copy (`spectrum_report_sm`) agrees with it on
reports and controls. OAI's gNB has a third copy whose `Spectrum-PRBBlacklistReport`
is extensible; it does not matter there, because the gNB never decodes a report.

One section at the end is the controller's own: `Spectrum-IQDataIndication`, the
indication of the **obsolete** eCPRI IQ path (the `ecpri_iq_samples` codelet). Its
bytes are unchanged from the previous grammar, and it goes away with that path.

What the controller encodes and decodes itself:

| Message | APER (`asn1`) | JSON (`json`) |
|---|---|---|
| RAN function data | `Spectrum-RanFunctionData` | `{"name":"spectrum_sm","version":1,"description":"..."}` |
| dApp control (id 1, PRB blacklist) | `Spectrum-DAppControlData` | `{"controlType":"prbBlacklist","controlPayload":{"prbBlacklistControl":{"blacklistedPRBs":[...]}}}`, as OAI's gNB takes it |
| IQ indication (legacy) | `Spectrum-IQDataIndication` | not sent |

The other alternatives of `Spectrum-DAppControlPayload` (a sensing policy, for
example) are refused with a NACK. Controls are acknowledged but not applied yet,
see [Known limitations](known-limitations.md).

`Spectrum-SensingPolicyControl` has a BOOLEAN, and libe3 ships no BOOLEAN runtime,
so `src/e3sm/asn/CMakeLists.txt` compiles asn1c's own (only while libe3 has none).
It generates the types with libe3's codec set (`-no-gen-BER -no-gen-JER
-no-gen-OER`), because libe3's `asn_config.h` decides the layout of every type
table.

## Adding a New Service Model

There are two flavours of SM, depending on what data it needs:

**Controls-only SM** (no IQ pipeline subscription) — declares `telemetry_ids() = {}`:
1. Extend `libe3::ServiceModel`; declare `telemetry_ids() = {}` and the relevant `control_ids()`.
2. Implement `handle_control_action()` and `ran_function_data()`.
3. Add ASN.1 types in `src/e3sm/asn/` and wrapper encoders alongside the existing ones.
4. Register in `e3_controller.cpp`:
   ```cpp
   agent.register_sm(std::make_unique<MyControlSM>(agent));
   ```

**IQ-consuming SM** — see `E3SMSpectrum` (RF=1) and `E3SMLayer1` (RF=2):
1. Same `libe3::ServiceModel` extension, but with `telemetry_ids() = {1}`.
2. Take a pipeline reference in your constructor — `IqPipeline&` (per-section,
   BFP-decompressed `ecpri_iq_samples` → `DecompressedSample`) or `SlotIqPipeline&`
   (per-slot `uplink_slot_samples` → `SlotSample`). Add a new pipeline if your
   codelet emits a different stream.
3. In `init()`, register the consumer: `pipeline.register_consumer([this](const auto& s){ on_sample(s); });`. Doing this in `init()` (one-shot at SM registration) — not `start()` — keeps the consumer wired across libe3's start/stop cycles.
4. In `start()`, call `pipeline.start()`. In `stop()`, call `pipeline.stop()`. This is what makes codelet load + worker thread *lazy*: nothing connects to LCM IPC until the first dApp subscribes to this SM.
5. Implement `on_sample()` and any per-slot/per-symbol assembly logic, then encode once in the agent's configured encoding (`agent.config().encoding`) and emit one indication per subscriber via `get_subscribers()` + `emit_outbound()`.
6. Register in `e3_controller.cpp` — no explicit start needed; libe3 calls `SM::start()` on first subscription and `SM::stop()` on last unsubscribe:
   ```cpp
   agent.register_sm(std::make_unique<MyTelemetrySM>(iq_pipeline, agent, ...));
   // … no manual start; libe3 will call it lazily on first dApp subscribe …
   ```

> **Launch order matters:** the LCM IPC socket the pipeline uses to load codelets belongs to ocudu, so it must be running before the *first dApp subscribes* (controller can boot earlier — it doesn't touch LCM IPC until then).

> **Note:** Do NOT release jbpf buffers in your SM — the pipeline that delivered the sample owns them and releases each one after the fan-out.
