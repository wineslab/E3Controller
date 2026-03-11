/*
 * Wrapper helpers implementing APER encoding for Spectrum Service Model
 * ASN.1 types. These use asn1c-generated C types and the APER encoder.
 */

#include "e3sm_spect_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "Spectrum-IQDataIndication.h"
#include "Spectrum-PRBBlacklistControl.h"
#include "Spectrum-ConfigControl.h"
#include "Spectrum-RanFunctionData.h"
#include "aper_encoder.h"
#include "aper_decoder.h"

#ifdef __cplusplus
}
#endif

#include <cstring>
#include <cstdlib>

namespace e3sm_spectrum {

bool encode_spectrum_iq_indication(const SpectrumIQIndication& in, std::vector<uint8_t>& out) {
    Spectrum_IQDataIndication_t si;
    memset(&si, 0, sizeof(si));

    // Set iqSamples (OCTET STRING)
    si.iqSamples.buf = (uint8_t *)malloc(in.iq_data.size());
    if (!si.iqSamples.buf) return false;
    memcpy(si.iqSamples.buf, in.iq_data.data(), in.iq_data.size());
    si.iqSamples.size = in.iq_data.size();

    // Set sampleCount
    si.sampleCount = in.sample_count;

    // Set timestamp
    si.timestamp = (long *)malloc(sizeof(long));
    if (!si.timestamp) {
        free(si.iqSamples.buf);
        return false;
    }
    *si.timestamp = in.timestamp;

    // APER encode — buffer must hold FFT-padded payload (up to 16384 bytes IQ + overhead)
    uint8_t buffer[20000];
    asn_enc_rval_t ret = aper_encode_to_buffer(
        &asn_DEF_Spectrum_IQDataIndication, NULL, &si, buffer, sizeof(buffer));

    bool ok = (ret.encoded != -1);
    if (ok) {
        size_t bytes = (ret.encoded + 7) / 8;
        out.assign(buffer, buffer + bytes);
    }

    // Cleanup
    free(si.iqSamples.buf);
    free(si.timestamp);
    return ok;
}

bool encode_spectrum_prb_blacklist_control(const SpectrumPRBBlacklistControl& in, std::vector<uint8_t>& out) {
    Spectrum_PRBBlacklistControl_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    // Set blacklistedPRBs (SEQUENCE OF INTEGER)
    for (int prb : in.blacklisted_prbs) {
        long *val = (long *)calloc(1, sizeof(long));
        if (!val) {
            ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_Spectrum_PRBBlacklistControl, &ctrl);
            return false;
        }
        *val = prb;
        if (ASN_SEQUENCE_ADD(&ctrl.blacklistedPRBs.list, val) != 0) {
            free(val);
            ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_Spectrum_PRBBlacklistControl, &ctrl);
            return false;
        }
    }

    // Set samplingThreshold
    ctrl.samplingThreshold = (long *)malloc(sizeof(long));
    if (!ctrl.samplingThreshold) {
        ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_Spectrum_PRBBlacklistControl, &ctrl);
        return false;
    }
    *ctrl.samplingThreshold = in.sampling_threshold;

    // Set validityPeriod
    ctrl.validityPeriod = (long *)malloc(sizeof(long));
    if (!ctrl.validityPeriod) {
        ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_Spectrum_PRBBlacklistControl, &ctrl);
        return false;
    }
    *ctrl.validityPeriod = in.validity_period;

    uint8_t buffer[4096];
    asn_enc_rval_t ret = aper_encode_to_buffer(
        &asn_DEF_Spectrum_PRBBlacklistControl, NULL, &ctrl, buffer, sizeof(buffer));

    bool ok = (ret.encoded != -1);
    if (ok) {
        size_t bytes = (ret.encoded + 7) / 8;
        out.assign(buffer, buffer + bytes);
    }

    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_Spectrum_PRBBlacklistControl, &ctrl);
    return ok;
}

