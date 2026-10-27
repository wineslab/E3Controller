# ASN.1 code generation using asn1c (requires mouse07410/asn1c fork with APER support)
# Assumes asn1c is already installed system-wide.
# Generates C sources from our SM .asn files at configure time, compiles into static library.
#
# IMPORTANT: Only our SM type-specific files are compiled here (Spectrum-* and
# L1KPM-*). The asn1c runtime (aper_encoder.c, constr_SEQUENCE.c, etc.) is
# provided by the installed libe3 library (via asn1_e3ap). We MUST NOT compile
# our own copy of the runtime, because different asn1c versions produce
# incompatible struct layouts, leading to ABI mismatches and crashes.

find_program(ASN1C_EXECUTABLE asn1c REQUIRED)

# Locate libe3's installed ASN.1 runtime headers (aper_encoder.h, OCTET_STRING.h, etc.)
# We use find_path instead of find_package(libe3) because tl::expected may not
# be available yet at this point in the configure sequence.
find_path(LIBE3_ASN1_INCLUDE_DIR
    NAMES aper_encoder.h OCTET_STRING.h constr_SEQUENCE.h
    HINTS /usr/local/include/libe3/asn1
    REQUIRED
)

set(ASN1C_OUTPUT_DIR "${CMAKE_BINARY_DIR}/asn1c_generated")
# Two SM grammars share one output dir so cross-references between modules
# resolve in one asn1c pass. Order doesn't matter to asn1c.
set(ASN1_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/e3sm_spectrum.asn"
    "${CMAKE_CURRENT_SOURCE_DIR}/e3sm_layer1.asn"
)
set(ASN1C_STAMP "${ASN1C_OUTPUT_DIR}/.asn1c_stamp")

# The codecs asn1c generates code for. They must be libe3's (libe3
# messages/CMakeLists.txt): asn1c writes them into asn_config.h as
# ASN_DISABLE_*_SUPPORT, and that header sets the layout of every
# asn_TYPE_operation_t. Only libe3's asn_config.h is on the include path (the
# cleanup below deletes ours), so a type generated with other codecs would be
# compiled against a runtime that has a different idea of its tables.
set(ASN1C_CODEC_FLAGS -no-gen-OER -no-gen-BER -no-gen-JER)

# BOOLEAN. Spectrum-SensingPolicyControl.deactivate (an alternative of
# Spectrum-DAppControlPayload) and Spectrum-ConfigControl.enable are BOOLEAN, and
# libe3 >= 0.0.7 ships no BOOLEAN runtime. asn1c copies its own BOOLEAN skeleton
# next to the types; keep it and compile it here, with the codec set above, so it
# is built exactly like libe3's runtime. Only when libe3 has none: a libe3 that
# adds one would otherwise define asn_DEF_BOOLEAN twice.
if(EXISTS "${LIBE3_ASN1_INCLUDE_DIR}/BOOLEAN.h")
    set(E3SM_OWN_BOOLEAN OFF)
else()
    set(E3SM_OWN_BOOLEAN ON)
endif()
message(STATUS "e3sm_asn: BOOLEAN runtime from ${LIBE3_ASN1_INCLUDE_DIR}/BOOLEAN.h: "
               "${E3SM_OWN_BOOLEAN} (ON = libe3 has none, compile asn1c's)")

# Only our SM type-specific generated files. asn1c emits one .c per top-level
# type definition; both modules share the same constraint-table / per-codec
# routines via the libe3 runtime.
#
# The Spectrum types are the ones the controller uses: the RAN function data,
# the dApp control envelope (with both of its alternatives, which a CHOICE needs)
# and the legacy IQ indication. The other types in the grammar (dApp report,
# xApp control, apply outcome, sensing indication) are generated but not
# compiled: the controller relays reports and controls as opaque bytes.
set(ASN1C_SRCS
    "${ASN1C_OUTPUT_DIR}/Spectrum-RanFunctionData.c"
    "${ASN1C_OUTPUT_DIR}/Spectrum-DAppControlData.c"
    "${ASN1C_OUTPUT_DIR}/Spectrum-DAppControlPayload.c"
    "${ASN1C_OUTPUT_DIR}/Spectrum-PRBBlacklistControl.c"
    "${ASN1C_OUTPUT_DIR}/Spectrum-SensingPolicyControl.c"
    "${ASN1C_OUTPUT_DIR}/Spectrum-IQDataIndication.c"
    "${ASN1C_OUTPUT_DIR}/L1KPM-ShmRef.c"
    "${ASN1C_OUTPUT_DIR}/L1KPM-Indication.c"
    "${ASN1C_OUTPUT_DIR}/L1KPM-RanFunctionData.c"
)
if(E3SM_OWN_BOOLEAN)
    list(APPEND ASN1C_SRCS
        "${ASN1C_OUTPUT_DIR}/BOOLEAN.c"
        "${ASN1C_OUTPUT_DIR}/BOOLEAN_aper.c"
        "${ASN1C_OUTPUT_DIR}/BOOLEAN_uper.c"
        "${ASN1C_OUTPUT_DIR}/BOOLEAN_xer.c"
        "${ASN1C_OUTPUT_DIR}/BOOLEAN_print.c"
        "${ASN1C_OUTPUT_DIR}/BOOLEAN_rfill.c"
    )
