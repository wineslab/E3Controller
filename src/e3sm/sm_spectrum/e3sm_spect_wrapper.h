/*
 * Codecs for the Spectrum Service Model (RAN function 1).
 *
 * The grammar is OAI's (src/e3sm/asn/e3sm_spectrum.asn: the Spectrum SM of OAI's
 * E2SM-DAPP bridge, plus one legacy type). Each "*Data" envelope there is a
 * single-member SEQUENCE around a Payload CHOICE, and its JSON twin adds an
 * optional type key ("controlType") next to the payload; see the grammar header.
 *
 * What the controller encodes and decodes itself:
 *   - Spectrum-RanFunctionData, in the agent's encoding (APER or JSON).
 *   - Spectrum-DAppControlData from a dApp, in the agent's encoding. Only the
 *     prbBlacklistControl alternative is handled.
 *   - Spectrum-IQDataIndication (APER), the legacy eCPRI IQ path.
 * dApp reports and xApp controls are relayed without being decoded.
 */
#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <string>

#define SPECTRUM_RAN_FUNCTION_NAME "spectrum_sm"
#define SPECTRUM_RAN_FUNCTION_DESCRIPTION "Spectrum service model for IQ indication and PRB blacklist control"
#define SPECTRUM_RAN_FUNCTION_VERSION 1

namespace e3sm_spectrum {

// Bounds of the grammar's PRB lists (maxPRBs, maxPRBIndex).
constexpr std::size_t kMaxPrbs     = 273;
constexpr int         kMaxPrbIndex = 272;

struct SpectrumIQIndication {
    std::vector<uint8_t> iq_data;       // Raw I/Q sample bytes (OCTET STRING)
    uint32_t sample_count;              // Number of I/Q samples
    uint32_t timestamp;  // Unix timestamp (optional)
};

// Spectrum-PRBBlacklistControl. An empty list is a valid control: it clears the
// blacklist.
struct SpectrumPRBBlacklistControl {
    std::vector<int>   blacklisted_prbs;     // PRB indices, 0..272
    std::optional<int> sampling_threshold;   // 0..100
    std::optional<int> validity_period;      // seconds, 1..3600
};

enum class DecodeResult {
    ok,
    malformed,     // not a Spectrum-DAppControlData, or out of its constraints
    unsupported,   // a valid control of a kind this controller does not handle
};

// Encode Spectrum-IQDataIndication into APER bytes
bool encode_spectrum_iq_indication(const SpectrumIQIndication& in, std::vector<uint8_t>& out);

// Encode Spectrum-RanFunctionData: APER, or the JSON object
// {"name","version","description"} (the field order of the grammar).
bool encode_spectrum_ran_function_data(std::vector<uint8_t>& out);
bool encode_spectrum_ran_function_data_json(std::vector<uint8_t>& out);

// Encode a Spectrum-DAppControlData carrying prbBlacklistControl, APER. The
// controller receives controls rather than sending them; this is the inverse of
// decode_spectrum_dapp_control_aper, for tests.
bool encode_spectrum_dapp_control_aper(const SpectrumPRBBlacklistControl& in, std::vector<uint8_t>& out);

// Decode a dApp's Spectrum-DAppControlData. `why` says what was wrong when the
// result is not ok.
//
// APER: the envelope, whose CHOICE must hold prbBlacklistControl.
// JSON: {"controlType":"prbBlacklist","controlPayload":{"prbBlacklistControl":
//        {"blacklistedPRBs":[...],"samplingThreshold":n,"validityPeriod":n}}}
//        "controlType" is optional; when present it must be "prbBlacklist".
//        blacklistedPRBs is required. These are the rules of OAI's gNB
//        decoder, which in addition does not range-check the two optional
//        fields; this one holds them to the grammar's ranges.
DecodeResult decode_spectrum_dapp_control_aper(const std::vector<uint8_t>& in,
                                               SpectrumPRBBlacklistControl& out,
                                               std::string& why);
DecodeResult decode_spectrum_dapp_control_json(const std::vector<uint8_t>& in,
                                               SpectrumPRBBlacklistControl& out,
                                               std::string& why);

} // namespace e3sm_spectrum
