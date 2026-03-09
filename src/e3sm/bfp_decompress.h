/*
 * bfp_decompress.h
 *
 * BFP (Block Floating Point) 9-bit decompression for O-RAN IQ samples.
 *
 * BFP format per PRB:
 *   [1 byte: exponent (4 bits exponent + 4 bits reserved)]
 *   [27 bytes: 12 I/Q pairs × 9 bits × 2 = 216 bits packed]
 *
 * Output: interleaved int16 pairs (I0, Q0, I1, Q1, ...) per PRB.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace e3sm_spectrum {

/// Size of one BFP-compressed PRB in bytes: 1 (exponent) + 27 (9-bit IQ) = 28
constexpr size_t BFP_PRB_SIZE = 28;

/// Number of I/Q pairs per PRB (12 subcarriers)
constexpr size_t IQ_PAIRS_PER_PRB = 12;

/// Output int16 values per PRB: 12 I + 12 Q = 24
constexpr size_t INT16_PER_PRB = IQ_PAIRS_PER_PRB * 2;

/**
 * Decompress BFP 9-bit compressed IQ data to int16 pairs.
 *
 * @param data       Pointer to raw BFP-compressed bytes
 * @param data_len   Length of input data in bytes
 * @param num_prbu   Number of PRBs in the data
 * @param out_iq     Output: interleaved int16 I/Q pairs (I0,Q0,I1,Q1,...)
 *
 * @return true on success, false if data_len is too small for num_prbu PRBs
 */
bool decompress_bfp_9bit(const uint8_t* data, size_t data_len,
                         uint16_t num_prbu,
                         std::vector<int16_t>& out_iq);

} // namespace e3sm_spectrum
