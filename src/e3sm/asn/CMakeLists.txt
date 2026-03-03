# ASN.1 code generation using asn1c (requires mouse07410/asn1c fork with APER support)
# Assumes asn1c is already installed system-wide.
# Generates C sources from e3sm_spectrum.asn at configure time, compiles into static library.

find_program(ASN1C_EXECUTABLE asn1c REQUIRED)

set(ASN1C_OUTPUT_DIR "${CMAKE_BINARY_DIR}/asn1c_generated")
set(ASN1_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/e3sm_spectrum.asn")
set(ASN1C_STAMP "${ASN1C_OUTPUT_DIR}/.asn1c_stamp")

# Generate ASN.1 C code at configure time
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
    COMMAND ${CMAKE_COMMAND} -E touch ${ASN1C_STAMP}
    DEPENDS ${ASN1_SOURCE}
    COMMENT "Generating ASN.1 C code from e3sm_spectrum.asn (APER enabled)"
)

add_custom_target(asn1c_generate DEPENDS ${ASN1C_STAMP})

# Collect generated .c files
file(GLOB ASN1C_SRCS "${ASN1C_OUTPUT_DIR}/*.c")

# Static library from generated ASN.1 sources
add_library(e3sm_asn STATIC ${ASN1C_SRCS})
add_dependencies(e3sm_asn asn1c_generate)

set_target_properties(e3sm_asn PROPERTIES C_STANDARD 11)
target_compile_options(e3sm_asn PRIVATE -w)
target_compile_definitions(e3sm_asn PUBLIC ASN_DISABLE_OER_SUPPORT)
target_include_directories(e3sm_asn PUBLIC ${ASN1C_OUTPUT_DIR})

# Re-configure if the ASN source changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ASN1_SOURCE})
