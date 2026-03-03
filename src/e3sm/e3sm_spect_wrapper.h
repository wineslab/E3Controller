/*
 * Wrapper helpers for encoding Spectrum Service Model ASN.1 types.
 * Uses APER encoding via asn1c-generated C types.
 */
#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <string>

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

struct SpectrumConfigControl {
    int noise_floor_threshold;   // Noise floor threshold (-100..100)
    int averaging_frames;        // Averaging window (1..255)
    bool enable;                 // Enable/disable monitoring
};

// Encode Spectrum-IQDataIndication into APER bytes
bool encode_spectrum_iq_indication(const SpectrumIQIndication& in, std::vector<uint8_t>& out);

// Encode Spectrum-PRBBlacklistControl into APER bytes
bool encode_spectrum_prb_blacklist_control(const SpectrumPRBBlacklistControl& in, std::vector<uint8_t>& out);

// Encode Spectrum-ConfigControl into APER bytes
bool encode_spectrum_config_control(const SpectrumConfigControl& in, std::vector<uint8_t>& out);

} // namespace e3sm_spectrum
