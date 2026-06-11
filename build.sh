#!/usr/bin/env bash
#
# One-shot build for the E3Controller.
#
# Build order (libe3 depends on nothing in-tree; the controller depends on a
# system-installed libe3 + an in-tree jbpf):
#
#   1. fetch submodules (libe3 @ pinned tag, jbpf)
#   2. init + patch jbpf's own 3p submodules
#   3. apply the one required libe3 change (patches/01-dual-encoding.patch)
#   4. configure libe3, stage asn1c's BOOLEAN.* skeletons (toolchain workaround),
#      then build + INSTALL to /usr/local (both encoders -> runtime --encoding)
#   5. build the E3Controller (jbpf is built in-tree via add_subdirectory)
#
# Needs nlohmann_json >= 3.11 installed (libe3's JSON floor). Linux only (jbpf
# pulls linux/vm_sockets.h). Run from anywhere; we cd to the repo root. Override
# parallelism with JOBS=<n> ./build.sh.
set -euo pipefail
cd "$(dirname "$0")"

JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"

echo "==> [1/5] Fetching submodules (libe3, jbpf)"
git submodule update --init --recursive

echo "==> [2/5] Initialising + patching jbpf 3p submodules"
( cd jbpf && bash ./init_and_patch_submodules.sh )

echo "==> [3/5] Applying required libe3 patch (01-dual-encoding)"
# The one functional libe3 change this release carries: relax the both-encoders
# FATAL_ERROR guard so a single build serves --encoding asn1|json. Idempotent —
# skipped if already applied (manually, or on a re-run).
PATCH01="$(pwd)/patches/01-dual-encoding.patch"
if git -C libe3 apply --check "${PATCH01}" 2>/dev/null; then
  git -C libe3 apply "${PATCH01}"
  echo "    applied 01-dual-encoding.patch"
else
  echo "    01-dual-encoding.patch already applied (or n/a) — skipping"
fi

echo "==> [4/5] Building + installing libe3 (ASN.1 + JSON) to /usr/local"
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
  for d in /usr/local/share/asn1c /opt/asn1c/share/asn1c /usr/share/asn1c; do
    [ -f "$d/BOOLEAN.c" ] && { SKEL="$d"; break; }
  done
fi
[ -n "${SKEL}" ] || { echo "ERROR: asn1c BOOLEAN skeletons not found; set ASN1C_SKELETON_DIR=<dir with BOOLEAN.c>"; exit 1; }
echo "    supplying BOOLEAN skeletons from ${SKEL} -> libe3/build/messages"
mkdir -p libe3/build/messages
for f in ${BOOLEAN_FILES}; do cp "${SKEL}/${f}" libe3/build/messages/; done
# ---------------------------------------------------------------------------

cmake --build libe3/build -j"${JOBS}"
sudo cmake --install libe3/build

echo "==> [5/5] Building E3Controller"
cmake -S . -B build -DINITIALIZE_SUBMODULES=OFF
cmake --build build -j"${JOBS}" --target e3_controller

echo "==> Done: $(pwd)/out/bin/e3_controller"
