# Codelets

The jbpf codelets E3Controller loads into the RAN. This directory is the
**canonical home** for them — they are built and verified here, with no
dependency on `jrtc-apps`.

| Directory | Hook | Program type | Service model |
|---|---|---|---|
| `ecpri_iq_samples/` | `capture_xran_packet` | `jbpf_ran_ofh` | Spectrum (RAN function 1) |
| `uplink_slot_samples/` | `capture_uplink_slot` | `jbpf_ran_slot` | L1 (RAN function 2) |

## Build

Prerequisites: `clang` with the BPF target, plus a built `e3_verifier_cli`
(produced by the top-level `./build.sh`, from `codelets/verifier/`).

```sh
make                    # build + verify every codelet
make verify             # re-verify existing objects
make clean
```

To build against an ocudu checkout that is not a sibling of this repo:

```sh
make OCUDU_DIR=/path/to/ocudu-e3
```

## Verification is mandatory

`make` fails if a codelet does not verify. That is deliberate, and it is not
belt-and-braces: **the gNB never verifies codelets.** jbpf's load path is
`ubpf_load_elf_ex` followed by `ubpf_compile`
(`external/jbpf/src/core/jbpf.c:968,976`), PREVAIL is not on it, and ubpf's JIT
emits no memory bounds checks at all — only its interpreter does, and jbpf uses
the JIT. So this build step is the only memory-safety gate these codelets ever
pass through, and an advisory one would be decorative.

The verifier is `codelets/verifier/e3_verifier_cli.cpp`. It replaces the SDK
image's `srsran_verifier_cli`, which models only `jbpf_ran_ofh_ctx` (27 bytes)
and therefore cannot verify the per-slot codelet at all — that is the origin of
the `Upper bound must be at most 27` message.

## The context contract

The hook context structs are defined by the **producer**, so ocudu owns them:
`ocudu-e3/include/ocudu/janus/jbpf_srsran_contexts.h`. `Makefile.defs`
puts that directory on the include path; nothing here keeps a maintained copy.

```sh
make import-contract    # take a local copy (for builds with no ocudu checkout)
make check-contract     # fail if the local copy has drifted from ocudu
```

`check-contract` runs as part of `make`. If you change a ctx struct on the RAN
side, **rebuild every codelet**: a changed layout shifts every offset they read
through, and `e3_verifier_cli` carries `static_assert`s on the ctx extents that
will fail the build first.

## Deployment

The codeletset YAMLs resolve the object path through `$JBPF_CODELETS`:

```yaml
codelet_path: ${JBPF_CODELETS}/uplink_slot_samples/uplink_slot_collect.o
```

That variable must point at **this** directory in the deployment environment,
since it is resolved by the LCM load request at runtime:

```sh
export JBPF_CODELETS=/path/to/E3Controller/codelets
```

## Extension IDs

Helper and program-type IDs live in `include/jbpf_e3_ids.h`, allocated from
jbpf's documented extension ranges (`CUSTOM_HELPER_START_ID`,
`CUSTOM_PROGRAM_START_ID`). That header is shared by the codelets, the
verifier, and the gNB-side helper registration in ocudu — if those three ever
disagree, a codelet that verifies cleanly still fails to load with
`call to nonexistent function <id>`.

Never add IDs by editing jbpf's core `enum jbpf_helper_type`: that claims an ID
upstream may later allocate to something else, which would silently bind a
codelet call to the wrong native function.
