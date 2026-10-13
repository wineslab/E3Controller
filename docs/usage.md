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
| *(top level)* | `target_slot` | Forward only this slot index within a 10 ms frame; `-1` forwards every UL slot |

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

The drop *counters* are always on — the throttled stderr line and the shutdown
summary do not depend on this path.

Per-slot stage timing is not here; see [Stage records](stage-records.md).

An example launcher is available [here](../start_e3controller_example.sh).
