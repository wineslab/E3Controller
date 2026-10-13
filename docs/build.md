# Build

Everything goes through the top-level [`build.sh`](../build.sh). The controller is
built against the gNB's own jbpf, so clone [ocudu-e3](https://github.com/wineslab/ocudu-e3)
next to it first (see [Using OCUDU's jbpf](#using-ocudus-jbpf)). Two modes:

```bash
git clone --branch e3 https://github.com/wineslab/ocudu-e3.git
git -C ocudu-e3 submodule update --init --recursive external/jbpf

git clone --recurse-submodules git@github.com:wineslab/E3Controller.git
cd E3Controller

# Fresh Debian/Ubuntu machine (installs every dep first, then builds):
./build.sh --install-deps

# Machine that already has the toolchain in place:
./build.sh
```

The binary lands at `out/bin/e3_controller`. The offline codelet verifier is a
separate target, `cmake --build build --target e3_verifier_cli`, and lands at
`out/bin/e3_verifier_cli` (see [Examples of System Building](#examples-of-system-building)).

<!-- ### `--install-deps` (or `-d`)

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
packages by hand and re-run `./build.sh` without `--install-deps`. -->

## What the normal build steps do

`build.sh` runs, in order:

1. `git submodule update --init --recursive` (fetches libe3 at its pinned tag).
2. Finds the ocudu checkout: `OCUDU_DIR` if set
   (`OCUDU_DIR=/path/to/ocudu ./build.sh`), otherwise `../ocudu-e3` next to this
   repository; a relative path is taken from this repository's root. It stops,
   printing the clone command, if there is none. It then checks that ocudu's
   `external/jbpf` and its third-party submodules are populated, and runs
   ocudu's `apply_jbpf_patches.sh` over them. That script is idempotent, so on a
   tree the gNB build already patched it does nothing.
3. Configure + build libe3 with both encoders
   (`-DLIBE3_ENABLE_ASN1=ON -DLIBE3_ENABLE_JSON=ON -DLIBE3_BUILD_EXAMPLES=OFF
   -DLIBE3_BUILD_TESTS=OFF`), then install it: `sudo cmake --install libe3/build`
   to `/usr/local`, or to `LIBE3_PREFIX` without `sudo`.
4. Configure + build the E3Controller and the recording-cost bench; ocudu's
   jbpf is compiled into this build tree via `add_subdirectory`.

`e3.encoding` in the config is therefore a pure runtime choice, because libe3 is built
with both encoders.

## `--latrec`

Builds libe3 with `-DLIBE3_ENABLE_LATREC=ON`, compiling in the stage recorder —
see [Stage records](stage-records.md). Off by default, and off is the normal way
to run: libe3 only compiles the recorder in when it is set, so without it the
controller's stamps degrade to `latrec.h`'s inline no-ops and nothing links the
recorder runtime.

There is only this one flag. `LIBE3_ENABLE_LATREC` is a `PUBLIC` compile
definition on `libe3::libe3`, so a traced libe3 turns the controller's stamps on
by itself — nothing has to be passed twice, and a mismatch between the two is a
link error rather than a silently untraced build. Set `LATREC_DEFAULT_DIR=<path>`
alongside it to change the compiled-in default ring directory (otherwise
`/tmp/latrec`).

`rm -rf libe3/build` before switching `--latrec` on or off: it changes the
behaviour of an installed public header.

## Overrides & re-runs

- `OCUDU_DIR=/path/to/ocudu ./build.sh` — the ocudu checkout to take jbpf and
  the hook contract from (defaults to `../ocudu-e3`; a relative path is taken
  from this repository's root).
- `LIBE3_PREFIX=$HOME/.local ./build.sh` — install libe3 there instead of
  `/usr/local`, without `sudo`, and build the controller against it. For hosts
  where you are not root.
- `JOBS=N ./build.sh` — parallelism (defaults to `nproc`).
- `E3C_CMAKE_ARGS='-DUSE_NATIVE=OFF' ./build.sh` — extra configure flags for
  the controller. CI passes exactly this: `-march=native` is right on a
  deployment host but wrong for a measurement run on a shared runner, where it
  makes numbers incomparable between runs.
- `LIBE3_BUILD_TYPE=RelWithDebInfo ./build.sh` — libe3's build type.
- If you re-build without `--install-deps` on a host where the earlier run
  put `asn1c` under `/opt/asn1c/`, the script re-adds `/opt/asn1c/bin` to
  `PATH` automatically before invoking `cmake`.

## Examples of System Building

Common ways to run `./build.sh`. It works from any directory; a relative
`OCUDU_DIR` or `LIBE3_PREFIX` is taken from this repository's root.

```bash
# Default layout: ocudu-e3 cloned next to E3Controller.
./build.sh

# Fresh Debian/Ubuntu machine: install the toolchain first (needs apt-get and sudo).
./build.sh --install-deps

# The ocudu checkout is elsewhere, or has another name.
OCUDU_DIR=$HOME/src/my-ocudu ./build.sh

# Not root: install libe3 under your home instead of /usr/local.
LIBE3_PREFIX=$HOME/.local ./build.sh

# Latency tracing (stage recorder). Clear libe3's build when switching it on or off.
rm -rf libe3/build
LATREC_DEFAULT_DIR=/dev/shm/latrec ./build.sh --latrec

# Several test trees on one host, each with its own ocudu and libe3, 8 jobs.
OCUDU_DIR=/work/feature-x/ocudu-e3 LIBE3_PREFIX=/work/feature-x/libe3 JOBS=8 ./build.sh

# Build that behaves the same on any CPU (no -march=native), as in CI.
E3C_CMAKE_ARGS='-DUSE_NATIVE=OFF' ./build.sh
```

The whole system, in order, from a directory that holds both repositories. Build
the gNB first and the controller after: both compile the same jbpf tree, so do
not run the two builds at the same time.

```bash
# 1. gNB with jbpf (see ocudu's README_JBPF.md for the radio options).
cmake -S ocudu-e3 -B ocudu-e3/build -DENABLE_JBPF=ON -DENABLE_DPDK=True \
      -DENABLE_ZEROMQ=ON -DENABLE_UHD=OFF -DASSERT_LEVEL=MINIMAL \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build ocudu-e3/build -j"$(nproc)" --target gnb

# 2. The controller, against the same ocudu checkout.
cd E3Controller
./build.sh

# 3. The offline verifier, then the codelets (verified as they are built).
cmake --build build --target e3_verifier_cli
make -C codelets
```

With this side-by-side layout, ocudu's configure also finds this repository
(`E3CONTROLLER_DIR`, default `../E3Controller`) and stops if the shared E3
headers differ. With another layout, pass the same paths everywhere:
`OCUDU_DIR=...` to `./build.sh` and to `make -C codelets`, and
`-DE3CONTROLLER_DIR=...` to ocudu's `cmake`.

## Using OCUDU's jbpf

The controller is the jbpf IPC primary and the gNB the secondary. Both map one
shared region and exchange one channel and message format, so they must be built
from the same jbpf. This repository therefore has no jbpf of its own: it builds
against `${OCUDU_DIR}/external/jbpf`, the tree the gNB is built from, with the
options ocudu uses (`JBPF_STATIC=ON`, `JBPF_EXPERIMENTAL_FEATURES=ON`). The
patches to that tree belong to ocudu too (`jbpf_patches/`, applied by its
`apply_jbpf_patches.sh`).

Configure reports which jbpf it found, and warns when:

- that jbpf is at a different commit than the one ocudu pins, or
- it lacks `jbpf_monotonic_time.patch`. The controller does not need the patch
  itself, but a gNB built from the same tree would stamp `codelet_ts_ns` with
  `CLOCK_REALTIME` while `gnb_ts_ns` is `CLOCK_MONOTONIC`.

jbpf builds `libck` inside its own source tree (`3p/ck`), as it does in ocudu's
build, so do not build ocudu and the controller at the same time from one
checkout. Run one after the other.

The codelet Makefiles follow the same rule: `JBPF_DIR` defaults to
`$(OCUDU_DIR)/external/jbpf`. Set `JBPF_DIR` (to `make`, or `-DJBPF_DIR` to
CMake) only to build against a different jbpf on purpose.

### Updating a clone made before this change

Older clones still carry the removed jbpf submodule: a `jbpf/` directory that
git now shows as untracked, its git data in `.git/modules/jbpf`, and a
`submodule.jbpf` entry in `.git/config`. Nothing uses them any more. Remove them
once, together with a `build/` configured against them:

```bash
git config --remove-section submodule.jbpf
rm -rf jbpf .git/modules/jbpf build
./build.sh
```

Local changes you made inside the old `jbpf/` are not used either. If you still
need one, it belongs in ocudu's `jbpf_patches/`.

## ASN.1 Code Generation

The build automatically:
1. Runs `asn1c` on the ASN.1 grammars in `src/e3sm/asn/` (`e3sm_spectrum.asn` for
   RF=1, `e3sm_layer1.asn` for RF=2) to generate C encoder/decoder code
2. Compiles the generated code into a static library (`e3sm_asn`) linked to `e3_controller`

Generated files go into `build/asn1c_generated/` and are **not** tracked in git.
asn1c writes one `.c` per type. When you add a type to an `.asn`, add its `.c` to
`ASN1C_SRCS` in `src/e3sm/asn/CMakeLists.txt`; an existing build folder then
regenerates on the next build.