endif()

# Generate ASN.1 C code at configure time, then remove runtime files
# to avoid conflicts with libe3's runtime.
if(NOT EXISTS "${ASN1C_STAMP}")
    message(STATUS "Running asn1c to generate ASN.1 sources...")
    file(MAKE_DIRECTORY ${ASN1C_OUTPUT_DIR})
    execute_process(
        COMMAND ${ASN1C_EXECUTABLE}
            -D ${ASN1C_OUTPUT_DIR}
            -fcompound-names
            -fincludes-quoted
            -fno-include-deps
            -no-gen-example
            ${ASN1C_CODEC_FLAGS}
            ${ASN1_SOURCES}
        RESULT_VARIABLE ASN1C_RESULT
    )
    if(NOT ASN1C_RESULT EQUAL 0)
        message(FATAL_ERROR "asn1c failed with exit code ${ASN1C_RESULT}")
    endif()
    # Remove all runtime support files — keep only our SM type files
    # (mirrors cleanup_asn1c.cmake; kept inline here for the configure path).
    file(GLOB _all_generated "${ASN1C_OUTPUT_DIR}/*")
    foreach(_file ${_all_generated})
        get_filename_component(_basename "${_file}" NAME)
        if(NOT _basename MATCHES "^Spectrum-"
           AND NOT _basename MATCHES "^L1KPM-"
           AND NOT (E3SM_OWN_BOOLEAN AND _basename MATCHES "^BOOLEAN")
           AND NOT _basename MATCHES "^\\.asn1c_stamp$")
            file(REMOVE "${_file}")
        endif()
    endforeach()
    file(TOUCH ${ASN1C_STAMP})
endif()

# Custom command: regenerate when any .asn source changes
add_custom_command(
    OUTPUT ${ASN1C_STAMP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ASN1C_OUTPUT_DIR}
    COMMAND ${ASN1C_EXECUTABLE}
        -D ${ASN1C_OUTPUT_DIR}
        -fcompound-names
        -fincludes-quoted
        -fno-include-deps
        -no-gen-example
        ${ASN1C_CODEC_FLAGS}
        ${ASN1_SOURCES}
    # Remove runtime files, keep only Spectrum-* and L1KPM-* types (and BOOLEAN)
    COMMAND ${CMAKE_COMMAND} -DASN1C_DIR=${ASN1C_OUTPUT_DIR} -DKEEP_BOOLEAN=${E3SM_OWN_BOOLEAN}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cleanup_asn1c.cmake
    COMMAND ${CMAKE_COMMAND} -E touch ${ASN1C_STAMP}
    # Declared so CMake knows they come from here: a type added to an .asn
    # (a new .c in the list) then regenerates in an existing build dir
    # instead of failing with "Cannot find source file".
    BYPRODUCTS ${ASN1C_SRCS}
    DEPENDS ${ASN1_SOURCES}
    COMMENT "Generating ASN.1 C code from e3sm_spectrum.asn + e3sm_layer1.asn (types only)"
)

add_custom_target(asn1c_generate DEPENDS ${ASN1C_STAMP})

# Static library from generated ASN.1 sources (types only, no runtime)
add_library(e3sm_asn STATIC ${ASN1C_SRCS})
add_dependencies(e3sm_asn asn1c_generate)

set_target_properties(e3sm_asn PROPERTIES C_STANDARD 11)
target_compile_options(e3sm_asn PRIVATE -w)
target_compile_definitions(e3sm_asn PUBLIC ASN_DISABLE_OER_SUPPORT)

# Include the generated directory for our SM type headers,
# and libe3's installed ASN.1 runtime headers for types like OCTET_STRING, etc.
target_include_directories(e3sm_asn PUBLIC
    ${ASN1C_OUTPUT_DIR}
    ${LIBE3_ASN1_INCLUDE_DIR}
)

# Re-configure if any of the ASN sources change
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ASN1_SOURCES})
