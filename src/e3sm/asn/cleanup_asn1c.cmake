# cleanup_asn1c.cmake — keep only our SM type files, drop the asn1c runtime.
# Called as a post-generation step to avoid ABI conflicts / duplicate symbols
# with libe3's runtime (which provides the runtime via asn1_e3ap). We keep:
#   Spectrum-*  → RF=1 SM (in-band IQ + PRB blacklist controls)
#   L1KPM-*     → RF=2 SM (SHM-pointer layer-1 indications)
# BOOLEAN* is intentionally NOT kept: the only type that used it,
# Spectrum-ConfigControl, is generated but no longer compiled (see
# CMakeLists.txt), and libe3 >= 0.0.7 does not ship a BOOLEAN runtime either.

set(ASN1C_DIR "${CMAKE_BINARY_DIR}/asn1c_generated")
if(NOT EXISTS "${ASN1C_DIR}")
    return()
endif()

file(GLOB _all_files "${ASN1C_DIR}/*")
foreach(_file ${_all_files})
    get_filename_component(_basename "${_file}" NAME)
    if(NOT _basename MATCHES "^Spectrum-"
       AND NOT _basename MATCHES "^L1KPM-"
       AND NOT _basename MATCHES "^\\.asn1c_stamp$")
        file(REMOVE "${_file}")
    endif()
endforeach()
