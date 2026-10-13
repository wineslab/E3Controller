cmake_minimum_required(VERSION 3.16)

project(e3_controller CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Build options ----
option(USE_NATIVE "Use -march=native" ON)

if(USE_NATIVE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Output directory ----
set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/out)
file(MAKE_DIRECTORY ${OUTPUT_DIR})
file(MAKE_DIRECTORY ${OUTPUT_DIR}/inc)
file(MAKE_DIRECTORY ${OUTPUT_DIR}/lib)

# ---- ocudu checkout ----
# The gNB this controller talks to is built from an ocudu checkout, and two things
# come from it: the jbpf tree (below) and the hook context contract the codelets
# and the verifier read (include/ocudu/janus). Clone it next to this repository,
# or point OCUDU_DIR at it.
set(OCUDU_DIR "${CMAKE_SOURCE_DIR}/../ocudu-e3"
    CACHE PATH "Path to the ocudu checkout the gNB is built from")
get_filename_component(OCUDU_DIR "${OCUDU_DIR}" ABSOLUTE)

# ---- jbpf: the gNB's own tree ----
# The controller is the jbpf IPC primary and the gNB the secondary. Both map one
# shared region and exchange one channel and message format, so they must be
# built from the SAME jbpf. Instead of pinning a second copy here and keeping the
# two pins, and their patches, in step by hand, build against ocudu's
# external/jbpf: the tree the gNB is built from. Its patches are ocudu's too
# (ocudu apply_jbpf_patches.sh; build.sh runs it).
#
# Pass -DJBPF_DIR only to build against a different jbpf on purpose.
if(NOT JBPF_DIR)
  set(JBPF_DIR "${OCUDU_DIR}/external/jbpf")
endif()
get_filename_component(JBPF_DIR "${JBPF_DIR}" ABSOLUTE)

if(NOT EXISTS "${JBPF_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "jbpf not found at JBPF_DIR='${JBPF_DIR}'.\n"
    "The controller builds against the gNB's jbpf (ocudu's external/jbpf). "
    "Clone ocudu next to this repository, or pass -DOCUDU_DIR=/path/to/ocudu, "
    "then populate and patch it:\n"
    "  git -C <ocudu> submodule update --init --recursive external/jbpf\n"
    "  <ocudu>/apply_jbpf_patches.sh")
endif()

# Disable tests and examples — we only need the libraries
set(BUILD_TESTING OFF CACHE BOOL "Disable jbpf tests" FORCE)

# Stop jbpf from re-initialising its own submodules.
#
# jbpf's CMakeLists.txt runs init_and_patch_submodules.sh when INITIALIZE_SUBMODULES
# is ON, which is its default. That script `rm -rf`s and re-clones 3p/*, so on any
# tree where git cannot resolve the submodules — a tarball, an `oc cp`/rsync copy
# into a container — it DESTROYS the working 3p checkout and then fails. The tree
# is ocudu's, populated and patched on ocudu's side, so there is nothing to gain
# from letting jbpf redo it. ocudu forces the same value for the same reason
# (ocudu CMakeLists.txt, the ENABLE_JBPF block).
#
# FORCE because it is a cached option in jbpf; without FORCE a stale ON from an
# earlier configure would survive.
set(INITIALIZE_SUBMODULES OFF CACHE BOOL "Let jbpf init its own submodules" FORCE)

# Build jbpf with the options ocudu builds it with (ocudu CMakeLists.txt, the
# ENABLE_JBPF block), so both ends of the IPC are the same jbpf in every respect,
# not just the same commit.
set(JBPF_STATIC ON CACHE BOOL "Build jbpf as static library" FORCE)
set(JBPF_EXPERIMENTAL_FEATURES ON CACHE BOOL "Enable jbpf experimental features" FORCE)

# Fail early and legibly if the 3p submodules are absent, rather than letting the
# first missing header surface hundreds of lines into the build.
#
# A per-module sentinel FILE, one that module actually ships.
#
# Two traps to avoid here, both hit while testing this:
#   - `ck` is autotools (configure/Makefile.in), so CMakeLists.txt is the wrong
#     sentinel for it and gives a false alarm.
#   - `file(GLOB dir/*)` is NOT a usable emptiness test: unlike a shell glob,
#     CMake's matches dotfiles, so a de-populated submodule that still has
#     .git/.github looks fully populated.
set(_jbpf_3p_checks
  "ubpf/CMakeLists.txt"
  "ebpf-verifier/CMakeLists.txt"
  "mimalloc/CMakeLists.txt"
  "ck/configure")
foreach(_jbpf_3p_file IN LISTS _jbpf_3p_checks)
  if(NOT EXISTS "${JBPF_DIR}/3p/${_jbpf_3p_file}")
    message(FATAL_ERROR
      "${JBPF_DIR}/3p/${_jbpf_3p_file} is missing, so jbpf's third-party "
      "submodules are not populated. jbpf's own auto-init is disabled here (see "
      "above, it would destroy them), so populate them on the ocudu side:\n"
      "  git -C ${OCUDU_DIR} submodule update --init --recursive external/jbpf\n"
      "  ${OCUDU_DIR}/apply_jbpf_patches.sh")
  endif()
endforeach()
unset(_jbpf_3p_checks)

# Say which jbpf this is, and whether it is the one ocudu pins. A jbpf checked
# out at another commit than ocudu's gitlink would build a gNB and a controller
# that disagree, so warn rather than let that surface as an IPC failure.
execute_process(COMMAND git -C "${JBPF_DIR}" rev-parse HEAD
                OUTPUT_VARIABLE _jbpf_rev OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _jbpf_rev_rc)
execute_process(COMMAND git -C "${OCUDU_DIR}" rev-parse HEAD:external/jbpf
                OUTPUT_VARIABLE _jbpf_pin OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _jbpf_pin_rc)
if(NOT _jbpf_rev_rc EQUAL 0)
  set(_jbpf_rev "unknown (not a git checkout)")
endif()
if(_jbpf_rev_rc EQUAL 0 AND _jbpf_pin_rc EQUAL 0 AND NOT _jbpf_rev STREQUAL _jbpf_pin)
  message(WARNING
    "jbpf at ${JBPF_DIR} is at ${_jbpf_rev}, but ocudu pins external/jbpf at "
    "${_jbpf_pin}. The gNB and the controller must be built from the same jbpf:\n"
    "  git -C ${OCUDU_DIR} submodule update --init --recursive external/jbpf")
endif()

# The gNB stamps gnb_ts_ns with CLOCK_MONOTONIC, and the codelet's stamps come from
# jbpf_time_get_ns(), which is only monotonic with ocudu's
# jbpf_patches/jbpf_monotonic_time.patch applied. The controller does not run
# codelets, so this build does not need it, but a gNB built from this same tree
# would produce stage intervals that mix two clocks. Upstream jbpf calls
# clock_gettime exactly once in that file, with CLOCK_REALTIME.
file(STRINGS "${JBPF_DIR}/src/core/jbpf_helper_impl.c" _jbpf_mono
     REGEX "clock_gettime\\(CLOCK_MONOTONIC")
if(_jbpf_mono)
  set(_jbpf_clock "monotonic patch applied")
else()
  set(_jbpf_clock "monotonic patch NOT applied")
  message(WARNING
    "jbpf at ${JBPF_DIR} lacks jbpf_monotonic_time.patch, so a gNB built from it "
    "stamps codelet_ts_ns with CLOCK_REALTIME while gnb_ts_ns is CLOCK_MONOTONIC. "
    "Apply it on the ocudu side:  ${OCUDU_DIR}/apply_jbpf_patches.sh")
endif()
message(STATUS "jbpf: ${JBPF_DIR} @ ${_jbpf_rev} (${_jbpf_clock})")
unset(_jbpf_rev)
unset(_jbpf_pin)
unset(_jbpf_mono)
unset(_jbpf_clock)

# Override add_test and set_tests_properties so jbpf subdir doesn't register tests
macro(add_test)
endmacro()
macro(set_tests_properties)
endmacro()

# JBPF_DIR is outside this source tree, so add_subdirectory needs an explicit
# binary directory. jbpf writes its libraries and copied headers under
# ${CMAKE_SOURCE_DIR}/out (this project's out/). Two things still land in ocudu's
# tree, as they do in ocudu's own build: libck, which jbpf builds in-source in
# 3p/ck, and src/common/jbpf_version.h, which it regenerates on every build (build
# parameters as strings only). Do not build ocudu and the controller at the same
# time from one checkout.
add_subdirectory("${JBPF_DIR}" "${CMAKE_BINARY_DIR}/jbpf")

# Restore original cmake commands
unset(add_test)
unset(set_tests_properties)

# ---- Codelet verifier (offline) ----
# The gNB does not verify codelets at load time: jbpf's path is
# ubpf_load_elf_ex + ubpf_compile, and ubpf's JIT emits no memory bounds
# checks. This binary, run from codelets/Makefile at build time, is therefore
# the only memory-safety gate these codelets ever pass through.
#
# It needs ocudu's janus headers, which define the hook context structs — the
# verifier derives its ctx descriptors from them with offsetof() so the
# contract has a single source of truth. They come from OCUDU_DIR (above).
set(OCUDU_JANUS_INC "${OCUDU_DIR}/include/ocudu/janus")
add_subdirectory(codelets/verifier)

# ---- ASN.1 (E3 Service Model) ----
add_subdirectory(src/e3sm/asn)

# ---- tl::expected (required by libe3 but not installed with it) ----
include(FetchContent)
FetchContent_Declare(
  tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.1.0
  GIT_SHALLOW TRUE
)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tl_expected)