bool encode_spectrum_config_control(const SpectrumConfigControl& in, std::vector<uint8_t>& out) {
    Spectrum_ConfigControl_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    // Set noiseFloorThreshold
    cfg.noiseFloorThreshold = (long *)malloc(sizeof(long));
    if (!cfg.noiseFloorThreshold) return false;
    *cfg.noiseFloorThreshold = in.noise_floor_threshold;

    // Set averagingFrames
    cfg.averagingFrames = (long *)malloc(sizeof(long));
    if (!cfg.averagingFrames) {
        free(cfg.noiseFloorThreshold);
        return false;
    }
    *cfg.averagingFrames = in.averaging_frames;

    // Set enable
    cfg.enable = (BOOLEAN_t *)malloc(sizeof(BOOLEAN_t));
    if (!cfg.enable) {
        free(cfg.noiseFloorThreshold);
        free(cfg.averagingFrames);
        return false;
    }
    *cfg.enable = in.enable ? 1 : 0;

    uint8_t buffer[256];
    asn_enc_rval_t ret = aper_encode_to_buffer(
        &asn_DEF_Spectrum_ConfigControl, NULL, &cfg, buffer, sizeof(buffer));

    bool ok = (ret.encoded != -1);
    if (ok) {
        size_t bytes = (ret.encoded + 7) / 8;
        out.assign(buffer, buffer + bytes);
    }

    free(cfg.noiseFloorThreshold);
    free(cfg.averagingFrames);
    free(cfg.enable);
    return ok;
}

bool encode_spectrum_ran_function_data(std::vector<uint8_t>& out)
{
    Spectrum_RanFunctionData_t rfd;
    memset(&rfd, 0, sizeof(rfd));

    // Set name from macro
    const char* name = SPECTRUM_RAN_FUNCTION_NAME;
    rfd.name.buf = (uint8_t *)malloc(strlen(name));
    if (!rfd.name.buf) return false;
    memcpy(rfd.name.buf, name, strlen(name));
    rfd.name.size = strlen(name);

    // Set version from macro
    rfd.version = SPECTRUM_RAN_FUNCTION_VERSION;

    // Set description from macro
    const char* desc = SPECTRUM_RAN_FUNCTION_DESCRIPTION;
    rfd.description.buf = (uint8_t *)malloc(strlen(desc));
    if (!rfd.description.buf) {
        free(rfd.name.buf);
        return false;
    }
    memcpy(rfd.description.buf, desc, strlen(desc));
    rfd.description.size = strlen(desc);

    uint8_t buffer[256];
    asn_enc_rval_t ret = aper_encode_to_buffer(
        &asn_DEF_Spectrum_RanFunctionData, NULL, &rfd, buffer, sizeof(buffer));

    bool ok = (ret.encoded != -1);
    if (ok) {
        size_t bytes = (ret.encoded + 7) / 8;
        out.assign(buffer, buffer + bytes);
    }

    free(rfd.name.buf);
    free(rfd.description.buf);
    return ok;
}

bool decode_spectrum_prb_blacklist_control(const std::vector<uint8_t>& in, SpectrumPRBBlacklistControl& out)
{
    Spectrum_PRBBlacklistControl_t *ctrl = nullptr;
    asn_dec_rval_t ret = aper_decode(NULL,
        &asn_DEF_Spectrum_PRBBlacklistControl,
        (void **)&ctrl,
        in.data(),
        in.size(),
        0, 0);

    if (ret.code != RC_OK || !ctrl) {
        if (ctrl) {
            ASN_STRUCT_FREE(asn_DEF_Spectrum_PRBBlacklistControl, ctrl);
        }
        return false;
    }

    // Extract blacklistedPRBs
    out.blacklisted_prbs.clear();
    for (int i = 0; i < ctrl->blacklistedPRBs.list.count; i++) {
        out.blacklisted_prbs.push_back(static_cast<int>(*ctrl->blacklistedPRBs.list.array[i]));
    }

    // Extract optional samplingThreshold
    if (ctrl->samplingThreshold) {
        out.sampling_threshold = static_cast<int>(*ctrl->samplingThreshold);
    } else {
        out.sampling_threshold = 0;
    }

    // Extract optional validityPeriod
    if (ctrl->validityPeriod) {
        out.validity_period = static_cast<int>(*ctrl->validityPeriod);
    } else {
        out.validity_period = 0;
    }

    ASN_STRUCT_FREE(asn_DEF_Spectrum_PRBBlacklistControl, ctrl);
    return true;
}

} // namespace e3sm_spectrum