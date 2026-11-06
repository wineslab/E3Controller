/*
 * Codecs for the Spectrum Service Model. APER through the asn1c-generated
 * types; JSON through nlohman, in the forms OAI's Spectrum SM uses.
 */

#include "e3sm_spect_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "Spectrum-IQDataIndication.h"
#include "Spectrum-DAppControlData.h"
#include "Spectrum-RanFunctionData.h"
#include "aper_encoder.h"
#include "aper_decoder.h"

#ifdef __cplusplus
}
#endif

#include <nlohmann/json.hpp>

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

bool encode_spectrum_ran_function_data_json(std::vector<uint8_t>& out)
{
    // ordered_json keeps the field order of the grammar; plain json would sort
    // the keys.
    nlohmann::ordered_json rfd;
    rfd["name"]        = SPECTRUM_RAN_FUNCTION_NAME;
    rfd["version"]     = SPECTRUM_RAN_FUNCTION_VERSION;
    rfd["description"] = SPECTRUM_RAN_FUNCTION_DESCRIPTION;

    const std::string text = rfd.dump();
    out.assign(text.begin(), text.end());
    return true;
}

// ---------------------------------------------------------------------------
// dApp control, APER
// ---------------------------------------------------------------------------

bool encode_spectrum_dapp_control_aper(const SpectrumPRBBlacklistControl& in, std::vector<uint8_t>& out)
{
    Spectrum_DAppControlData_t envelope;
    memset(&envelope, 0, sizeof(envelope));
    envelope.controlPayload.present = Spectrum_DAppControlPayload_PR_prbBlacklistControl;
    Spectrum_PRBBlacklistControl_t* control = &envelope.controlPayload.choice.prbBlacklistControl;

    bool ok = true;

    // blacklistedPRBs: SEQUENCE OF INTEGER. Every element is its own allocation.
    for (int prb : in.blacklisted_prbs) {
        long* element = (long*)calloc(1, sizeof(long));
        if (element == nullptr) {
            ok = false;
            break;
        }
        *element = prb;
        if (ASN_SEQUENCE_ADD(&control->blacklistedPRBs.list, element) != 0) {
            free(element);
            ok = false;
            break;
        }
    }

    // The two optional fields are present only when set.
    if (ok && in.sampling_threshold.has_value()) {
        control->samplingThreshold = (long*)malloc(sizeof(long));
        if (control->samplingThreshold == nullptr) {
            ok = false;
        } else {
            *control->samplingThreshold = in.sampling_threshold.value();
        }
    }
    if (ok && in.validity_period.has_value()) {
        control->validityPeriod = (long*)malloc(sizeof(long));
        if (control->validityPeriod == nullptr) {
            ok = false;
        } else {
            *control->validityPeriod = in.validity_period.value();
        }
    }

    if (ok) {
        uint8_t buffer[4096];
        asn_enc_rval_t ret = aper_encode_to_buffer(
            &asn_DEF_Spectrum_DAppControlData, NULL, &envelope, buffer, sizeof(buffer));
        if (ret.encoded == -1) {
            ok = false;
        } else {
            size_t bytes = (ret.encoded + 7) / 8;
            out.assign(buffer, buffer + bytes);
        }
    }

    // Frees what the fields point to; the envelope itself is on the stack.
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_Spectrum_DAppControlData, &envelope);
    return ok;
}

DecodeResult decode_spectrum_dapp_control_aper(const std::vector<uint8_t>& in,
                                               SpectrumPRBBlacklistControl& out,
                                               std::string& why)
{
    Spectrum_DAppControlData_t* envelope = nullptr;
    asn_dec_rval_t ret = aper_decode(NULL,
        &asn_DEF_Spectrum_DAppControlData,
        (void**)&envelope,
        in.data(),
        in.size(),
        0, 0);

    if (ret.code != RC_OK || envelope == nullptr) {
        if (envelope != nullptr) {
            ASN_STRUCT_FREE(asn_DEF_Spectrum_DAppControlData, envelope);
        }
        why = "not an APER Spectrum-DAppControlData";
        return DecodeResult::malformed;
    }

    // A valid control of another kind, such as a sensing policy.
    if (envelope->controlPayload.present != Spectrum_DAppControlPayload_PR_prbBlacklistControl) {
        why = "the control is not a prbBlacklistControl (CHOICE alternative " +
              std::to_string(envelope->controlPayload.present) + ")";
        ASN_STRUCT_FREE(asn_DEF_Spectrum_DAppControlData, envelope);
        return DecodeResult::unsupported;
    }

    // PER cannot carry a value outside the grammar's constraints, so the values
    // need no range check here.
    const Spectrum_PRBBlacklistControl_t* control = &envelope->controlPayload.choice.prbBlacklistControl;

    out = SpectrumPRBBlacklistControl{};
    for (int i = 0; i < control->blacklistedPRBs.list.count; i++) {
        out.blacklisted_prbs.push_back(static_cast<int>(*control->blacklistedPRBs.list.array[i]));
    }
    if (control->samplingThreshold != nullptr) {
        out.sampling_threshold = static_cast<int>(*control->samplingThreshold);
    }
    if (control->validityPeriod != nullptr) {
        out.validity_period = static_cast<int>(*control->validityPeriod);
    }

    ASN_STRUCT_FREE(asn_DEF_Spectrum_DAppControlData, envelope);
    return DecodeResult::ok;
}