# ---- yaml-cpp (E3Controller configuration) ----
# The controller is configured by a single YAML file rather than 20 CLI options.
# YAML specifically (not JSON, which would be free via libe3's nlohmann) because
# ocudu's own configuration is YAML, including the `jbpf:` section this file must
# agree with — keeping both sides in the same language makes them diffable and
# reviewable together, which is the point when the invariant is "these must match".
#
# FetchContent rather than a system package, matching the tl::expected pattern
# above, so the build stays self-contained.
FetchContent_Declare(
  yaml_cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
  GIT_SHALLOW TRUE
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml_cpp)

# ---- libe3 (E3 agent library) ----
# libe3 is the `libe3/` git submodule (https://github.com/wineslab/libe3.git,
# pinned to tag 0.0.4). build.sh builds it with BOTH encoders and installs it to
# /usr/local before configuring this project; we consume the installed CMake
# package (and its ASN.1 runtime headers, see src/e3sm/asn) here. To build it by
# hand:
#   cmake -S libe3 -B libe3/build -DLIBE3_ENABLE_ASN1=ON -DLIBE3_ENABLE_JSON=ON \
#         -DLIBE3_BUILD_EXAMPLES=OFF -DLIBE3_BUILD_TESTS=OFF
#   cmake --build libe3/build -j && sudo cmake --install libe3/build
find_package(libe3 REQUIRED)

