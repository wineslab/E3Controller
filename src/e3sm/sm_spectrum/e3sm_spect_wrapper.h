/*
 * Wrapper helpers for encoding Spectrum Service Model ASN.1 types.
 * Uses APER encoding via asn1c-generated C types.
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

struct SpectrumIQIndication {
    std::vector<uint8_t> iq_data;       // Raw I/Q sample bytes (OCTET STRING)
    uint32_t sample_count;              // Number of I/Q samples
    uint32_t timestamp;  // Unix timestamp (optional)
};

struct SpectrumPRBBlacklistControl {
    std::vector<int> blacklisted_prbs;          // List of PRB indices
    int sampling_threshold;      // Sampling ratio (0..100)
    int validity_period;         // Validity in seconds (1..3600)
};

struct SpectrumRanFunctionData {
    std::vector<uint8_t> name;
    int version;
    std::vector<uint8_t> description;
};

// Encode Spectrum-IQDataIndication into APER bytes
bool encode_spectrum_iq_indication(const SpectrumIQIndication& in, std::vector<uint8_t>& out);

// Encode Spectrum-PRBBlacklistControl into APER bytes
bool encode_spectrum_prb_blacklist_control(const SpectrumPRBBlacklistControl& in, std::vector<uint8_t>& out);

// Encode RAN function data into APER bytes
bool encode_spectrum_ran_function_data(std::vector<uint8_t>& out);

// Decode Spectrum-PRBBlacklistControl from APER bytes
bool decode_spectrum_prb_blacklist_control(const std::vector<uint8_t>& in, SpectrumPRBBlacklistControl& out);

} // namespace e3sm_spectrum
