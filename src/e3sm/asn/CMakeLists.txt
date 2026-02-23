# ASN.1 code generation using asn1c (mouse07410 fork with APER support)
# Builds asn1c from source, then generates C sources from e3sm_spectrum.asn

include(ExternalProject)

set(ASN1C_INSTALL_DIR "${CMAKE_BINARY_DIR}/asn1c_install")
set(ASN1C_EXECUTABLE "${ASN1C_INSTALL_DIR}/bin/asn1c")
set(ASN1C_OUTPUT_DIR "${CMAKE_BINARY_DIR}/asn1c_generated")
set(ASN1_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/e3sm_spectrum.asn")
set(ASN1C_STAMP "${ASN1C_OUTPUT_DIR}/.asn1c_stamp")

# ---- Build asn1c from mouse07410 fork (APER support) ----
ExternalProject_Add(asn1c_build
    GIT_REPOSITORY https://github.com/mouse07410/asn1c.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    PREFIX "${CMAKE_BINARY_DIR}/asn1c_ext"
    CONFIGURE_COMMAND autoreconf -iv <SOURCE_DIR>
        COMMAND <SOURCE_DIR>/configure --prefix=${ASN1C_INSTALL_DIR}
    BUILD_COMMAND make -j4
    INSTALL_COMMAND make install
    BUILD_IN_SOURCE FALSE
    LOG_DOWNLOAD TRUE
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_INSTALL TRUE
)

# ---- Generate ASN.1 C code ----
# At configure time: if asn1c is already built, generate immediately
if(EXISTS "${ASN1C_EXECUTABLE}" AND NOT EXISTS "${ASN1C_STAMP}")
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

# Custom command: regenerate when ASN file changes (after initial build)
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
add_dependencies(asn1c_generate asn1c_build)

# ---- Collect and compile generated sources ----
file(GLOB ASN1C_SRCS "${ASN1C_OUTPUT_DIR}/*.c")

# On first build, sources don't exist yet. We add a placeholder that CMake
# can handle, then the real sources appear after asn1c_build + asn1c_generate.
if(NOT ASN1C_SRCS)
    # Create a minimal placeholder so the library target can be defined.
    # It will be replaced by real sources on the second cmake run.
    file(MAKE_DIRECTORY ${ASN1C_OUTPUT_DIR})
    file(WRITE "${ASN1C_OUTPUT_DIR}/_placeholder.c" "/* placeholder - will be replaced by asn1c output */\n")
    set(ASN1C_SRCS "${ASN1C_OUTPUT_DIR}/_placeholder.c")
endif()

add_library(e3sm_asn STATIC ${ASN1C_SRCS})
add_dependencies(e3sm_asn asn1c_generate)

# Suppress warnings in generated code
set_target_properties(e3sm_asn PROPERTIES C_STANDARD 11)
target_compile_options(e3sm_asn PRIVATE -w)
target_compile_definitions(e3sm_asn PUBLIC ASN_DISABLE_OER_SUPPORT)

target_include_directories(e3sm_asn PUBLIC ${ASN1C_OUTPUT_DIR})

# Re-configure if the ASN source changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ASN1_SOURCE})