# ---- libe3 staleness stamp ----
#
# libe3::libe3 is STATIC IMPORTED (see /usr/local/lib/cmake/libe3/libe3Targets.cmake),
# so this binary carries a SNAPSHOT of liblibe3.a taken at link time. Reinstalling
# libe3 afterwards changes nothing until the controller is relinked, and there is
# no runtime symptom: a changed SLEEP_DURATION or E3AP encoder is simply absent.
# Record what we linked against so the binary can say so on startup and warn when
# /usr/local has moved on. Costs one stat() at boot.
get_filename_component(LIBE3_PREFIX "${libe3_DIR}/../../.." ABSOLUTE)
set(LIBE3_STAMP_LIB       "${LIBE3_PREFIX}/lib/liblibe3.a")
set(LIBE3_STAMP_VERSION   "${libe3_VERSION}")
set(LIBE3_STAMP_BUILDTYPE "${CMAKE_BUILD_TYPE}")

# Bake the queue's poll period: it is the value most often changed by hand and
# the one whose staleness is least visible (it only shows up as a shifted
# outbound queue-wait distribution).
set(LIBE3_STAMP_SLEEP_US "unknown")
if(EXISTS "${LIBE3_PREFIX}/include/libe3/lockfree_queue.hpp")
  file(STRINGS "${LIBE3_PREFIX}/include/libe3/lockfree_queue.hpp"
       _libe3_sleep_line REGEX "SLEEP_DURATION[ \t]*=")
  string(REGEX MATCH "microseconds\\(([0-9]+)\\)" _m "${_libe3_sleep_line}")
  if(CMAKE_MATCH_1)
    set(LIBE3_STAMP_SLEEP_US "${CMAKE_MATCH_1}")
  endif()
