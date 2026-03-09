# ASN.1 code generation using asn1c (requires mouse07410/asn1c fork with APER support)
# Assumes asn1c is already installed system-wide.
# Generates C sources from e3sm_spectrum.asn at configure time, compiles into static library.
#
# IMPORTANT: Only the Spectrum type-specific files are compiled here.
# The asn1c runtime (aper_encoder.c, constr_SEQUENCE.c, etc.) is provided
# by the installed libe3 library (via asn1_e3ap). We MUST NOT compile our
# own copy of the runtime, because different asn1c versions produce
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
set(ASN1_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/e3sm_spectrum.asn")
set(ASN1C_STAMP "${ASN1C_OUTPUT_DIR}/.asn1c_stamp")

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
            -no-gen-OER
            ${ASN1_SOURCE}
        RESULT_VARIABLE ASN1C_RESULT
    )
    if(NOT ASN1C_RESULT EQUAL 0)
        message(FATAL_ERROR "asn1c failed with exit code ${ASN1C_RESULT}")
    endif()
    # Remove all runtime support files — keep only Spectrum-* type files
    file(GLOB _all_generated "${ASN1C_OUTPUT_DIR}/*")
    foreach(_file ${_all_generated})
        get_filename_component(_basename "${_file}" NAME)
        if(NOT _basename MATCHES "^Spectrum-" AND NOT _basename MATCHES "^\\.asn1c_stamp$")
            file(REMOVE "${_file}")
        endif()
    endforeach()
    file(TOUCH ${ASN1C_STAMP})
endif()

# Custom command: regenerate when ASN file changes
add_custom_command(
    OUTPUT ${ASN1C_STAMP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ASN1C_OUTPUT_DIR}
    COMMAND ${ASN1C_EXECUTABLE}
        -D ${ASN1C_OUTPUT_DIR}
        -fcompound-names
        -fincludes-quoted
        -fno-include-deps
        -no-gen-example
        -no-gen-OER
        ${ASN1_SOURCE}
    # Remove runtime files, keep only Spectrum-* types
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cleanup_asn1c.cmake
    COMMAND ${CMAKE_COMMAND} -E touch ${ASN1C_STAMP}
    DEPENDS ${ASN1_SOURCE}
    COMMENT "Generating ASN.1 C code from e3sm_spectrum.asn (types only)"
)

add_custom_target(asn1c_generate DEPENDS ${ASN1C_STAMP})

# Only the Spectrum SM type-specific generated files
set(ASN1C_SRCS
    "${ASN1C_OUTPUT_DIR}/Spectrum-IQDataIndication.c"
    "${ASN1C_OUTPUT_DIR}/Spectrum-PRBBlacklistControl.c"
    "${ASN1C_OUTPUT_DIR}/Spectrum-RanFunctionData.c"
    "${ASN1C_OUTPUT_DIR}/Spectrum-ConfigControl.c"
)

# Static library from generated ASN.1 sources (types only, no runtime)
add_library(e3sm_asn STATIC ${ASN1C_SRCS})
add_dependencies(e3sm_asn asn1c_generate)

set_target_properties(e3sm_asn PROPERTIES C_STANDARD 11)
target_compile_options(e3sm_asn PRIVATE -w)
target_compile_definitions(e3sm_asn PUBLIC ASN_DISABLE_OER_SUPPORT)

# Include the generated directory for the Spectrum type headers,
# and libe3's installed ASN.1 runtime headers for types like OCTET_STRING, etc.
target_include_directories(e3sm_asn PUBLIC
    ${ASN1C_OUTPUT_DIR}
    ${LIBE3_ASN1_INCLUDE_DIR}
)

# Re-configure if the ASN source changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ASN1_SOURCE})
