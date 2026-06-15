/*
 * APER encoder implementation for L1-KPM Indication.
 *
 * Payload schema derived from NVIDIA Aerial's public E3 message schema
 * (see e3sm/asn/e3sm_layer1.asn for attribution).
 *
 * Mirrors e3sm_spect_wrapper.cpp's encode_spectrum_iq_indication() style: use
 * the asn1c-generated C struct, fill it with calloc'd OPTIONAL pointers,
 * call aper_encode_to_buffer, then free the allocations. The asn1c runtime
 * itself lives in libe3 (asn1_e3ap) — we only depend on the L1KPM-* type
 * files generated under build/asn1c_generated/.
 */

#include "e3sm_layer1_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "L1KPM-ShmRef.h"
#include "L1KPM-Indication.h"
#include "Spectrum-RanFunctionData.h"
#include "aper_encoder.h"

#ifdef __cplusplus
}
#endif

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>

namespace e3sm_layer1 {

bool encode_iq_indication_aper(uint64_t timestamp_ns,
                               uint16_t sfn,
                               uint16_t slot,
                               const std::string& shm_name,
                               uint8_t fh_buffer_index,
                               uint32_t fh_write_index,
                               uint16_t nof_ports,
                               std::vector<uint8_t>& out)
{
    L1KPM_Indication_t ind;
    std::memset(&ind, 0, sizeof(ind));

    // iqSamplesRef is OPTIONAL on the wire but we always emit it — the dApp
    // expects the shm coordinates on every indication.
    ind.iqSamplesRef = static_cast<L1KPM_ShmRef_t*>(std::calloc(1, sizeof(L1KPM_ShmRef_t)));
    if (!ind.iqSamplesRef) return false;

    ind.iqSamplesRef->shmName.buf = static_cast<uint8_t*>(std::malloc(shm_name.size()));
    if (!ind.iqSamplesRef->shmName.buf) {
        ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_L1KPM_Indication, &ind);
        return false;
    }
    std::memcpy(ind.iqSamplesRef->shmName.buf, shm_name.data(), shm_name.size());
    ind.iqSamplesRef->shmName.size = static_cast<int>(shm_name.size());

    ind.iqSamplesRef->fhBufferIndex = static_cast<long>(fh_buffer_index);
    ind.iqSamplesRef->fhWriteIndex  = static_cast<long>(fh_write_index);

    // asn1c maps unconstrained INTEGER to `long` here (not INTEGER_t), so we
    // just assign. uint64 ns timestamps from jbpf_time_get_ns() fit in
    // signed long on 64-bit Linux (long is 8 bytes); on a hypothetical
    // 32-bit build the high bits would silently truncate.
    ind.timestamp = static_cast<long>(timestamp_ns);

    ind.sfn  = static_cast<long>(sfn);
    ind.slot = static_cast<long>(slot);

    // cellId left OPTIONAL-absent. nRxAnt carries the antenna count so a dApp
    // knows how many antennas the shm row holds. asn1c maps OPTIONAL INTEGER
    // to long*; the calloc'd member is freed by ASN_STRUCT_FREE_CONTENTS_ONLY.
    if (nof_ports > 0) {
        ind.nRxAnt = static_cast<long*>(std::calloc(1, sizeof(long)));
        if (ind.nRxAnt) *ind.nRxAnt = static_cast<long>(nof_ports);
    }

    // Buffer sizing: payload is small (≈80 bytes including shm_name); 256 is
    // comfortable headroom and matches the existing Spectrum encoders.
    uint8_t buffer[256];
    asn_enc_rval_t ret = aper_encode_to_buffer(
        &asn_DEF_L1KPM_Indication, nullptr, &ind, buffer, sizeof(buffer));

    bool ok = (ret.encoded != -1);
    if (ok) {
        const size_t bytes = (ret.encoded + 7) / 8;
        out.assign(buffer, buffer + bytes);
    }

    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_L1KPM_Indication, &ind);
    return ok;
}

bool encode_ran_function_data_aper(std::vector<uint8_t>& out)
{
    // Reuses Spectrum-RanFunctionData on the wire — same {name, version,
    // description} SEQUENCE — so the dApp decodes both SMs' RFD with one
    // schema, but with layer1's own strings.
    Spectrum_RanFunctionData_t rfd;
    std::memset(&rfd, 0, sizeof(rfd));

    const char* name = LAYER1_RAN_FUNCTION_NAME;
    rfd.name.buf = static_cast<uint8_t*>(std::malloc(std::strlen(name)));
    if (!rfd.name.buf) return false;
    std::memcpy(rfd.name.buf, name, std::strlen(name));
    rfd.name.size = static_cast<int>(std::strlen(name));

    rfd.version = LAYER1_RAN_FUNCTION_VERSION;

    const char* desc = LAYER1_RAN_FUNCTION_DESCRIPTION;
    rfd.description.buf = static_cast<uint8_t*>(std::malloc(std::strlen(desc)));
    if (!rfd.description.buf) {
        std::free(rfd.name.buf);
        return false;
    }
    std::memcpy(rfd.description.buf, desc, std::strlen(desc));
    rfd.description.size = static_cast<int>(std::strlen(desc));

    uint8_t buffer[256];
    asn_enc_rval_t ret = aper_encode_to_buffer(
        &asn_DEF_Spectrum_RanFunctionData, nullptr, &rfd, buffer, sizeof(buffer));

    bool ok = (ret.encoded != -1);
    if (ok) {
        const size_t bytes = (ret.encoded + 7) / 8;
        out.assign(buffer, buffer + bytes);
    }

    std::free(rfd.name.buf);
    std::free(rfd.description.buf);
    return ok;
}

bool encode_ran_function_data_json(std::vector<uint8_t>& out)
{
    nlohmann::json j = {
        {"name",        LAYER1_RAN_FUNCTION_NAME},
        {"version",     LAYER1_RAN_FUNCTION_VERSION},
        {"description", LAYER1_RAN_FUNCTION_DESCRIPTION},
    };
    const std::string s = j.dump();
    out.assign(s.begin(), s.end());
    return true;
}

}  // namespace e3sm_layer1
