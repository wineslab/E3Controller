#!/usr/bin/env bash
#
# One-shot build for the E3Controller.
#
# Build order (libe3 depends on nothing in-tree; the controller depends on an
# installed libe3 + the gNB's jbpf, from the ocudu checkout next to this one):
#
#   0. (optional, --install-deps) install system packages. This step delegates
#      to libe3's own `libe3/build_libe3 -I`, which does an apt install of the
#      build toolchain + libzmq + nlohmann_json + libsctp and builds asn1c from
#      the mouse07410 fork into /opt/asn1c so the file list libe3's E3AP
#      grammar expects is produced. We also apt-install a small set of extras
#      the E3Controller itself needs (python + pip for jbpf's build).
#   1. fetch the libe3 submodule (pinned tag)
#   2. check that ocudu's external/jbpf is populated, then run ocudu's
#      apply_jbpf_patches.sh over it (idempotent: a no-op on a patched tree)
#   3. configure libe3, then build + INSTALL it (to /usr/local, or LIBE3_PREFIX)
#      with both encoders, so the encoding is a runtime choice
#   4. build the E3Controller (ocudu's jbpf is built via add_subdirectory)
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
# Environment:
#   OCUDU_DIR=<path>     The ocudu checkout the gNB is built from (default
#                        ../ocudu-e3, relative to this repository). Its
#                        external/jbpf is the jbpf this build uses, and its
#                        include/ocudu/janus the hook contract.
#   LIBE3_PREFIX=<path>  Install libe3 there instead of /usr/local, without sudo,
#                        and build the controller against it. For hosts where
#                        you are not root.
#   JOBS=<n>             Parallelism (default: nproc).
#   E3C_CMAKE_ARGS=...   Extra configure flags for the controller, e.g.
#                        '-DUSE_NATIVE=OFF'.
set -euo pipefail
cd "$(dirname "$0")"

INSTALL_DEPS=0
ENABLE_LATREC=0
for arg in "$@"; do
  case "$arg" in
    --install-deps|-d) INSTALL_DEPS=1 ;;
    --latrec) ENABLE_LATREC=1 ;;
    -h|--help) sed -n '2,44p' "$0"; exit 0 ;;
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
echo "==> [1/4] Fetching submodules (libe3)"
git submodule update --init --recursive

# ---------------------------------------------------------------------------
# Step 2: the gNB's jbpf, from the ocudu checkout
# ---------------------------------------------------------------------------
# The controller (jbpf IPC primary) and the gNB (secondary) must be built from
# the same jbpf, so this build uses ocudu's external/jbpf rather than a copy of
# its own. ocudu owns that tree and its patches; we only check that it is there
# and run ocudu's own, idempotent, patch script over it.
# A relative OCUDU_DIR is taken from this repository's root (we cd'd there).
OCUDU_DIR_IN="${OCUDU_DIR:-../ocudu-e3}"
if ! OCUDU_DIR="$(cd "${OCUDU_DIR_IN}" 2>/dev/null && pwd)"; then
  echo "ERROR: no ocudu checkout at OCUDU_DIR='${OCUDU_DIR_IN}'." >&2
  echo "       Clone it next to this repository, or point OCUDU_DIR at it:" >&2
  echo "         git clone --branch e3 https://github.com/wineslab/ocudu-e3.git ../ocudu-e3" >&2
  echo "         OCUDU_DIR=/path/to/ocudu ./build.sh" >&2
  exit 1
fi
JBPF_DIR="${OCUDU_DIR}/external/jbpf"
echo "==> [2/4] Preparing the gNB's jbpf (${JBPF_DIR})"

# Same sentinels as CMakeLists.txt: one file each 3p module actually ships.
for sentinel in CMakeLists.txt 3p/ubpf/CMakeLists.txt 3p/ebpf-verifier/CMakeLists.txt \
                3p/mimalloc/CMakeLists.txt 3p/ck/configure; do
  if [ ! -f "${JBPF_DIR}/${sentinel}" ]; then
    echo "ERROR: ${JBPF_DIR}/${sentinel} is missing: ocudu's jbpf submodule is" >&2
    echo "       not populated. On the ocudu side, run:" >&2
    echo "         git -C ${OCUDU_DIR} submodule update --init --recursive external/jbpf" >&2
    exit 1
  fi
done

if [ ! -f "${OCUDU_DIR}/apply_jbpf_patches.sh" ]; then
  echo "ERROR: ${OCUDU_DIR}/apply_jbpf_patches.sh not found; this ocudu checkout" >&2
  echo "       predates the jbpf integration. Use the e3 branch." >&2
  exit 1
fi
bash "${OCUDU_DIR}/apply_jbpf_patches.sh"

# ---------------------------------------------------------------------------
# Step 3: libe3
# ---------------------------------------------------------------------------
LIBE3_PREFIX="${LIBE3_PREFIX:-}"
if [ -n "${LIBE3_PREFIX}" ]; then
  mkdir -p "${LIBE3_PREFIX}"
  LIBE3_PREFIX="$(cd "${LIBE3_PREFIX}" && pwd)"
fi
echo "==> [3/4] Building + installing libe3 (ASN.1 + JSON) to ${LIBE3_PREFIX:-/usr/local}"
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
if [ -n "${LIBE3_PREFIX}" ]; then
  LIBE3_CMAKE_ARGS+=( -DCMAKE_INSTALL_PREFIX="${LIBE3_PREFIX}" )
fi
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
if [ -n "${LIBE3_PREFIX}" ]; then
  cmake --install libe3/build
else
  $SUDO cmake --install libe3/build
fi

# ---------------------------------------------------------------------------
# Step 4: E3Controller
# ---------------------------------------------------------------------------
echo "==> [4/4] Building E3Controller"
# E3C_CMAKE_ARGS lets a caller add configure flags without editing this script.
# CI sets -DUSE_NATIVE=OFF: -march=native is right on a deployment host but wrong
# for a measurement run on whatever CPU a shared runner happens to be, since it
# makes numbers incomparable between runs.
E3C_PREFIX_ARGS=()
if [ -n "${LIBE3_PREFIX}" ]; then
  # find_package(libe3) and the ASN.1 runtime headers (src/e3sm/asn) both have to
  # look in the prefix, or they silently pick up an older libe3 in /usr/local.
  E3C_PREFIX_ARGS+=( -DCMAKE_PREFIX_PATH="${LIBE3_PREFIX}"
                     -DLIBE3_ASN1_INCLUDE_DIR="${LIBE3_PREFIX}/include/libe3/asn1" )
fi
# shellcheck disable=SC2086
cmake -S . -B build -DOCUDU_DIR="${OCUDU_DIR}" \
  ${E3C_PREFIX_ARGS[@]+"${E3C_PREFIX_ARGS[@]}"} ${E3C_CMAKE_ARGS:-}
cmake --build build -j"${JOBS}" --target e3_controller bench_stage_recording

echo "==> Done: $(pwd)/out/bin/e3_controller"
