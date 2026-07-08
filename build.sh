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
#   2. init + patch jbpf's own 3p submodules
#   3. configure libe3, stage asn1c's BOOLEAN.* skeletons (toolchain workaround),
#      then build + INSTALL to /usr/local (both encoders -> runtime --encoding)
#   4. build the E3Controller (jbpf is built in-tree via add_subdirectory)
#
# Usage: ./build.sh [--install-deps]
#   --install-deps   Install all system packages before building. Debian/Ubuntu
#                    only (uses apt-get). Uses sudo if not already root.
#
# Override parallelism with JOBS=<n> ./build.sh.
set -euo pipefail
cd "$(dirname "$0")"

INSTALL_DEPS=0
for arg in "$@"; do
  case "$arg" in
    --install-deps|-d) INSTALL_DEPS=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "ERROR: unknown argument '$arg'" >&2
       echo "Usage: $0 [--install-deps]" >&2
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
# Step 2: jbpf's own 3p submodules
# ---------------------------------------------------------------------------
echo "==> [2/4] Initialising + patching jbpf 3p submodules"
( cd jbpf && bash ./init_and_patch_submodules.sh )

# ---------------------------------------------------------------------------
# Step 3: libe3
# ---------------------------------------------------------------------------
echo "==> [3/4] Building + installing libe3 (ASN.1 + JSON) to /usr/local"
# JSON encoding needs nlohmann_json >= 3.11 on the system (libe3's floor); install
# it (header-only) or let libe3's FetchContent fetch it when the host has internet.
cmake -S libe3 -B libe3/build -DLIBE3_ENABLE_ASN1=ON -DLIBE3_ENABLE_JSON=ON \
      -DLIBE3_BUILD_EXAMPLES=OFF -DLIBE3_BUILD_TESTS=OFF

# --- toolchain shim: supply BOOLEAN.* to libe3's E3AP runtime --------------
# libe3 0.0.4's messages/asn1/V1/e3ap-1.0.0.cmake hard-lists BOOLEAN.{c,h} and
# BOOLEAN_{aper,print,rfill,uper,xer}.c as asn1c outputs, but its E3AP grammar
# never uses BOOLEAN, so the mouse07410 asn1c fork does NOT emit them and the
# build fails on the missing sources. Rather than patch libe3, drop asn1c's own
# BOOLEAN skeletons into libe3's generated dir before the build (asn1c won't
# overwrite them). libe3 then owns asn_DEF_BOOLEAN; E3Controller links it
# instead of compiling its own (see src/e3sm/asn/CMakeLists.txt). Re-run this
# script after a clean (rm -rf libe3/build) so the copy is re-staged.
BOOLEAN_FILES="BOOLEAN.c BOOLEAN.h BOOLEAN_aper.c BOOLEAN_print.c BOOLEAN_rfill.c BOOLEAN_uper.c BOOLEAN_xer.c"
SKEL="${ASN1C_SKELETON_DIR:-}"
if [ -z "${SKEL}" ]; then
  for d in /opt/asn1c/share/asn1c /usr/local/share/asn1c /usr/share/asn1c; do
    [ -f "$d/BOOLEAN.c" ] && { SKEL="$d"; break; }
  done
fi
[ -n "${SKEL}" ] || { echo "ERROR: asn1c BOOLEAN skeletons not found; set ASN1C_SKELETON_DIR=<dir with BOOLEAN.c>"; exit 1; }
echo "    supplying BOOLEAN skeletons from ${SKEL} -> libe3/build/messages"
mkdir -p libe3/build/messages
for f in ${BOOLEAN_FILES}; do cp "${SKEL}/${f}" libe3/build/messages/; done
# ---------------------------------------------------------------------------

cmake --build libe3/build -j"${JOBS}"
$SUDO cmake --install libe3/build

# ---------------------------------------------------------------------------
# Step 4: E3Controller
# ---------------------------------------------------------------------------
echo "==> [4/4] Building E3Controller"
cmake -S . -B build -DINITIALIZE_SUBMODULES=OFF
cmake --build build -j"${JOBS}" --target e3_controller

echo "==> Done: $(pwd)/out/bin/e3_controller"
