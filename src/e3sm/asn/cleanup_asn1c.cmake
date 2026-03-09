# cleanup_asn1c.cmake — Remove asn1c runtime files, keep only Spectrum-* types
# Called as a post-generation step to avoid ABI conflicts with libe3's runtime.

set(ASN1C_DIR "${CMAKE_BINARY_DIR}/asn1c_generated")
if(NOT EXISTS "${ASN1C_DIR}")
    return()
endif()

file(GLOB _all_files "${ASN1C_DIR}/*")
foreach(_file ${_all_files})
    get_filename_component(_basename "${_file}" NAME)
    if(NOT _basename MATCHES "^Spectrum-" AND NOT _basename MATCHES "^\\.asn1c_stamp$")
        file(REMOVE "${_file}")
    endif()
endforeach()
