# `dapp_report_relay` — dApp reports to an xApp over E2SM-DAPP

On OAI, a dApp's E3 report (for example the PRBs where it detected an
incumbent) goes to the gNB's E3 agent, which hands it to the E2 agent; the E2
agent sends it to the subscribed xApps as an E2SM-DAPP indication (RAN function
255). On OCUDU the E3 agent is the E3Controller, a separate process, and jbpf is
its only way into the gNB. This codelet is that way in.

This page describes what the E3Controller does with dApp reports, what the
codelet does in the gNB, and how to read the counters on both sides. The
E2SM-DAPP service model itself is in ocudu (`lib/e2/e2sm/e2sm_dapp/`), and the
gNB side of the relay in ocudu's `ocudu_janus/e3_dapp/`.

---

## 1. The path of one report

```
dApp ─E3 DAppReport─▶ E3Controller ─control input─▶ codelet ─helper─▶ E2SM-DAPP bridge ─E2 indication─▶ RIC ─▶ xApp
                      DAppReportRelay   report_in    this one   e3_dapp_report_push   (in the gNB)
```

1. **E3Controller, libe3's report worker thread.** libe3 calls
   `DAppReportRelay::on_report()` (`src/e3sm/dapp_report_relay.cpp`). The relay
   checks the size (1 to 2048 bytes), copies the report into a
   `struct e3_dapp_report_msg` with its receive time, and queues it. On the first
   report it loads this codelet into the gNB over the gNB's LCM socket.
2. **E3Controller, main loop.** `poll()` sends the queued messages to the
   codelet's control input `report_in`. jbpf allows that only from the thread
   that owns the io context, hence the queue between the two threads.
3. **gNB, the bridge's poll thread.** While at least one E2SM-DAPP data
   subscription (report style 1) exists, the bridge fires the hook
   `e3_dapp_report_poll` every 0.5 ms. Each time, the codelet takes up to 8
   messages from `report_in` and hands each one to the bridge with the helper
   `e3_dapp_report_push`.
4. **gNB, E2 agent.** The bridge keeps each report for every subscription and
   notifies the E2 agent, which sends one indication per report right away:
   header format 1 (dApp id, RAN function id, sequence id, this node's identity,
   a timestamp) and message format 1 (the report bytes).

**Nobody decodes the report.** `data[]` is the E3 `reportData`, and it becomes
the indication's data unchanged. The xApp decodes it with the service model that
`ran_function_id` names. For Spectrum (1) that is APER `Spectrum-DAppReportData`,
also when the E3 link is JSON: libe3 carries `reportData` as hex in JSON and gives
back the raw bytes.

A report reaches the E2 agent at most about 0.5 ms after the E3Controller sends
it, plus the wait in the E3Controller's main loop.

## 2. Only while an xApp listens

Without a subscription the bridge does not fire the hook, so nothing drains the
codelet's queue. The E3Controller must not fill it, and the dApp is not told:
it keeps reporting, as it does with OAI. The codelet is the only one that can
say when someone listens.

- **The codelet reports the subscriptions.** Each time the hook fires, the
  codelet sends a `struct e3_dapp_presence_msg` on its output channel
  `presence_out`, with the number of data subscriptions from the hook context.
  It sends one when the number changes and every 100 ms otherwise. When the last
  subscription ends, the bridge fires the hook once more with 0.
- **The E3Controller forwards only while there is one.** `XAppPresence`
  (`src/e3sm/xapp_presence.h`) says an xApp is there while the last message
  counted at least one subscription and is less than 300 ms old (three
  periods). Otherwise the relay drops the report, counted as `no_xapp`.
- **Both changes are logged:**
  ```
  [DAppReportRelay] xApp subscribed (1 data subscription(s) in the gNB): forwarding dApp reports
  [DAppReportRelay] no xApp subscribed any more: dropping dApp reports
  ```
