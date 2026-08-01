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

# ---- Build jbpf (full build) ----
# Disable tests and examples — we only need the libraries
set(BUILD_TESTING OFF CACHE BOOL "Disable jbpf tests" FORCE)

# Override add_test and set_tests_properties so jbpf subdir doesn't register tests
macro(add_test)
endmacro()
macro(set_tests_properties)
endmacro()

add_subdirectory(jbpf)

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
# contract has a single source of truth. Point OCUDU_DIR elsewhere if your
# checkout is not a sibling of this one.
set(OCUDU_DIR "${CMAKE_SOURCE_DIR}/../ocudu-e3"
    CACHE PATH "Path to the ocudu-e3 checkout (hook context contract)")
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
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/jbpf/src/io
  ${CMAKE_SOURCE_DIR}/jbpf/src/mem_mgmt
  ${CMAKE_SOURCE_DIR}/jbpf/src/logger
  ${CMAKE_SOURCE_DIR}/jbpf/src/common
  ${OUTPUT_DIR}/inc
  ${CMAKE_SOURCE_DIR}/jbpf/src/lcm
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
