# e3_verifier_cli — offline PREVAIL verifier for E3Controller's codelets.
#
# Replaces the SDK image's srsran_verifier_cli. Built from the jbpf submodule's
# verifier library, plus the E3 program-type and helper registrations.
#
# Needs ocudu's janus headers for the ctx struct definitions: the context
# descriptors are derived with offsetof() from the RAN's own header rather than
# hard-coded, so the contract has exactly one source of truth.

if(NOT EXISTS "${OCUDU_JANUS_INC}/jbpf_srsran_contexts.h")
  message(WARNING
    "e3_verifier_cli: jbpf_srsran_contexts.h not found under OCUDU_JANUS_INC="
    "'${OCUDU_JANUS_INC}'. Skipping the codelet verifier target. Point OCUDU_DIR "
    "at an ocudu-e3 checkout to build it:\n"
    "  cmake -DOCUDU_DIR=/path/to/ocudu-e3 ...")
  return()
endif()

add_executable(e3_verifier_cli e3_verifier_cli.cpp)

target_link_libraries(e3_verifier_cli PRIVATE jbpf::verifier_lib)

target_include_directories(e3_verifier_cli PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include   # jbpf_e3_ids.h
  ${OCUDU_JANUS_INC}                       # jbpf_srsran_contexts.h (contract)
)

set_target_properties(e3_verifier_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/bin"
)
