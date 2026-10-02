#!/usr/bin/env bash
#
# One-shot build for the E3Controller.
#
# Build order (libe3 depends on nothing in-tree; the controller depends on a
# system-installed libe3 + an in-tree jbpf):
#
#   0. (optional, --install-deps) install system packages. This step delegates
#      to libe3's own `libe3/build_libe3 -I`, which does an apt install of the
#      build toolchain + libzmq + nlohmann_json + libsctp and builds asn1c from
#      the mouse07410 fork into /opt/asn1c so the file list libe3's E3AP
#      grammar expects is produced. We also apt-install a small set of extras
#      the E3Controller itself needs (python + pip for jbpf's build).
#   1. fetch submodules (libe3 @ pinned tag, jbpf)
#   2. init + patch jbpf's own 3p submodules, then apply our patch to jbpf core
#   3. configure libe3, stage asn1c's BOOLEAN.* skeletons (toolchain workaround),
#      then build + INSTALL to /usr/local (both encoders -> runtime --encoding)
#   4. build the E3Controller (jbpf is built in-tree via add_subdirectory)
#
# Usage: ./build.sh [--install-deps] [--latrec]
#   --install-deps   Install all system packages before building. Debian/Ubuntu
#                    only (uses apt-get). Uses sudo if not already root.
#   --latrec         Build libe3 with -DLIBE3_ENABLE_LATREC=ON, so the stage
#                    recorder is compiled in. Off by default: a normal build has
#                    no recorder in the process and the controller's own stamps
#                    compile to nothing. LIBE3_ENABLE_LATREC is a PUBLIC compile
#                    definition on libe3::libe3, so this one flag reaches the
#                    controller too -- there is nothing to pass twice, and a
#                    mismatch is a link error rather than a silently untraced
#                    build. Set LATREC_DEFAULT_DIR=<path> alongside it to move
#                    the compiled-in default ring directory off /tmp/latrec.
#
# Override parallelism with JOBS=<n> ./build.sh.
# Extra configure flags for the controller: E3C_CMAKE_ARGS='-DUSE_NATIVE=OFF'.
set -euo pipefail
cd "$(dirname "$0")"

INSTALL_DEPS=0
ENABLE_LATREC=0
for arg in "$@"; do
  case "$arg" in
    --install-deps|-d) INSTALL_DEPS=1 ;;
    --latrec) ENABLE_LATREC=1 ;;
    -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
    *) echo "ERROR: unknown argument '$arg'" >&2
       echo "Usage: $0 [--install-deps] [--latrec]" >&2
       exit 2 ;;
  esac
done

JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"

# ---------------------------------------------------------------------------
# sudo shim: prefer no-op when we're already root, and don't require the
# `sudo` binary to be present on minimal container images. libe3's
# `build_libe3` uses `sudo` unconditionally, so on rootless setups it must
# already be installed; we can't help that from here.
# ---------------------------------------------------------------------------
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
elif command -v sudo >/dev/null 2>&1; then
  SUDO="sudo"
else
  echo "ERROR: not running as root and 'sudo' is not installed." >&2
  echo "       Either run this script as root or install sudo." >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Step 0 (optional): install system dependencies.
# ---------------------------------------------------------------------------
if [ "$INSTALL_DEPS" -eq 1 ]; then
  echo "==> [0/4] Installing system dependencies"

  if ! command -v apt-get >/dev/null 2>&1; then
    echo "ERROR: --install-deps expects apt-get (Debian/Ubuntu)." >&2
    echo "       On other distros install the equivalents of the packages" >&2
    echo "       listed in libe3/build_libe3::install_dependencies and re-run" >&2
    echo "       this script without --install-deps." >&2
    exit 1
  fi

  # Bootstrap: libe3's build_libe3 -I runs a `check_command cmake` at the
  # very top and exits BEFORE its `apt-get install` step if cmake is missing,
  # so `-I` can never bootstrap itself on a truly bare image. It also uses
  # `sudo` unconditionally, and it clones asn1c with git. Install these
  # baseline tools ourselves first, then delegate. Everything else libe3
  # needs is inside its own -I installer.
  export DEBIAN_FRONTEND=noninteractive
  $SUDO apt-get update
  $SUDO apt-get install -y --no-install-recommends \
    ca-certificates cmake git
  if ! command -v sudo >/dev/null 2>&1; then
    $SUDO apt-get install -y --no-install-recommends sudo
  fi

  # Make sure libe3's submodule is present before we call its build script.
  git submodule update --init libe3

  # Delegate to libe3's own installer: apt packages + asn1c from mouse07410
  # into /opt/asn1c. Exits after installing per libe3's -I semantics.
  # We invoke via bash explicitly since the file has no .sh extension and
  # may lose its +x bit through git config on some systems.
  bash libe3/build_libe3 -I

  # E3Controller extras beyond what libe3 pulls in:
  #   python3 + pip                 jbpf's nanopb / code-gen tooling
  #   file                          some autotools probes want it
  #   ca-certificates               for https git clones from inside minimal containers
  #   libyaml-cpp-dev               jbpf runtime config parser
  #   libboost-*-dev                jbpf pulls Boost headers + program_options
  #                                 + filesystem for its LCM front-end
  export DEBIAN_FRONTEND=noninteractive
  $SUDO apt-get install -y --no-install-recommends \
    python3 python3-pip python3-dev \
    file ca-certificates \
    libyaml-cpp-dev \
    libboost-dev libboost-program-options-dev libboost-filesystem-dev

  # libe3's -I lands asn1c at /opt/asn1c/bin; make sure the rest of this
  # script (and any child cmake) can find it without a manual PATH edit.
  export PATH="/opt/asn1c/bin:${PATH}"
