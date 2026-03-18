/*
 * bfp_decompress.cpp
 *
 * BFP (Block Floating Point) 9-bit decompression for O-RAN IQ samples.
 *
 * BFP 9-bit format per PRB (28 bytes total):
 *   Byte 0:       [exponent:4][reserved:4]
 *   Bytes 1-27:   12 × (9-bit I, 9-bit Q) = 216 bits = 27 bytes
 *
 * The 9-bit values are packed MSB-first in network byte order.
 * Each 9-bit value is a signed two's complement integer in [-256, 255].
 * The full-scale value is: mantissa << exponent
 */

#include "bfp_decompress.h"
#include <cstdio>

namespace e3sm_spectrum {

/**
 * Extract a 9-bit signed value from a packed bit stream.
 *
 * @param data      Pointer to the byte array containing packed bits
 * @param bit_offset  Bit offset from the start of data (MSB=0)
 * @return sign-extended int16_t value
 */
static inline int16_t extract_9bit(const uint8_t* data, unsigned bit_offset)
{
    // Byte index and bit position within that byte
    unsigned byte_idx = bit_offset / 8;
    unsigned bit_pos  = bit_offset % 8;

    // We need up to 3 bytes to extract 9 bits starting at bit_pos
    // Assemble a 24-bit window from up to 3 bytes
    uint32_t window = (uint32_t)data[byte_idx] << 16;
    window |= (uint32_t)data[byte_idx + 1] << 8;
    window |= (uint32_t)data[byte_idx + 2];

    // Shift right to align the 9-bit value to the LSB
    // The 9 bits start at bit_pos from the MSB of the 24-bit window
    unsigned shift = 24 - bit_pos - 9;
    uint16_t raw = (window >> shift) & 0x1FF;

    // Sign-extend from 9 bits to 16 bits
    if (raw & 0x100) {
        return (int16_t)(raw | 0xFE00);
    }
    return (int16_t)raw;
}

bool decompress_bfp_9bit(const uint8_t* data, size_t data_len,
                         uint16_t num_prbu,
                         std::vector<int16_t>& out_iq)
{
    size_t expected_len = (size_t)num_prbu * BFP_PRB_SIZE;
    if (data_len < expected_len) {
        std::fprintf(stderr,
            "[BFP] Data too short: got %zu bytes, expected %zu for %u PRBs\n",
            data_len, expected_len, num_prbu);
        return false;
    }

    out_iq.resize((size_t)num_prbu * INT16_PER_PRB);

    for (uint16_t prb = 0; prb < num_prbu; prb++) {
        const uint8_t* prb_data = data + (size_t)prb * BFP_PRB_SIZE;

        // First byte: exponent in upper 4 bits, reserved in lower 4
        // uint8_t exponent = (prb_data[0] >> 4) & 0x0F;
        uint8_t exponent = prb_data[0];


        // IQ data starts at byte 1 of the PRB block
        const uint8_t* iq_bytes = prb_data + 1;

        // Extract 24 values (12 I/Q pairs): I0, Q0, I1, Q1, ..., I11, Q11
        // Each is 9 bits, packed MSB-first
        size_t out_offset = (size_t)prb * INT16_PER_PRB;
        for (unsigned i = 0; i < INT16_PER_PRB; i++) {
            unsigned bit_offset = i * 9;
            int16_t mantissa = extract_9bit(iq_bytes, bit_offset);

            // Scale by exponent: value = mantissa << exponent
            int16_t value = mantissa << exponent;
            out_iq[out_offset + i] = value;
        }
    }

    return true;
}

} // namespace e3sm_spectrum