- **Nothing is replayed.** Reports dropped before a subscription are not sent
  later. In the gNB, a report that is more than 1 s old when it reaches the
  bridge is not sent either (`stale` in the gNB's line, see §5).

The codelet is loaded at the first report even when no xApp is subscribed,
because it is what announces the first subscription.

## 3. The contract

The shared header is `codelets/include/jbpf_e3_dapp_api.h`. ocudu keeps a
byte-identical copy in `ocudu_janus/e3/jbpf_e3_dapp_api.h`, and its configure
step stops if the two differ.

| What | Value |
|---|---|
| Hook | `e3_dapp_report_poll`, fired by the bridge's poll thread (not by the RAN data plane); declared in ocudu's `ocudu_janus/e3_dapp/jbpf_e3_dapp_hooks.h` |
| Program type, ELF section | `JBPF_E3_PROG_TYPE_E2_DAPP` (`CUSTOM_PROGRAM_START_ID + 5`), section `jbpf_e2_dapp` |
| Hook context | `struct jbpf_e2_dapp_ctx`: `data`, `data_end`, `meta_data` and `ctx_id` are 0; `nof_subscriptions` is the number of data subscriptions now |
| Helper | `e3_dapp_report_push`, `CUSTOM_HELPER_START_ID + 2`, registered by the gNB; returns a negative value for a malformed message or when the gNB has no bridge |
| Input channel | `report_in`, stream `7531abcd1234567890fedcba0987656a`, `struct e3_dapp_report_msg`, up to 64 waiting |
| Output channel | `presence_out`, stream `7531abcd1234567890fedcba0987656b`, `struct e3_dapp_presence_msg` |
| Other maps | `scratch` holds one message (2 KB does not fit in eBPF's 512-byte stack); `presence` holds what the last presence message said and when |

`struct e3_dapp_report_msg`:

| Field | Meaning |
|---|---|
| `version` | `E3_DAPP_REPORT_VERSION` (1) |
| `data_len` | bytes of `data[]` in use, 1 to 2048 |
| `dapp_id` | E3 dAppIdentifier of the sender |
| `ran_function_id` | E3 RAN function of the report (1 = Spectrum) |
| `sequence_id` | E3 sequenceId, set by the dApp; the xApp gets it in the indication header |
| `received_ts_ns` | CLOCK_MONOTONIC when the E3Controller received the report (the two processes share the host); the bridge uses it for the 1 s cut |
| `data[2048]` | E3 `reportData`, unchanged. A Spectrum report for all 273 PRBs is about 310 bytes |

The E3Controller builds the load request in code (`load_codelet()` in
`src/e3sm/dapp_report_relay.cpp`). `dapp_report_relay.yaml` is there for
reference and for `jbpf_lcm_cli`: the channel names and stream ids must be the
same in the codelet, the YAML and `src/e3sm/dapp_report_relay.h`.

## 4. Configuration

E3Controller (`configs/e3_controller.yaml`):

```yaml
dapp_reports:
  enabled: true     # false: reports are not relayed and the codelet is never loaded
  queue_depth: 256  # reports waiting for the main loop; more are dropped as queue_full
```

gNB: ocudu built with `-DENABLE_JBPF=ON` from a branch with the bridge
(`ocudu_janus/e3_dapp/`), with the usual `jbpf:` section and, in `e2:`,
`enable_du_e2: true` and `e2sm_dapp_enabled: true`. Without the bridge, the
codelet does not load and the E3Controller counts `no_codelet`.

dApp (adaptive_cpu): `prb_report.enabled: true`, and `e3_manager.spectrum_agent`
naming the E3 agent that serves RAN function 1 (`OCUDU_L2` on OCUDU; the default
`OAI_L2` is OAI's gNB).

## 5. Reading the counters

**E3Controller**, once a second while reports arrive, and in the shutdown
summary:

```
[DAppReportRelay] reports received 305863, forwarded 273034, dropped: no_xapp 32829, too_big 0, no_codelet 0, queue_full 0, send_failed 0
```

Each report ends up in exactly one of these:

| Counter | Meaning |
|---|---|
| `forwarded` | sent to the codelet |
| `no_xapp` | no xApp subscribed: normal before the xApp subscribes and after it leaves |
| `too_big` | the report was empty or larger than 2048 bytes |
| `no_codelet` | the codelet could not be loaded (gNB not running, or built without the bridge); retried at most every 2 s |
| `queue_full` | the E3Controller's own queue was full: the main loop is not keeping up |
| `send_failed` | the codelet's queue in the gNB was full: the bridge stopped draining it before the presence messages said so. Should stay 0 |

**gNB**, the bridge's line on stdout, every 10 s while reports arrive (with
`first dApp report` and `subscription ... started` lines when those happen):

```
[e3_dapp] reports stored 39972, sent 39972, not sent: stale 0, overrun 0
```

| Counter | Meaning |
|---|---|
| `stored` | reports the codelet handed to the bridge |
| `sent` | reports given to the E2 agent as indications |
| `stale` | the report was more than 1 s old when it reached the bridge |
| `overrun` | a subscription fell more than 64 reports behind, and lost the oldest ones |

## 6. Limits

- **One direction only.** xApp controls are not relayed to the dApp: the gNB
  answers E2SM-DAPP controls with a failure.
- **Report style 1 only.** Report style 2 (the map of dApps and their E3 RAN
  functions) is accepted but sends no indication.
- **A gNB restart needs an E3Controller restart.** The E3Controller does not
  load the codelet again, so the presence messages stop and every report is
  dropped as `no_xapp` until the E3Controller is restarted too.
- **Rate.** The codelet moves at most 8 reports per call, every 0.5 ms (16 000
  reports/s), and up to 64 can wait in the gNB between calls.

See also [Known limitations](../../docs/known-limitations.md#dapp-reports-reach-the-xapp-nothing-comes-back)
and [Service models](../../docs/service-models.md#dapp-reports-to-an-xapp-e2sm-dapp).

## 7. Build and verify

The codelet builds and verifies with the others (see [the codelets
README](../README.md)):

```sh
make -C codelets OCUDU_DIR=/path/to/ocudu-e3
```

The output must say `verified:` for `dapp_report_relay.o`. The verifier knows
the program type and the helper from `jbpf_e3_dapp_api.h`
(`codelets/verifier/e3_verifier_cli.cpp`).
