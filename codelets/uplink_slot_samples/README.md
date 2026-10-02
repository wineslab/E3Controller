# `uplink_slot_samples` — what this codelet gathers, and what the numbers mean

This codelet is the RAN-side entry point of the E3 uplink IQ path. It runs in the
gNB's PHY RX thread, under ubpf, on every completed uplink slot.

It is **descriptor-only**. No IQ ever crosses the jbpf ring: the codelet calls the
gNB-side helper, which converts and writes the samples straight into
`/e3_ran_buffers`, and the codelet then publishes a ~64-byte descriptor saying
*where the row landed*. That is the whole point of the design — it halves total
memory traffic versus copying the grid through jbpf, and it is why
`iq_bytes` reads `0` in the controller's stage CSV.

---

## 1. What the codelet reads (input)

`struct jbpf_ran_slot_ctx` — defined by the hook PRODUCER, i.e. ocudu, in
`include/ocudu/janus/jbpf_srsran_contexts.h`. That file is the single source of
truth; `codelets/Makefile` diffs it at build time (`check-contract`) and fails on
drift, because a field added on the RAN side would silently shift every offset the
codelet reads through.

| field | meaning |
|---|---|
| `data` / `data_end` | packet-style bounds over the cbf16 resource grid |
| `gnb_ts_ns` | `CLOCK_MONOTONIC` ns, stamped by the gNB just before the hook fires |
| `sfn`, `subframe_id`, `slot_id` | slot identity (`slot_id` = index within the 10 ms frame) |
| `ctx_id` | sector |
| `nof_ports`, `nof_symbols`, `nof_subcarriers` | grid shape for THIS slot |

The grid is `nof_ports x nof_symbols x nof_subcarriers` × `cbf16_t` (4 B), laid
out `[port][symbol][subcarrier]`. For the 4×4 / 273 PRB / 30 kHz configuration
that is **733,824 bytes per slot**.

## 2. What the codelet publishes (output)

`struct e3_slot_desc` — the shared contract in `codelets/include/jbpf_e3_slot_api.h`,
vendored byte-identically into the gNB at `ocudu_janus/e3/`. Both copies are
compared at configure time; a divergence would make the two writers disagree with
**no runtime error**, because bf16 and fp16 are both 2 bytes.

| field | meaning |
|---|---|
| `gnb_ts_ns` | copied through from the ctx (the RAN-side anchor) |
| `codelet_ts_ns` | `jbpf_time_get_ns()` at the **END** of the codelet — see §3 |
| `bytes_written` | bytes the helper actually wrote; `0` means it refused |
| `fh_buffer_index`, `fh_write_index` | the row the helper wrote — what the Indication carries |
| `flags` | `E3_SLOT_FLAG_TRUNCATED` when the helper refused rather than writing a partial row |
| `sfn`, `subframe_id`, `slot_id`, `sector_id` | slot identity |
| `nof_ports`, `nof_symbols`, `nof_subcarriers` | grid shape, so the controller can validate against its config |

## 3. The timestamps — and the one that is easy to misread

`codelet_ts_ns` is stamped at `uplink_slot_collect.c:220`, **after** the
`jbpf_e3_publish_slot()` call at `:174-180`. So the controller's
`gnb_to_codelet_us` column is **not** "hook → codelet entry", despite what its
name suggests. It spans:

```
gnb_ts_ns          clock_gettime(CLOCK_MONOTONIC) immediately before the hook fires,
                   on the LAST symbol of the slot, i.e. when the resource grid is
                   complete  (lib/phy/upper/upper_phy_rx_symbol_handler_impl.cpp:73)
  |
  |  ubpf JIT dispatch + the codelet's constant-size bounds-check ladder
  |  jbpf_e3_publish_slot():  bf16 -> fp16 AVX2/F16C convert
  |                        +  the full 733,824-byte row write into /e3_ran_buffers
  |  descriptor field writes
  v
codelet_ts_ns      jbpf_time_get_ns()  (uplink_slot_collect.c:220)
```

**Read it as "gNB hands off the completed slot → the IQ is in shared memory."**
It excludes PHY symbol processing (the anchor is grid-complete) and it is *not*
jbpf overhead.

Sanity check on that reading: the convert alone accounts for the large majority of
this stage, with only a few microseconds of jbpf dispatch left over. So treat this
column as a memory-bandwidth measurement, not as instrumentation overhead.

### One clock, everywhere

Every stamp on this path is **`CLOCK_MONOTONIC`**, and that is a five-place
invariant rather than a local choice:

| site | file |
|---|---|
| `gnb_ts_ns` | `lib/phy/upper/upper_phy_rx_symbol_handler_impl.cpp` |
| `codelet_ts_ns`, via `jbpf_time_get_ns()` | `jbpf/src/core/jbpf_helper_impl.c` — **local patch**, see `jbpf_monotonic_time.patch` |
| `dispatch_ts_ns` | `E3Controller/src/e3sm/slot_iq_pipeline.cpp` |
| handler entry | `E3Controller/src/e3sm/l1_kpm/e3sm_layer_1.cpp` |
| the dApp's arrival age | `adaptive_cpu/subcarrier_power_app.cpp` (`now_mono_ns`) |

Change one without the others and the subtraction mixes two unrelated epochs.
**That failure is silent** — no error, just plausible-looking wrong latencies, or,
in unsigned arithmetic, a wrap to ~1.8e19 that clamps to 0. It has already
happened once, when the dApp measured a `CLOCK_REALTIME` wire timestamp against
`CLOCK_MONOTONIC` and reported an arrival age of ~0.

Two reasons for monotonic over realtime:

