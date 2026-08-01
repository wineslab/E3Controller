# E3Controller

A standalone C++ daemon that bridges ocudu's jbpf shared memory (IPC primary) with the E3 protocol via [libe3](https://github.com/wineslab/libe3). It receives I/Q sample data from jbpf codelets and exposes it as E3 Service Model indications to subscribed dApps.

The controller serves **one** wire encoding at a time, over a configurable link layer and
transport. All of that — along with the radio geometry, SHM layout, jbpf IPC settings and
thread pinning — lives in a single YAML configuration file; see [Usage](#usage).

> To use this E3Controller you need to build and run [this version](https://github.com/wineslab/ocudu-e3) of OCUDU.

## Build

Everything goes through the top-level [`build.sh`](build.sh). Two modes:

```bash
git clone --recurse-submodules git@github.com:wineslab/E3Controller.git
cd E3Controller

# Fresh Debian/Ubuntu machine (installs every dep first, then builds):
./build.sh --install-deps

# Machine that already has the toolchain in place:
./build.sh
```

The binary lands at `out/bin/e3_controller`, and the offline codelet verifier at
`out/bin/e3_verifier_cli`.

### `--install-deps` (or `-d`)

Use this on a clean Debian/Ubuntu install (24.04 tested; 22.04 works with the
source fallback for nlohmann_json). The flag drives a **step 0** that runs
before the normal build and needs `apt-get`. The script uses `sudo` when it is
not run as root; on minimal container images that ship without `sudo` it
installs the `sudo` package first so the delegation below can succeed.

Step 0 does, in order:

1. `apt-get update`; installs `sudo` if we are root and it is missing.
2. `git submodule update --init libe3` so libe3's own installer is present.
3. **Delegates to `libe3/build_libe3 -I`.** libe3's helper already knows the
   full dependency set (apt: `build-essential`, `cmake`, `pkg-config`, `git`,
   `ninja-build`, `autoconf`, `automake`, `libtool`, `m4`, `bison`, `flex`,
   `libzmq3-dev`, `nlohmann-json3-dev`, `libsctp-dev`) and builds `asn1c` from
   the `mouse07410` fork at the exact commit libe3's E3AP grammar expects,
   installing it under `/opt/asn1c/`.
4. Adds the extras libe3 does not pull in but the E3Controller / jbpf build
   needs: `python3`, `python3-pip`, `python3-dev`, `file`, `ca-certificates`.
5. Puts `/opt/asn1c/bin` on `PATH` so `cmake` finds `asn1c` in the same shell
   without a manual export.

On non-Debian distros the flag refuses to run — install the equivalent
packages by hand and re-run `./build.sh` without `--install-deps`.

### What the normal build steps do

Whether or not `--install-deps` was used, `build.sh` then runs:

1. `git submodule update --init --recursive` (fetches libe3 @ tag `0.0.6` and
   jbpf).
2. `jbpf/init_and_patch_submodules.sh` to bring in jbpf's third-party
   dependencies.
3. Configure + build libe3 with both encoders
   (`-DLIBE3_ENABLE_ASN1=ON -DLIBE3_ENABLE_JSON=ON -DLIBE3_BUILD_EXAMPLES=OFF
   -DLIBE3_BUILD_TESTS=OFF`), stage `asn1c`'s `BOOLEAN.*` skeletons into
   `libe3/build/messages/` (toolchain shim — libe3's E3AP grammar does not use
   `BOOLEAN` and the `mouse07410` fork skips them, so we supply the reference
   copies), then `sudo cmake --install libe3/build` to `/usr/local`.
4. Configure + build the E3Controller; jbpf is compiled in-tree via
   `add_subdirectory`.

`e3.encoding` in the config is therefore a pure runtime choice, because libe3 is built
with both encoders.

### Overrides & re-runs

- `JOBS=N ./build.sh` — parallelism (defaults to `nproc`).
- `ASN1C_SKELETON_DIR=<dir> ./build.sh` — override where `BOOLEAN.*` are
  copied from. Default probe order is
  `/opt/asn1c/share/asn1c` → `/usr/local/share/asn1c` → `/usr/share/asn1c`.
- If you re-build without `--install-deps` on a host where the earlier run
  put `asn1c` under `/opt/asn1c/`, the script re-adds `/opt/asn1c/bin` to
  `PATH` automatically before invoking `cmake`.
- `rm -rf libe3/build` before re-running is enough to force the `BOOLEAN.*`
  shim to re-stage; `cmake` picks the rest up incrementally.

### ASN.1 Code Generation

The build automatically:
1. Runs `asn1c` on the ASN.1 grammars in `src/e3sm/asn/` (`e3sm_spectrum.asn` for
   RF=1, `e3sm_layer1.asn` for RF=2) to generate C encoder/decoder code
2. Compiles the generated code into a static library (`e3sm_asn`) linked to `e3_controller`

Generated files go into `build/asn1c_generated/` and are **not** tracked in git.

## Usage

> **Important:** E3Controller (IPC primary) must start **before** ocudu (IPC secondary).

```bash
./out/bin/e3_controller --config configs/e3_controller.yaml
```

`--config` is the **only** argument. It replaced 20 command-line options, deliberately:
two configuration mechanisms invite the two to disagree, and the radio geometry has to be
stated in exactly one place.

A fully documented example ships at [`configs/e3_controller.yaml`](configs/e3_controller.yaml).
Unknown keys are a **startup error**, not a warning — a typo'd `nof_port:` that was
silently ignored would leave the controller sizing rows for the default while you believed
you had configured something else.

### Example

100 MHz / 30 kHz SCS, 4x4, ASN.1 encoding, three pinned cores:

```yaml
# radio geometry — MUST match the running gNB (validated against it at runtime)
radio:
  nof_ports:   4        # UL antenna ports
  nof_prbs:    273      # 100 MHz @ 30 kHz SCS
  nof_symbols: 14       # per slot
  scs_khz:     30       # also fixes slots/frame (20)

# /e3_ran_buffers — owned by the controller, read by the dApp
shm:
  name:        /e3_ran_buffers
  size_bytes:  1073741824      # 1 GiB
  cbf16_scale: 1.0             # bf16 -> fp16 scale; must be > 0
  writer:      controller      # controller | gnb  (exactly one may write)

# must agree with the gNB's own `jbpf:` section
jbpf:
  ipc_name:          e3_controller
  run_path:          /dev/shm
  mem_size_bytes:    1073741824
  lcm_socket_path:   /tmp/jbpf/jbpf_lcm_ipc
  codelet_base_path: /workspace/e3_release/E3Controller/codelets

e3:
  encoding:        asn1        # asn1 | json
  link_layer:      zmq         # zmq | posix
  transport:       tcp         # tcp | ipc | sctp
  setup_port:      9990
  publisher_port:  9991
  subscriber_port: 9999

threads:
  poll_core:        2
  worker_core:      3
  publisher_core:   4
  poll_interval_us: 100        # ignored when poll_core >= 0 (busy-poll)

logging:
  stats_log_path: ""           # empty disables the per-slot stage CSV

target_slot: -1                # -1 = every UL slot
```

For a **JSON / cuBB dApp** (such as `adaptive_cpu`), change the `e3:` section — the port
convention differs, so keep one config file per encoding rather than trying to override
individual values at launch:

```yaml
e3:
  encoding:        json
  link_layer:      zmq
  transport:       tcp
  setup_port:      5555
  publisher_port:  5556
  subscriber_port: 5557
```

For a **2x2** deployment, only `radio.nof_ports` changes — the row stride, header and all
derived sizes follow from it, with no recompile:

```yaml
radio:
  nof_ports:   2
  nof_prbs:    273
  nof_symbols: 14
  scs_khz:     30
```

### Configuration

| Section | Keys | Notes |
|---|---|---|
| `radio` | `nof_ports`, `nof_prbs`, `nof_symbols`, `scs_khz` | **Must match the running gNB.** Validated against the RAN — see below. |
| `shm` | `name`, `size_bytes`, `cbf16_scale`, `writer` | The `/e3_ran_buffers` region the controller owns and the dApp reads; `writer` picks which process converts and writes rows (see below) |
| `jbpf` | `ipc_name`, `run_path`, `mem_size_bytes`, `lcm_socket_path`, `codelet_base_path` | Must agree with the gNB's own `jbpf:` YAML section |
| `e3` | `encoding`, `link_layer`, `transport`, `setup_port`, `publisher_port`, `subscriber_port` | `encoding` is `asn1` or `json`; JSON/cuBB dApps expect ports 5555/5556/5557 |
| `threads` | `poll_core`, `worker_core`, `publisher_core`, `poll_interval_us` | `-1` = no pinning (the poll thread then sleeps rather than busy-spinning) |
| `logging` | `stats_log_path` | Empty disables the per-slot stage CSV |
| *(top level)* | `target_slot` | Forward only this slot index within a 10 ms frame; `-1` forwards every UL slot |

#### Who writes the rows (`shm.writer`)

| Value | Data path | Requires |
|---|---|---|
| `controller` (default) | codelet copies the grid into the jbpf ring -> the controller converts cbf16 -> fp16 into `/e3_ran_buffers` | any gNB |
| `gnb` | codelet calls the gNB-side publish helper, which converts and writes the row in the PHY RX thread; the jbpf ring carries only a ~64 B descriptor | a gNB with the E3 helpers registered and the descriptor-emitting codelet |

`gnb` halves total memory traffic (3 MB -> 1.5 MB per slot) by fusing the copy and
the conversion into one pass, and takes the controller out of the data plane entirely.

**Exactly one process may write.** Both writers keep their own ring cursor, so if both
were active they would overwrite each other's rows with no error anywhere. The switch makes
that structurally impossible: in `gnb` mode the controller's `publish_row_cbf16`
hard-refuses and the row indices come from the RAN instead.

Either way the controller **owns** the region — it creates, sizes, zero-fills, headers and
tears it down; the helper only attaches (`O_RDWR` without `O_CREAT`, so it cannot race the
owner into creating one with the wrong shape).

#### Radio geometry is checked, not trusted

`radio:` has to be declared up front because the SHM region must exist and be sized before
the codelet can be loaded. But the RAN is the real source of truth: the per-slot hook
context carries `nof_ports` / `nof_symbols` / `nof_subcarriers`, so the controller compares
your configuration against the first slot it receives and **refuses to publish on a
mismatch**, naming both sides.

The two directions are not symmetric, which is why the check exists:

- Declaring **fewer** ports than the gNB sends is already safe — the gNB-side publish
  helper refuses the oversized slot and the codelet reports it.
- Declaring **more** is what needs catching: the surplus antennas are written as silence,
  and the dApp cannot distinguish that from a genuinely quiet antenna. A plausible-looking
  wrong spectrum, with nothing to notice.

> **`E3_CBF16_SCALE` no longer has any effect.** The bf16 -> fp16 scale moved into
> `shm.cbf16_scale`, because the gNB-side publish helper needs the *same* value and a
> disagreement would produce different rows with no error at all — bf16 and fp16 are both
> 2 bytes, so a wrong scale is silently wrong data rather than a failure.

> **Encoding vs. libe3 build.** When libe3 is built with both encoders
> (the recommended build — see [libe3 (git submodule)](#libe3-git-submodule)),
> `e3.encoding` is a pure runtime choice. If libe3 was built with only one
> encoder, it must match, or the outbound encoder rejects every PDU.

#### Timing logs

Off by default; enabled by setting `logging.stats_log_path`. Written by `E3SMLayer1`, one
row per published UL slot: `slot_seq, gnb_to_codelet_us, codelet_to_dispatch_us,
dispatch_to_handler_us, shm_ns, encode_ns, emit_ns, nof_subc, iq_bytes`.

An example launcher is available [here](start_e3controller_example.sh).

## Codelets

The jbpf codelets are built and verified **in this repository**, under
[`codelets/`](codelets/) — there is no longer any dependency on an external SDK tree.

```bash
cd codelets
make            # build + verify every codelet
make verify     # re-verify existing objects
make show-config  # resolved paths, [ok]/[MISSING] per include root
```

Requires `clang` with the BPF target, and `e3_verifier_cli` from the main build. The
context contract (`jbpf_srsran_contexts.h`) is imported from the ocudu checkout rather than
copied — point `OCUDU_DIR` at it if it is not a sibling of this repo:

```bash
make OCUDU_DIR=/path/to/ocudu-e3
```

`make check-contract` fails if a local copy has drifted from ocudu's.

### Verification is mandatory

`make` **fails** if a codelet does not verify, and that is not belt-and-braces: the gNB
does no verification at load time. jbpf's load path is `ubpf_load_elf_ex` followed by
`ubpf_compile`, PREVAIL is not on it, and ubpf's JIT emits no memory bounds checks — only
its interpreter does, and jbpf uses the JIT. So this build step is the only memory-safety
gate these codelets ever pass through.

`codelets/verifier/e3_verifier_cli.cpp` registers the ocudu program types and the E3 helper
prototypes on top of jbpf's built-ins, deriving its context descriptors with `offsetof`
from ocudu's own header so the contract has one source of truth. It replaces the SDK
image's `srsran_verifier_cli`, which models only the 27-byte `jbpf_ran_ofh_ctx` and
therefore cannot verify the per-slot codelet at all.

Helper and program-type IDs live in [`codelets/include/jbpf_e3_ids.h`](codelets/include/jbpf_e3_ids.h),
shared by the codelets, the verifier, and the gNB-side helper registration. If those three
disagree, a codelet that verifies cleanly still fails to load.

## Architecture

Two RAN functions, each fed by its own jbpf codelet through its own data-plane
pipeline. The `JbpfDispatcher` runs a single poll loop and routes each jbpf
buffer to the right pipeline by `stream_id`:

```
ocudu + jbpf (IPC Secondary)  ──shared memory──►  E3Controller (IPC Primary)
                                                        │
                                                  JbpfDispatcher
                                          (single poll loop; routes each jbpf
                                           buffer to a pipeline by stream_id)
                                                        │
                 ┌──────────────────────────────────────┴──────────────────────────────────────┐
            stream: ecpri_iq                                                          stream: slot_iq
                 │                                                                               │
                 ▼                                                                               ▼
  ┌───────────────────────────────────────┐                       ┌───────────────────────────────────────┐
  │ IqPipeline (iq_pipeline.{h,cpp})       │                       │ SlotIqPipeline (slot_iq_pipeline.*)    │
  │  codelet: ecpri_iq_samples             │                       │  codelet: uplink_slot_samples          │
  │  hook:    capture_xran_packet          │                       │  hook:    capture_uplink_slot          │
  │  - LCM IPC codelet load/unload         │                       │  - LCM IPC codelet load/unload         │
  │  - SPSC queue + worker thread          │                       │  - SPSC queue + worker thread          │
  │  - BFP-9 decompression (per-section)   │                       │  - per-slot cbf16_t (no BFP)           │
  │  - pushes prb_config → codelet         │                       │                                        │
  └───────────────────┬────────────────────┘                       └───────────────────┬────────────────────┘
            const DecompressedSample&                                       const SlotSample&
                      ▼                                                                ▼
  ┌───────────────────────────────────────┐                       ┌───────────────────────────────────────┐
  │ E3SMSpectrum  (RF=1, sm_spectrum/)     │                       │ E3SMLayer1   (RF=2, l1_kpm/)           │
  │  - FFT zero-pad the UL section         │                       │  - ShmIqWriter → /e3_ran_buffers       │
  │  - emit APER Spectrum-IQDataIndication │                       │  - emit L1KPM-Indication, encoded once │
  │    (ASN.1 only)                        │                       │    in config().encoding (JSON or APER) │
  │  - PRB-blacklist control + RanFuncData │                       │  - SHM-pointer payload; no controls    │
  │  - fan out to RF=1 subscribers         │                       │  - fan out to RF=2 subscribers         │
  └───────────────────────────────────────┘                       └───────────────────────────────────────┘
     → public Spectrum Sharing dApps                                  → NVIDIA-Aerial L1-KPM dApps
```

The pipeline ↔ SM split: each codelet's load/queue/(de)compression is
wire-format-agnostic and lives in a pipeline — `IqPipeline` (per-section,
BFP-9-decompressed `ecpri_iq_samples`) and `SlotIqPipeline` (per-slot `cbf16_t`
`uplink_slot_samples`). Service models register a callback and receive each
sample by `const&` (no copies). `E3SMSpectrum` (RF=1) consumes `IqPipeline` and
emits in-band **APER-only** `Spectrum-IQDataIndication` to the public Spectrum
Sharing dApp; `E3SMLayer1` (RF=2) consumes `SlotIqPipeline` and emits the
SHM-pointer `L1KPM-Indication` (JSON or APER) to NVIDIA-Aerial dApps. Both
pipelines are constructed eagerly but **lazy-started**: libe3 calls the SM's
`start()` (which boots its pipeline — codelet load + worker thread) only on the
first dApp subscription to that RAN function.

### Dispatcher Model

The `JbpfDispatcher` is the central routing component:

1. **Single poll loop** — the main thread calls `dispatcher.poll()` which invokes `jbpf_io_channel_handle_out_bufs` once
2. **Stream routing** — each pipeline registers its `stream_id` + callback (`IqPipeline` on `ecpri_iq`, `SlotIqPipeline` on `slot_iq`); the dispatcher routes each jbpf buffer to the pipeline whose stream_id matches
3. **Centralized buffer release** — the dispatcher always releases all buffers after the callback returns, preventing leaks

### Adding a New Service Model

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

> **Note:** Do NOT release jbpf buffers in your SM or pipeline callback — the dispatcher handles that.

### Key Components

Service-model code is grouped under `src/e3sm/` by component: `sm_spectrum/`
(RF=1), `l1_kpm/` (RF=2), `utils/` (writers + decompression), with the shared
data-plane pipelines at the `src/e3sm/` root and the ASN.1 grammars in `asn/`.

| File | Description |
|---|---|
| `src/e3_controller.cpp` | Main daemon — jbpf IO init, E3 agent, dispatcher poll loop, both pipelines + SM wiring |
| `include/jbpf_dispatcher.h` | Central buffer dispatcher — routes by stream_id, releases buffers |
| `src/e3sm/iq_pipeline.{h,cpp}` | RF=1 data plane — `ecpri_iq_samples` codelet load, SPSC queue, worker, BFP-9 decompression → `DecompressedSample` |
| `src/e3sm/slot_iq_pipeline.{h,cpp}` | RF=2 data plane — `uplink_slot_samples` codelet load, SPSC queue, worker, per-slot `cbf16_t` → `SlotSample` |
| `src/e3sm/sm_spectrum/e3sm_spectrum.{h,cpp}` | Spectrum SM (RF=1) — ecpri_iq IQ telemetry (APER `Spectrum-IQDataIndication`, ASN.1 only) via IqPipeline + PRB blacklist control + descriptive RanFunctionData |
| `src/e3sm/sm_spectrum/e3sm_spect_wrapper.{h,cpp}` | C++ wrappers around the Spectrum ASN.1 types (incl. PRB blacklist control decode) |
| `src/e3sm/l1_kpm/e3sm_layer_1.{h,cpp}` | L1-KPM SM (RF=2) — SHM-pointer IQ indications in the configured encoding (JSON or APER) via SlotIqPipeline; writes the optional per-slot stats CSV |
| `src/e3sm/l1_kpm/e3sm_layer1_json.{h,cpp}` | JSON encoder for the L1-KPM (RF=2) indication — `e3sm_layer1::encode_iq_indication_json`, NVIDIA-Aerial-conformant `protocolData` |
| `src/e3sm/l1_kpm/e3sm_layer1_wrapper.{h,cpp}` | APER encoder for `L1KPM-Indication` + layer-1 RanFunctionData (JSON & APER twins) |
| `src/e3sm/utils/e3sm_shm_writer.{h,cpp}` | POSIX shm writer for `/e3_ran_buffers` — owned by `E3SMLayer1` |
| `src/e3sm/utils/bfp_decompress.{h,cpp}` | BFP-9 → int16 IQ decompression — called from `IqPipeline::dispatch_sample` |
| `src/e3sm/asn/e3sm_spectrum.asn` | ASN.1 definitions for Spectrum SM (`Spectrum-IQDataIndication`, `Spectrum-PRBBlacklistControl`, `Spectrum-RanFunctionData`, `Spectrum-ConfigControl`) |
| `src/e3sm/asn/e3sm_layer1.asn` | ASN.1 definitions for L1-KPM SM (`L1KPM-ShmRef`, `L1KPM-Indication`) — module `L1-KPM-SM`; derived from NVIDIA's E3 schema |

## Attribution

The **RF=2 L1-KPM** service model targets NVIDIA Aerial dApps: its
`indicationMessage.protocolData` payload conforms to NVIDIA Aerial's public E3
message schema, in both JSON and ASN.1 (APER).

- JSON (`src/e3sm/l1_kpm/e3sm_layer1_json.cpp`) reproduces NVIDIA's `protocolData`
  keys directly.
- ASN.1 (`src/e3sm/asn/e3sm_layer1.asn`, `L1KPM-Indication`) is wineslab's ASN.1
  representation **derived from** the same JSON schema (NVIDIA publishes the JSON
  schema only).

Source schema (Apache-2.0, `Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES`):
- [e3_message_schemas.json](https://github.com/NVIDIA/aerial-sample-apps/blob/main/dapps/docs/e3_message_schemas.json)
- [e3_message_examples.json](https://github.com/NVIDIA/aerial-sample-apps/blob/main/dapps/docs/e3_message_examples.json)


**Deviations (out of scope for this release):** `cell_id` / `n_rx_ant` are
`OPTIONAL` in `e3sm_layer1.asn` but omitted from the JSON path today (the codelet
doesn't surface them); 

<!-- `setupResponse.ranFunctionList[].ranFunctionData` is a
descriptive `{name, version, description}` object rather than NVIDIA's array of
stream descriptors. See [`docs/nvidia/README.md`](docs/nvidia/README.md). -->

The **RF=1 Spectrum** service model (PRB-blacklist / spectrum sharing) is
wineslab's own and is not part of NVIDIA's schema.

## Known Limitations

### Single encoding per process

The controller serves exactly **one** wire encoding at a time, fixed at startup
by `e3.encoding` (libe3 is built with both encoders, so this is a runtime choice
— see [libe3 (git submodule)](#libe3-git-submodule)). To serve both an ASN.1 dApp and a
JSON dApp simultaneously, run two controller instances on different port triples.
This is the deliberate simplification from the earlier dual-channel design: with
a single encoding, `ServiceModel::ran_function_data()` and the indication
fan-out simply read `E3Agent::config().encoding` — no per-dApp encoding lookup,
no race during simultaneous setup.

Note that **RF=1 Spectrum is ASN.1/APER-only** — there is no JSON encoder for its
in-band IQ indication, so it is only useful under `e3.encoding: asn1` (under
`json` the SM warns once and drops indications). **RF=2 L1-KPM**
supports both encodings.

### PRB blacklist control is a stub

`E3SMSpectrum::handle_control_action()` (RF=1) decodes the
`Spectrum-PRBBlacklistControl` payload, logs the requested PRB list, and
sends a POSITIVE ACK back to the dApp — but the actual application of
the blacklist to the RAN scheduler is not implemented. The
`// TODO: Apply the PRB blacklist to the RAN` line in
[`src/e3sm/sm_spectrum/e3sm_spectrum.cpp`](src/e3sm/sm_spectrum/e3sm_spectrum.cpp)
marks the spot. dApps that rely on the control side-effect (rather than just the
ACK) will not see scheduler behaviour change.

### No verification at codelet load time

Codelets are verified **offline**, at build time (see [Codelets](#codelets)). The gNB does
not re-verify on load: jbpf JIT-compiles with ubpf, which emits no memory bounds checks. So
an object that bypasses `make` — hand-copied onto a pod, say — is loaded unchecked.

Mitigations in place: `make` refuses to replace a codelet object that does not verify, and
the gNB-side publish helper range-checks its source pointer at runtime against a window the
hook publishes, independently of whether the codelet was verified. A load-time
`jbpf_verify()` call before the LCM request is the remaining gap.