fi

# ---------------------------------------------------------------------------
# Make asn1c reachable even on non-fresh runs (e.g. a rebuild without
# --install-deps on a machine where the earlier run already installed it
# under /opt/asn1c).
# ---------------------------------------------------------------------------
if [ -x /opt/asn1c/bin/asn1c ] && ! command -v asn1c >/dev/null 2>&1; then
  export PATH="/opt/asn1c/bin:${PATH}"
fi

# ---------------------------------------------------------------------------
# Step 1: submodules
# ---------------------------------------------------------------------------
echo "==> [1/4] Fetching submodules (libe3, jbpf)"
git submodule update --init --recursive

# ---------------------------------------------------------------------------
# Step 2: jbpf's own 3p submodules, then jbpf core
# ---------------------------------------------------------------------------
echo "==> [2/4] Initialising + patching jbpf 3p submodules"
( cd jbpf && bash ./init_and_patch_submodules.sh )

# Then our own patches against jbpf core, which that script knows nothing
# about (it is upstream jbpf's, so edits to it are lost on re-clone).
bash ./apply_jbpf_patches.sh

# ---------------------------------------------------------------------------
# Step 3: libe3
# ---------------------------------------------------------------------------
echo "==> [3/4] Building + installing libe3 (ASN.1 + JSON) to /usr/local"
# JSON encoding needs nlohmann_json >= 3.11 on the system (libe3's floor); install
# it (header-only) or let libe3's FetchContent fetch it when the host has internet.
#
# CMAKE_BUILD_TYPE is NOT optional. libe3's own CMakeLists never defaults it, and
# this line used to omit it, so libe3 compiled with NO -O flag at all:
#
#   CXX_FLAGS = -fPIC -Wall -Wextra ... -std=c++17      # and nothing else
#
# That is the E3AP encoder, the ZMQ connector and the outbound lock-free queue --
# i.e. exactly the stages the RAN->dApp latency budget attributes its residual to
# ("queueing within libe3, E3AP encoding, and the transmission"). An unoptimised
# build inflates all three and the only symptom is a slower number, which is
# indistinguishable from the system genuinely being slow.
#
# Overridable for a debug build:  LIBE3_BUILD_TYPE=RelWithDebInfo ./build.sh
LIBE3_BUILD_TYPE="${LIBE3_BUILD_TYPE:-Release}"
echo "    libe3 CMAKE_BUILD_TYPE=${LIBE3_BUILD_TYPE}"
LIBE3_CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE="${LIBE3_BUILD_TYPE}"
  -DLIBE3_ENABLE_ASN1=ON
  -DLIBE3_ENABLE_JSON=ON
  -DLIBE3_BUILD_EXAMPLES=OFF
  -DLIBE3_BUILD_TESTS=OFF
)
if [ "$ENABLE_LATREC" -eq 1 ]; then
  echo "    latrec: ON (stage recorder compiled in)"
  LIBE3_CMAKE_ARGS+=( -DLIBE3_ENABLE_LATREC=ON )
  # libe3 only defaults LATREC_DEFAULT_DIR into its build tree when it is
  # building its own tests, which we turn off -- so without this the compiled-in
  # default stays latrec.h's /tmp/latrec. Pass it through when the caller names
  # one; logging.latrec_dir in the YAML overrides it per run either way.
  if [ -n "${LATREC_DEFAULT_DIR:-}" ]; then
    echo "    latrec: default ring directory ${LATREC_DEFAULT_DIR}"
    LIBE3_CMAKE_ARGS+=( "-DLATREC_DEFAULT_DIR=${LATREC_DEFAULT_DIR}" )
  fi
fi
cmake -S libe3 -B libe3/build "${LIBE3_CMAKE_ARGS[@]}"

# No BOOLEAN.* skeleton staging here. It existed because Spectrum-ConfigControl
# used BOOLEAN while libe3's E3AP grammar did not, so asn1c never emitted the
# skeleton on libe3's side and we staged it there to have libe3 compile it for
# us. Spectrum-ConfigControl is no longer compiled (src/e3sm/asn/CMakeLists.txt)
# and nothing references asn_DEF_BOOLEAN, so the staging had nothing left to
# supply -- while still aborting the build outright on any host without asn1c's
# reference skeletons on disk.

cmake --build libe3/build -j"${JOBS}"
$SUDO cmake --install libe3/build

# ---------------------------------------------------------------------------
# Step 4: E3Controller
# ---------------------------------------------------------------------------
echo "==> [4/4] Building E3Controller"
# E3C_CMAKE_ARGS lets a caller add configure flags without editing this script.
# CI sets -DUSE_NATIVE=OFF: -march=native is right on a deployment host but wrong
# for a measurement run on whatever CPU a shared runner happens to be, since it
# makes numbers incomparable between runs.
# shellcheck disable=SC2086
cmake -S . -B build -DINITIALIZE_SUBMODULES=OFF ${E3C_CMAKE_ARGS:-}
cmake --build build -j"${JOBS}" --target e3_controller bench_stage_recording

echo "==> Done: $(pwd)/out/bin/e3_controller"
