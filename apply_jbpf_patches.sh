#!/usr/bin/env bash
#
# Apply this repo's patches against the jbpf submodule.
#
# Run after `git submodule update --init --recursive` and before configuring.
# build.sh calls it as step 2; it is also safe to run by hand.
#
# Idempotent: an already-patched tree is detected and skipped.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
JBPF_DIR="${REPO_DIR}/jbpf"
OURS_DIR="${REPO_DIR}/jbpf_patches"

if [ ! -f "${JBPF_DIR}/CMakeLists.txt" ]; then
  echo "ERROR: jbpf submodule at ${JBPF_DIR} is not populated." >&2
  echo "       Run: git submodule update --init --recursive" >&2
  exit 1
fi

# run_patch <git|patch> <tree> <patch_file> <forward|reverse> <dry|real>
#
# One call site for both back-ends so the two never drift in what they
# consider "applies". Returns the tool's exit status; prints nothing.
run_patch() {
  local mode="$1" tree="$2" file="$3" dir="$4" kind="$5"
  local -a args=()
  if [ "${dir}" = reverse ]; then args+=(--reverse); fi

  # ${args[@]+...}: a forward+real call leaves args empty, and bash 3.2 (still
  # the /bin/bash on macOS) treats "${args[@]}" as unbound under `set -u`.
  if [ "${mode}" = git ]; then
    if [ "${kind}" = dry ]; then args+=(--check); fi
    git -C "${tree}" apply ${args[@]+"${args[@]}"} "${file}" >/dev/null 2>&1
  else
    if [ "${kind}" = dry ]; then args+=(--dry-run); fi
    ( cd "${tree}" && patch -p1 --silent --force --no-backup-if-mismatch \
        ${args[@]+"${args[@]}"} <"${file}" ) >/dev/null 2>&1
  fi
}

apply_patch() {
  local patch="$1"
  local patch_file="${OURS_DIR}/${patch}"

  if [ ! -f "${patch_file}" ]; then
    echo "ERROR: ${patch} is missing from ${OURS_DIR}." >&2
    exit 1
  fi

  # git apply where the tree is a real checkout, patch(1) where it is not.
  # The fallback is not hypothetical: deployment rsyncs this tree into the pod
  # without .git, and there `git apply` fails at repository DISCOVERY --
  # indistinguishable, from the exit status alone, from a conflicting patch.
  local mode=git
  git -C "${JBPF_DIR}" rev-parse --git-dir >/dev/null 2>&1 || mode=patch

  if run_patch "${mode}" "${JBPF_DIR}" "${patch_file}" reverse dry; then
    echo "==> jbpf: ${patch} already applied, skipping"
  elif run_patch "${mode}" "${JBPF_DIR}" "${patch_file}" forward dry; then
    echo "==> jbpf: applying ${patch}"
    run_patch "${mode}" "${JBPF_DIR}" "${patch_file}" forward real
  else
    echo "ERROR: ${patch} does not apply cleanly to jbpf (nor is it already" >&2
    echo "       applied). The tree may have local modifications; inspect it," >&2
    echo "       or re-init with:" >&2
    echo "       git submodule update --init --recursive --force jbpf" >&2
    exit 1
  fi
}

# Upstream jbpf_time_get_ns() stamps CLOCK_REALTIME, and that is the clock the
# slot codelet writes into codelet_ts_ns. The gNB, this controller and the dApp
# all read CLOCK_MONOTONIC, so leaving it unpatched puts codelet_ts_ns in a
# different epoch from gnb_ts_ns: gnb_to_codelet_us then comes out silently
# wrong rather than failing.
apply_patch jbpf_monotonic_time.patch

echo "==> All E3Controller jbpf patches applied."