// ---------------------------------------------------------------------------
// dApp control, JSON
//
// The form OAI's gNB accepts (spectrum_dec.c):
//   {"controlType": "prbBlacklist",
//    "controlPayload": {"prbBlacklistControl": {"blacklistedPRBs": [5, 6],
//                                               "samplingThreshold": 10,
//                                               "validityPeriod": 60}}}
// ---------------------------------------------------------------------------

// The other alternatives of Spectrum-DAppControlPayload. OAI's gNB handles them;
// this controller recognises them only to say so.
static bool is_other_control_type(const std::string& type)
{
    return type == "sensingPolicy" || type == "sensingReservation" || type == "sensingPuschSlots";
}

static bool has_other_control_payload(const nlohmann::json& payload)
{
    return payload.contains("sensingPolicyControl") || payload.contains("sensingReservationControl") ||
           payload.contains("sensingPuschSlotsControl");
}

// Reads an optional integer field and checks it against the grammar's range.
// Returns false, and says why, when the field is there but not valid.
static bool read_optional_int(const nlohmann::json& object,
                              const char*           key,
                              int                   min,
                              int                   max,
                              std::optional<int>&   value,
                              std::string&          why)
{
    if (!object.contains(key)) {
        return true;  // absent is fine
    }

    const nlohmann::json& field = object[key];
    if (!field.is_number_integer()) {
        why = std::string(key) + " is not an integer";
        return false;
    }

    const int64_t number = field.get<int64_t>();
    if (number < min || number > max) {
        why = std::string(key) + " = " + std::to_string(number) + " is outside " +
              std::to_string(min) + ".." + std::to_string(max);
        return false;
    }

    value = static_cast<int>(number);
    return true;
}

DecodeResult decode_spectrum_dapp_control_json(const std::vector<uint8_t>& in,
                                               SpectrumPRBBlacklistControl& out,
                                               std::string& why)
{
    // 1. Parse. With allow_exceptions=false a parse error gives a "discarded"
    //    value instead of throwing.
    const nlohmann::json root = nlohmann::json::parse(in.begin(), in.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        why = "not a JSON object";
        return DecodeResult::malformed;
    }

    // 2. "controlType". It is a convenience key added by the codecs, not a field
    //    of the grammar, so it may be absent. When present it must name a PRB
    //    blacklist.
    if (root.contains("controlType")) {
        const nlohmann::json& control_type = root["controlType"];
        if (!control_type.is_string()) {
            why = "controlType is not a string";
            return DecodeResult::malformed;
        }
        const std::string type = control_type.get<std::string>();
        if (is_other_control_type(type)) {
            why = "controlType " + type + " is not handled by this controller";
            return DecodeResult::unsupported;
        }
        if (type != "prbBlacklist") {
            why = "unknown controlType " + type;
            return DecodeResult::malformed;
        }
    }

    // 3. "controlPayload", which holds the alternative. Its key is the real
    //    discriminator.
    if (!root.contains("controlPayload") || !root["controlPayload"].is_object()) {
        why = "no controlPayload object";
        return DecodeResult::malformed;
    }
    const nlohmann::json& payload = root["controlPayload"];

    if (!payload.contains("prbBlacklistControl")) {
        if (has_other_control_payload(payload)) {
            why = "the control is not a prbBlacklistControl";
            return DecodeResult::unsupported;
        }
        why = "no prbBlacklistControl in controlPayload";
        return DecodeResult::malformed;
    }
    const nlohmann::json& control = payload["prbBlacklistControl"];
    if (!control.is_object()) {
        why = "prbBlacklistControl is not an object";
        return DecodeResult::malformed;
    }

    // 4. "blacklistedPRBs" is required. An empty list is a valid control that
    //    clears the blacklist; a missing list is an error, never a silent clear.
    if (!control.contains("blacklistedPRBs") || !control["blacklistedPRBs"].is_array()) {
        why = "no blacklistedPRBs array";
        return DecodeResult::malformed;
    }
    const nlohmann::json& prbs = control["blacklistedPRBs"];
    if (prbs.size() > kMaxPrbs) {
        why = "blacklistedPRBs has " + std::to_string(prbs.size()) + " entries, the most is " +
              std::to_string(kMaxPrbs);
        return DecodeResult::malformed;
    }

    SpectrumPRBBlacklistControl result;
    for (const nlohmann::json& prb : prbs) {
        if (!prb.is_number_integer()) {
            why = "blacklistedPRBs entry " + prb.dump() + " is not an integer";
            return DecodeResult::malformed;
        }
        const int64_t index = prb.get<int64_t>();
        if (index < 0 || index > kMaxPrbIndex) {
            why = "PRB index " + std::to_string(index) + " is outside 0.." + std::to_string(kMaxPrbIndex);
            return DecodeResult::malformed;
        }
        result.blacklisted_prbs.push_back(static_cast<int>(index));
    }

    // 5. The optional fields, held to the grammar's ranges. OAI's decoder takes
    //    any integer here; an APER control could not carry one outside them.
    if (!read_optional_int(control, "samplingThreshold", 0, 100, result.sampling_threshold, why)) {
        return DecodeResult::malformed;
    }
    if (!read_optional_int(control, "validityPeriod", 1, 3600, result.validity_period, why)) {
        return DecodeResult::malformed;
    }

    out = result;
    return DecodeResult::ok;
}

} // namespace e3sm_spectrum
