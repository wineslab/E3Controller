# Service models

The controller serves two RAN functions, one Service Model each:

| RAN function | Service model | Code | For | Encodings |
|---|---|---|---|---|
| RF=1 | Spectrum (**obsolete**: needs updating before use) | `src/e3sm/sm_spectrum/` | public Spectrum Sharing dApps | ASN.1 (APER) only |
| RF=2 | L1-KPM | `src/e3sm/l1_kpm/` | L1 spectrum sensing | JSON or ASN.1 (APER) |

How they are fed is in [Architecture](architecture.md); what they do not do yet
is in [Known limitations](known-limitations.md).

## L1-KPM (RF=2) and NVIDIA's schema

The payload follows NVIDIA Aerial's public E3 message schema (see
[Attribution](../README.md#attribution)), with these differences.

**Deviations (out of scope for this release):** `cellId` (`cell_id`) and
`validSymbolMask` (`valid_symbol_mask`) are `OPTIONAL` in `e3sm_layer1.asn` and
not sent yet, in either encoding: the codelet does not surface the cell id, and
the mask needs the TDD pattern. `nRxAnt` (`n_rx_ant`) is sent in both. A dApp
that reads the JSON mask should treat a missing one as `0x3FFF` (all symbols).

**ASN.1 grammar change.** `validSymbolMask` is a new `OPTIONAL` field, so every
APER `L1KPM-Indication` carries one more presence bit, even when the field is
absent. A dApp built on the previous `e3sm_layer1.asn` misreads the new
indications: rebuild it with this grammar. JSON is not affected.

## RAN function data (RF=2)

The setup response describes RF=2 with `L1KPM-RanFunctionData`
(`name`, `version`, `description`), with the same values as OAI's L1-KPM SM:

| Encoding | `ranFunctionData` bytes |
|---|---|
| `asn1` | APER `L1KPM-RanFunctionData` |
| `json` | `{"name":"L1-KPM","version":1,"description":"L1-KPM SM: post-FFT IQ telemetry."}`, the same fields in the ASN.1 order, byte-for-byte what OAI sends |

libe3 0.2.x writes `ranFunctionData` as a hex string of these bytes in a JSON
setup response; with [wineslab/libe3#118](https://github.com/wineslab/libe3/pull/118)
it nests the object as JSON. NVIDIA's schema has an array of stream descriptors
here instead ([wineslab/libe3#86](https://github.com/wineslab/libe3/issues/86));
the Aerial dApps only print the field, so either form works with them.

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