endif()
message(STATUS "libe3: ${LIBE3_STAMP_VERSION} from ${LIBE3_PREFIX} "
               "(static, SLEEP_DURATION=${LIBE3_STAMP_SLEEP_US}us)")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/libe3_stamp.h.in"
               "${CMAKE_BINARY_DIR}/generated/libe3_stamp.h" @ONLY)

# ---- E3Controller executable ----
add_executable(e3_controller
  src/e3_controller.cpp
  # RF=1 Spectrum SM (PRB-blacklist / spectrum sharing)
  src/e3sm/sm_spectrum/e3sm_spectrum.cpp
  src/e3sm/sm_spectrum/e3sm_spect_wrapper.cpp
  # RF=2 L1-KPM SM (NVIDIA-Aerial-conformant indications)
  src/e3sm/l1_kpm/e3sm_layer_1.cpp
  src/e3sm/l1_kpm/e3sm_layer1_wrapper.cpp
  src/e3sm/l1_kpm/e3sm_layer1_json.cpp
  # shared data plane
  src/e3sm/iq_pipeline.cpp
  src/e3sm/slot_iq_pipeline.cpp
  # configuration (YAML)
  src/e3_config.cpp
  # writers + decompression
  src/e3sm/utils/e3sm_shm_writer.cpp
  src/e3sm/utils/bfp_decompress.cpp
)

# Include paths: our own headers + jbpf headers
target_include_directories(e3_controller PRIVATE
  ${CMAKE_BINARY_DIR}/generated        # libe3_stamp.h
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${JBPF_DIR}/src/io
  ${JBPF_DIR}/src/mem_mgmt
  ${JBPF_DIR}/src/logger
  ${JBPF_DIR}/src/common
  ${OUTPUT_DIR}/inc
  ${JBPF_DIR}/src/lcm
)

# Link against jbpf_io (transitively brings mem_mgmt, logger, ck, mimalloc)
target_link_libraries(e3_controller PRIVATE
  jbpf::io_lib
  jbpf::lcm_ipc_lib
  e3sm_asn
  libe3::libe3
  yaml-cpp
  pthread
  rt
  dl
)

# Output the binary to out/bin/
set_target_properties(e3_controller PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/bin"
)

# ---- Recording-cost bench ----
# Prices the stage recorder that replaced the per-slot stats CSV. Deliberately
# minimal: it links the same trace header the slot handler uses and nothing
# else -- no jbpf, no Service Model, no shared-memory writer, no encoders. That
# is enough to measure the recording mechanism, and it keeps the target
# buildable (and CI-runnable) with no RAN attached.
#
# There is no "real slot work" arm to link for: under the default
# shm.writer: gnb the gNB converts and writes the row itself, so the controller
# moves no slot data at all. See bench/bench_stage_recording.cpp.
option(E3C_BUILD_BENCH "Build the stage-recording cost bench" ON)

if(E3C_BUILD_BENCH)
  add_executable(bench_stage_recording bench/bench_stage_recording.cpp)
  target_include_directories(bench_stage_recording PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(bench_stage_recording PRIVATE libe3::libe3 pthread rt)
  set_target_properties(bench_stage_recording PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/bin"
  )
endif()
