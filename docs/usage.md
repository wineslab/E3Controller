# Usage

> **Important:** E3Controller (IPC primary) must start **before** ocudu (IPC secondary).

```bash
./out/bin/e3_controller --config configs/e3_controller.yaml
```

`--config` is the **only** argument. It replaced 20 command-line options, deliberately:
two configuration mechanisms invite the two to disagree, and the radio geometry has to be
stated in exactly one place.

A fully documented example ships at [`configs/e3_controller.yaml`](../configs/e3_controller.yaml).
Unknown keys are a **startup error**, not a warning — a typo'd `nof_port:` that was
silently ignored would leave the controller sizing rows for the default while you believed
you had configured something else.

## Example

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
  writer:      gnb             # the gNB writes the rows

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
  drops_log_path: ""           # empty disables the drop-accounting CSV
  latrec_dir: ""               # where stage-record rings go; needs --latrec

slot_filter:
  ue_free_only: false          # true = only the UL slots no UE transmits in
  slots: []                    # slot indices within the frame; empty = every slot

prb_block:
  enabled: false               # true = a dApp's PRB block goes to the gNB's scheduler

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

## Configuration

| Section | Keys | Notes |
|---|---|---|
| `radio` | `nof_ports`, `nof_prbs`, `nof_symbols`, `scs_khz` | **Must match the running gNB.** Validated against the RAN — see below. |
| `shm` | `name`, `size_bytes`, `cbf16_scale`, `writer` | The `/e3_ran_buffers` region the controller owns and the dApp reads; `writer: gnb`: the gNB writes the rows (see below) |
| `jbpf` | `ipc_name`, `run_path`, `mem_size_bytes`, `lcm_socket_path`, `codelet_base_path` | Must agree with the gNB's own `jbpf:` YAML section |
| `e3` | `encoding`, `link_layer`, `transport`, `setup_port`, `publisher_port`, `subscriber_port` | `encoding` is `asn1` or `json`; JSON/cuBB dApps expect ports 5555/5556/5557 |
| `threads` | `poll_core`, `worker_core`, `publisher_core`, `poll_interval_us` | `-1` = no pinning (the poll thread then sleeps rather than busy-spinning) |
| `logging` | `drops_log_path`, `latrec_dir` | Drop accounting, and where stage-record rings land ([Stage records](stage-records.md)) |
| `slot_filter` | `ue_free_only`, `slots` | Which captured UL slots the gNB publishes, decided in the slot codelet before any IQ moves (see below) |
| `prb_block` | `enabled` | Apply a dApp's PRB block (RAN function 1, control 1) in the gNB's scheduler (see below) |
| *(top level)* | `target_slot` | Forward only this slot index within a 10 ms frame; `-1` forwards every UL slot. Filters in the controller, after the gNB has published the slot; `slot_filter.slots` does the same in the gNB |

### Sensing only the slots no UE transmits in (`slot_filter`)

A sensing dApp often wants spectrum that no UE of the cell is using. With
`slot_filter.ue_free_only: true` the gNB publishes only the uplink slots in
which its PHY expects no PUSCH, PUCCH or SRS reception.
- **Free slots cost nothing extra.** The slot codelet makes the decision in the
  gNB, from counts the PHY passes with the slot, so a busy slot is never
  converted or written.
- **Busy slots are counted.** The codelet still reports each one, and the
  controller counts them as `ue_busy` in its drop accounting, so free and busy
  slots can be told apart in the run's numbers.
- **The gNB must capture empty slots.** By default the gNB asks the RU only for
  slots that have something scheduled, so an empty slot never reaches the
  codelet. Set `expert_phy.allow_request_on_empty_uplink_slot: true` in the gNB
  config (see ocudu's `README_JBPF.md`).
- **Under full uplink load almost no slot is free.**

`slot_filter.slots` picks slot indices within the 10 ms frame, in the gNB.
For example, `[8, 9, 18]` leaves out slot 19, which holds the PRACH occasion
with PRACH configuration index 159.

### Blocking the PRBs a dApp detected (`prb_block`)

A spectrum-sharing dApp sends a Spectrum SM PRB blacklist control (RAN function
1, control 1) with the PRBs where it detected an incumbent. With
`prb_block.enabled: true` the gNB keeps every data allocation off those PRBs,
in every slot, UL and DL, until the dApp's next control replaces the list; an
empty list clears it. This is what OAI's gNB does with the same control, so the
same dApp works with both.
- **What avoids the PRBs.** PDSCH and PUSCH: UE data, SIB1, RAR, Msg3, Msg4 and
  paging. PUCCH, PDCCH, SSB, PRACH, SRS and CSI-RS keep their PRBs, even inside
  a blocked range.
- **How it gets there.** The first dApp subscription to RAN function 1 loads the
  `prb_block` codelet onto the gNB scheduler's `sched_prb_block` hook (the gNB
  must be built with it, `ENABLE_JBPF=ON`). Each control goes to the codelet,
  and the scheduler applies it from its next slot. The dApp gets its ACK when
  the block is in the gNB, as from OAI's.
- **When it ends.** When the last dApp leaves RAN function 1 the codelet is
  unloaded, which clears the block. `validityPeriod` is ignored, as on OAI.
- **RAN function 1 carries no IQ** while this is on; the IQ comes from the L1-KPM
  SM (RAN function 2).
- **Detect on free slots only.** If the dApp also senses the slots UEs transmit
  in, it sees their PUSCH as interference and blocks it. Use
  `slot_filter.ue_free_only: true`.

The controller prints a `[PrbBlockRelay]` line each second with the blocks it
received, sent and saw applied, and the gNB logs each new block (`PRB block id=...`).
Timing the loop end to end is described in [Stage records](stage-records.md).

### Who writes the rows (`shm.writer`)

The gNB does (`writer: gnb`). The shipped slot codelet calls the gNB-side
publish helper, which converts the resource grid cbf16 -> fp16 and writes the row
into `/e3_ran_buffers` in the PHY RX thread, in one pass. The jbpf ring carries
only a ~64 B descriptor, so the controller stays out of the data plane: it only
tells the dApp which row to read. This needs a gNB with the E3 helpers
registered (ocudu-e3 built with `ENABLE_JBPF=ON`).

The controller still **owns** the region: it creates, sizes, zero-fills, headers
and tears it down. The helper only attaches (`O_RDWR` without `O_CREAT`, so it
cannot race the owner into creating one with the wrong shape).

### Radio geometry is checked, not trusted

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
> (the recommended build — see [What the normal build steps do](build.md#what-the-normal-build-steps-do)),
> `e3.encoding` is a pure runtime choice. If libe3 was built with only one
> encoder, it must match, or the outbound encoder rejects every PDU.

### Drop accounting

Off by default; enabled by setting `logging.drops_log_path`. One cumulative row
per second: `uptime_s, published, dropped_total, latrec_clamped`, then a column
per drop reason. Aggregate and throttled, so it does no work on the slot path.

The drop *counters* are always on: the shutdown summary prints them by reason,
with or without this path. Nothing is printed while running. Each fault has its
own message (geometry mismatch, a helper that wrote nothing, an encode or send
failure), and `ue_busy` drops are by design: with `slot_filter.ue_free_only`,
every UL slot a UE transmits in is one.

Per-slot stage timing is not here; see [Stage records](stage-records.md).

An example launcher is available [here](../start_e3controller_example.sh).
