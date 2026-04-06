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

# ---- spear-libe3 (E3 agent library) ----
# Requires libe3 to be installed: cd ../spear-libe3/build && sudo make install
find_package(libe3 REQUIRED)

# ---- E3Controller executable ----
add_executable(e3_controller
  src/e3_controller.cpp
  src/e3sm_spectrum.cpp
  src/e3sm/e3sm_spect_wrapper.cpp
  src/e3sm/bfp_decompress.cpp
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
  pthread
  rt
  dl
)

# Output the binary to out/bin/
set_target_properties(e3_controller PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/bin"
)
