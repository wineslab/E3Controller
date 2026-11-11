# cleanup_asn1c.cmake — keep only our SM type files, drop the asn1c runtime.
# Called as a post-generation step to avoid ABI conflicts / duplicate symbols
# with libe3's runtime (which provides the runtime via asn1_e3ap). We keep:
#   Spectrum-*  → RF=1 SM (PRB blacklist controls, legacy in-band IQ)
#   L1KPM-*     → RF=2 SM (SHM-pointer layer-1 indications)
#   BOOLEAN*    → only with -DKEEP_BOOLEAN=ON: libe3 >= 0.0.7 ships no BOOLEAN
#                 runtime, and Spectrum-SensingPolicyControl needs one (see
#                 CMakeLists.txt).

# Pass -DASN1C_DIR=<dir>. In -P script mode CMAKE_BINARY_DIR is the working
# directory, not the build root: a custom command runs in
# <build>/src/e3sm/asn, so the default below would name a directory that does
# not exist, and the cleanup would silently keep the whole asn1c runtime --
# whose headers (asn_config.h among them) would then shadow libe3's.
if(NOT ASN1C_DIR)
    set(ASN1C_DIR "${CMAKE_BINARY_DIR}/asn1c_generated")
endif()
if(NOT EXISTS "${ASN1C_DIR}")
    message(FATAL_ERROR "cleanup_asn1c.cmake: ${ASN1C_DIR} does not exist")
endif()

file(GLOB _all_files "${ASN1C_DIR}/*")
foreach(_file ${_all_files})
    get_filename_component(_basename "${_file}" NAME)
    if(NOT _basename MATCHES "^Spectrum-"
       AND NOT _basename MATCHES "^L1KPM-"
       AND NOT (KEEP_BOOLEAN AND _basename MATCHES "^BOOLEAN")
       AND NOT _basename MATCHES "^\\.asn1c_stamp$")
        file(REMOVE "${_file}")
    endif()
endforeach()