- it is the clock **latrec** stamps, so the stage CSVs and the latrec rings share
  one epoch and join directly, with no offset arithmetic;
- `CLOCK_REALTIME` can be stepped by NTP, and a step mid-run corrupts every stage
  derived from these absolute stamps.

The cost: `CLOCK_MONOTONIC` shares an origin only within a single boot. These
stamps are comparable across **processes on one host**, not across hosts. A
distributed deployment would have to move back to an NTP-disciplined clock — and
move all five sites together.

## 4. Downstream: the controller's stage CSV

Enabled by `logging.stats_log_path` in `e3_controller.yaml`. One row per
**published** slot:

```
slot_seq,gnb_to_codelet_us,codelet_to_dispatch_us,dispatch_to_handler_us,
shm_ns,encode_ns,emit_ns,nof_subc,iq_bytes
```

| column | meaning |
|---|---|
| `gnb_to_codelet_us` | §3 — the data plane: hook → IQ in SHM |
| `codelet_to_dispatch_us` | codelet → controller's jbpf dispatcher poll |
| `dispatch_to_handler_us` | dispatcher → `on_sample` (SPSC queue wait) |
| `shm_ns` | controller-side convert cost. **0 in `writer: gnb` mode** — the helper already wrote the row |
| `encode_ns` | SM payload encode (APER or JSON) |
| `emit_ns` | `emit_outbound` fan-out, i.e. SM-side enqueue only |
| `iq_bytes` | **0** in descriptor-only mode — confirms no IQ crossed jbpf |

A sibling `<name>_drops.csv` carries cumulative drop counts per reason
(`no_subscribers`, `ran_published_nothing`, `blob_too_small`, …), one row per
second, written only when a total changes.

## 5. Measured results

Concrete measurements are deliberately **not** kept in this file. They are
run-, pod- and configuration-specific, and this README is publishable
documentation of *what* is measured rather than a results record.

Current numbers live in the internal reports:

- `report/e2e_latency_audit_7ds2u.md` — 7DS2U, 4x4, 273 PRB, 30 kHz
- `report/e2e_latency_audit_2ds7u.md` — the higher-UL-rate baseline

Those cover the per-stage percentiles, the `gnb_to_codelet_us` bimodality and its
occupancy explanation, the dApp end-to-end figure, and the loss map below filled
in with actual counts.

## 6. Known blind spots in the accounting

A low drop count in `_drops.csv` does **not** mean nothing was lost end to end.
Each segment below is counted by a different thing, and one segment is counted by
nothing at all.

| segment | who counts it |
|---|---|
| RAN → controller (hook fired vs codelet published vs ring delivered) | **nobody** — no counter exists on either side |
| pipeline SPSC queue full | `SlotIqPipeline::dropped_samples()`, printed **only** in the controller's shutdown summary, never in a CSV |
| controller SM (9 named reasons) | `<stats>_drops.csv` + the shutdown summary |
| libe3 outbound queue | libe3's own log / its `L9_DROP` latrec stage |
| wire → dApp | the dApp only (`life: rx/drop`) |
| dApp shed (`no_range`, `shed_age`, …) | the dApp's `[Status]` block |

Practical consequences: capture the controller's **stdout** (the queue-full count
lives only there), and compare the RAN's expected UL-slot rate against the
published rate by hand — a mismatch there is invisible to every counter listed
above. Expected rate is
`nrofUplinkSlots / dl-UL-TransmissionPeriodicity` (both visible in the gNB's
resolved `pattern1` log line), times sectors.

Two consequences:

1. The controller's drop counters only see slots that reach `on_sample`. Anything
   lost in the hook, the codelet, or the jbpf ring is invisible to them.
2. **Sequence-gap detection is deliberately NOT implemented.** Under TDD only UL
   slots fire the hook, and with `allow_request_on_empty_uplink_slot` an idle UE
   can leave UL slots unprocessed too — so a missing slot index is not
   necessarily a loss, and inferring drops from gaps would be confidently wrong.
   Doing it properly needs a learned UL-slot mask.

## 7. Building and verifying

```bash
make -B OCUDU_DIR=/path/to/ocudu-e3        # -B is REQUIRED, see below
```

Every build compiles **and then verifies** with the offline PREVAIL verifier
(`codelets/verifier/`), and a verification failure fails the build, leaving the
previous good `.o` in place. Current size: **346 instructions**, verified.

`make clean` deletes only `*.o.tmp` **by design** — the `.o` files are tracked and
deployed, so a failed compile must not destroy the last good one. The consequence
is that a bare `make` against an unchanged `.c` is a silent **no-op**; use `-B` to
force a real rebuild.

Verification matters here because the gNB does **no** load-time verification: it
loads via ubpf, whose JIT emits no memory bounds checks. This build step is the
only safety gate that ever runs on this object. That is also why the publish
helper re-validates the source window at runtime (`e3_set_src_window`) instead of
trusting that the loaded object was verified.

## 8. Enabling end-to-end latency tracing

The stage CSV stops at `emit`, so the `emit → wire → dApp receive` segment is
invisible to it — and in practice that segment dominates. To see inside it, use
latrec —
libe3 and the dApp share the format, so their rings correlate:

```bash
. run_scripts/latrec_capture.sh /tmp/latrec/run1   # in EVERY pane, same dir
# start controller, then dApp
python3 /workspace/e3_improved/libe3/tools/latrec2csv.py /tmp/latrec/run1 -o /tmp/latrec/run1/out
```

Both are no-ops unless `LATREC_DIR` is set, so production pays nothing. Mind the
ring defaults: several roles default to 2^22 records = 128 MiB each.
