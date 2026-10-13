# Stage records

Where the time goes on the slot path, recorded without perturbing it.

The controller stamps its own stages into **latrec**, the per-thread lock-free
ring recorder libe3 ships in `libe3/latrec.h`. A stamp is one
`clock_gettime(CLOCK_MONOTONIC)` plus four stores into an mmap-backed ring: no
syscall, no allocation, no formatting, no lock and no I/O on the slot path.
Conversion to tables happens offline, out of process, against the ring files.

This replaced a per-slot CSV that opened an `ofstream`, wrote a row and flushed
it once per slot inside the sample handler — so the numbers it produced included
the cost of producing them. See [What recording costs](#what-recording-costs).

Build with `./build.sh --latrec`, then point the rings somewhere with
`logging.latrec_dir`.

## The stages

One row per slot, on the forward leg. Box numbering is libe3's
`docs/path-a-e3-loop.md`; the identifiers are the shared catalog's, which names
*operations* rather than components — there is no controller-specific stage
block, and which component performed an operation is read off the ring that
recorded it.

| Box | Segment | Covers |
|---|---|---|
| A1 | `RECORD_BEGIN` → `PROCESS_BEGIN` | the data recording: jbpf dispatch, then the cbf16 → fp16 convert of the grid and the row write into `/e3_ran_buffers` |
| A2 | `PROCESS_BEGIN` → `ENCODE_E3SM_BEGIN` | getting it to the Service Model: jbpf ring transit, dispatcher poll, queue wait |
| A3 | `ENCODE_E3SM_BEGIN` → `ENCODE_E3SM_DONE` | the E3SM payload encoder |
| — | `ENCODE_E3SM_DONE` → `WAIT_ENTER` | the emit tail, over every subscriber |

**A1 and A2 are recorded here, and the gNB needs no instrumentation for it.**
Both of A1's boundaries are already on the wire by the time the slot arrives:
`gnb_ts_ns` is stamped on the last symbol with the resource grid complete and
nothing yet copied, and `codelet_ts_ns` just before the codelet submits. So the
controller replays them rather than the RAN keeping a ring of its own.

That works because the whole chain reads `CLOCK_MONOTONIC` — the gNB hook,
`jbpf_time_get_ns()` (see ocudu's `jbpf_patches/jbpf_monotonic_time.patch`) and the
dispatcher poll — which is latrec's own clock. No domain conversion, no offset
arithmetic, no rate skew to absorb. **All of those sites have to agree**; if one
drifts back to `CLOCK_REALTIME` the stage intervals silently mix epochs.

Four sub-hops stay recoverable from `aux` payloads, which is what keeps jbpf
dispatch separable from the data movement:

| Sub-hop | Value |
|---|---|
| jbpf dispatch | `RECORD_BEGIN.aux2` − `RECORD_BEGIN` |
| convert + row write | `PROCESS_BEGIN` − `RECORD_BEGIN.aux2` |
| jbpf ring transit | `PROCESS_BEGIN.aux` − `PROCESS_BEGIN` |
| queue wait | `ENCODE_E3SM_BEGIN` − `PROCESS_BEGIN.aux` |

Everything past the emit boundary — E3AP encode, the outbound queue, the
connector send — is libe3's box and is stamped inside libe3. **E3SM and E3AP are
separate boxes**: the Service Model codec is ours, the E3AP codec is the
library's, and keeping them apart is what makes the library's cost separable
from ours. We do not re-time the library's stages; we join to them.

Slots that never reach the traced region are not stamped. In particular the "no
subscribers" path returns before the first stamp: that is the normal idle state,
and recording it would fill the ring with slots nobody asked for. A slot whose
encode fails closes its row with `SKIPPED` / `LATREC_SKIP_ENCODE`.

## Back-dating and the clamp

A1's boundaries happened before the handler ran, so those two stamps carry times
earlier than the moment they are issued. A ring is a single-writer log whose
`t_ns` must ascend, and libe3's reader treats the *one* permitted descent as the
wrap point and silently rotates there — a descent would not raise an error, it
would produce a plausible capture cut at the wrong offset.

Ordering normally holds with a wide margin: the jbpf hook is a synchronous
inline call, so slot N's codelet has returned before slot N+1 is stamped, and A1
is tens of microseconds against a slot spacing of at least 500 µs at 30 kHz SCS.
Two things can still break it — a pipeline stall longer than the slot spacing,
and several RU receive threads (one per sector) whose slots interleave on one
ring. So the floor is enforced rather than assumed, and the count of enforced
stamps is reported in `latrec_clamped` in the drop CSV and in the shutdown
summary. **A non-zero count means A1 is understated for that many slots.**

## Reading a capture

```bash
python3 /usr/local/share/libe3/tools/latrec2csv.py <latrec_dir>
```

Records land in `ocudu.csv` — the ring role is `e3controller.l1_kpm`, and
`latrec2csv.py` maps the `e3controller` prefix onto the `ocudu` component. The
hops appear as `RECORD_BEGIN__PROCESS_BEGIN_us` and so on, with the emit tail as
`ENCODE_E3SM_DONE__WAIT_ENTER_us`.

**Joining to libe3's records.** The controller publishes each slot's record
sequence with `latrec_ctx_set()` immediately before entering the library; libe3
stamps it into `EMIT_ENTER`'s `aux`, which surfaces as the `origin_seq` column
on the outbound leg. So:

- `source.seq == outbound.origin_seq` links a slot to its emission. Both legs
  are in `ocudu.csv`, because the emit boundary and the enqueue are stamped on
  the calling thread — ours.
- That outbound row's `seq` then links into `libe3.csv`, where the library's own
  outbound thread stamped `DEQUEUE` → `ENCODE_E3AP_DONE` → `SEND_DONE`.

Two steps, because the outbound leg is split across the two threads that perform
it. There is no shared message identifier doing this work: E3AP's `message_id`
wraps at 1000, and while libe3 ≥ 0.1.2 surfaces it to a *dApp* calling
`send_control`/`send_report`, it is still not visible on the Service Model emit
path this SM uses.

## Ring sizing and the capture window

Rings default to 2^18 records (8 MiB per thread). The slot path writes five
records per published slot, so at 30 kHz SCS with every UL slot forwarded
(~2000 slots/s) a default ring holds about **26 seconds** before it wraps:

```bash
LATREC_ENTRIES_LOG2_E3CONTROLLER_L1_KPM=24    # 512 MiB, ~28 min; ceiling 2^28
```

The variable is the ring role uppercased with non-alphanumerics replaced by `_`.
It sizes the ring — it does not enable anything.

**A wrapped capture is not a valid measurement**: records are lost off the front
and any span computed across the wrap is wrong. `rings.csv` reports `wrapped`
and `lost_records` for exactly this reason, and CI fails on a non-zero value.

## What recording costs

`out/bin/bench_stage_recording` prices the recorder. It links the same trace
header the slot handler uses and nothing else — no jbpf, no Service Model, no
shared-memory writer — so it runs in CI with no RAN attached.

There is deliberately no "real slot work" arm: the gNB converts and writes the row itself, so the controller
moves no slot data at all, and against real work the stamps were never
resolvable anyway.

Median of 15 batches, net of the loop floor, `-DUSE_NATIVE=OFF`:

| Recording one slot | Workstation | CI (EPYC 7763) | Of a 500 µs slot |
|---|---|---|---|
| latrec, 5 records | ~111 ns | ~102 ns | 0.02% |

Per record that is ~20–22 ns against a measured 22–28 ns clock read: the clock
and essentially nothing else.

<!-- **Compare within a host, never across one.** The two CI legs are separate jobs
and land on whatever runner they get — on one run an EPYC 7763 and an Intel Xeon
6973P-C, where the `csv` arm alone differed by 2.7×. That arm is the calibration
anchor: it is the same code in both builds, so when it disagrees, nothing else
in those two reports is comparable either.

The untraced leg is further apart still, and for a second reason: with the
recorder off `latrec_tnow()` compiles to `return 0`, so the bench's per-slot
clock read disappears and the loop floor collapses (~1 ns instead of ~30 ns).
Read that leg for the `csv` arm and the not-linked assertion, not for a
comparison against the traced one. -->
