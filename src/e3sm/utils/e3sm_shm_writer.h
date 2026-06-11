/*
 * POSIX SHM writer for the cuBB-compatible /dev/shm/e3_ran_buffers contract.
 *
 * The dApp side (spear-aerial-sample-apps adaptive_cpu) mmaps the same name
 * and reads:
 *
 *     offset = sizeof(SharedMemoryHeader)
 *            + fh_buf_idx * fh_buffer_size
 *            + fh_write_idx * (num_fh_samples * 2 bytes)
 *
 * Row layout the dApp expects: [ant][sym][prb][sc][I,Q] row-major, each
 * sample is an IEEE half-precision float. The dApp only reads antenna 0;
 * the row still has to be sized for N_ANTS_LAYOUT antennas to pass the
 * dApp's expected_u16 sanity check. The other antennas' slots are left as
 * zeros.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace e3sm_spectrum {

// Layout constants — must match the dApp's compile-time constants in
// subcarrier_power_app.cpp (N_ANTS, N_SYMBOLS, N_PRBS, N_SC_PER_PRB).
// Hardcoded to the srsRAN-janus 100 MHz @ 30 kHz SCS config (273 PRBs).
// If the RAN bandwidth changes, both this constant AND the dApp's N_PRBS
// must be updated in lockstep so the SHM row stride matches.
constexpr int kShmAntsLayout    = 4;     // dApp's expected_u16 uses N_ANTS=4
constexpr int kShmSymbolsPerRow = 14;
constexpr int kShmPrbsPerSymbol = 273;
constexpr int kShmScPerPrb      = 12;
constexpr int kShmScPerSymbol   = kShmPrbsPerSymbol * kShmScPerPrb;  // 3276
constexpr int kShmAntStride     = kShmSymbolsPerRow * kShmScPerSymbol * 2;
constexpr int kShmSymStride     = kShmScPerSymbol * 2;

// Mirrors SharedMemoryHeader in
// spear-aerial-sample-apps/dapps/common/e3_manager/e3_manager.h:39.
struct SharedMemoryHeader {
    uint32_t version;
    uint32_t fh_buffer_size;
    uint32_t pusch_buffer_size;
    uint32_t hest_buffer_size;
    uint32_t num_fh_samples;
    uint32_t num_fh_rows;
    uint32_t num_pusch_rows;
    uint32_t num_hest_rows;
    uint32_t max_hest_samples_per_row;
    uint32_t reserved[7];
};
static_assert(sizeof(SharedMemoryHeader) == 64, "SHM header layout drift");

class ShmIqWriter {
public:
    ShmIqWriter() = default;
    ~ShmIqWriter();

    ShmIqWriter(const ShmIqWriter&) = delete;
    ShmIqWriter& operator=(const ShmIqWriter&) = delete;

    // shm_open + ftruncate + mmap + write header. Returns false on failure
    // (with errno set + a message on stderr).
    bool open(const std::string& shm_name, size_t total_size);

    void close();

    // Write antenna 0 of one full slot row (14 symbols × 3276 SCs × I/Q) into
    // the next ring slot. iq_int16 is laid out as [sym][sc][I,Q] (3276*2*14
    // int16 values). Returns the (fh_buffer_index, fh_write_index) that the
    // dApp will use to address the row.
    void publish_row(const int16_t* iq_int16,
                     uint8_t& out_buffer_index,
                     uint32_t& out_write_index);

    // Sibling of publish_row for the slot-level pipeline. Input is the
    // codelet-produced cbf16_t blob (bf16 real + bf16 imag, 4 bytes per
    // complex sample) laid out as [sym][sc][I,Q] - matches ocudu's
    // resource_grid_reader_impl tensor layout for a single port.
    // Converts bf16 → IEEE half (fp16) on the fly so the dApp's reader
    // sees the same wire shape it expects from the legacy int16 path.
    // Returns the (fh_buffer_index, fh_write_index) like publish_row.
    void publish_row_cbf16(const uint8_t* iq_cbf16_bytes,
                           uint8_t& out_buffer_index,
                           uint32_t& out_write_index);

    uint32_t num_fh_rows()   const { return num_fh_rows_; }
    uint32_t num_buffers()   const { return num_buffers_; }
    uint32_t num_fh_samples() const { return num_fh_samples_; }

    // Scale factor applied to bf16 values in publish_row_cbf16 before
    // they are converted to fp16. ocudu's resource grid stores cbf16
    // values whose natural magnitude depends on the RU's BFP scale,
    // and the dApp's dBFS display is calibrated against fp16 full
    // scale (65504). Tuning this knob shifts the on-wire fp16
    // magnitudes up/down by a constant linear factor so the dApp's
    // dBFS readouts land in the displayable [-110, -10] range. Tuned
    // by env var E3_CBF16_SCALE (read at open() time). Default 1.0
    // means "pass through unchanged".
    float cbf16_scale() const { return cbf16_scale_; }

private:
    int fd_ = -1;
    void* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    std::string shm_name_;

    SharedMemoryHeader* header_ = nullptr;
    uint8_t* buffers_base_ = nullptr;

    uint32_t num_fh_samples_ = 0;   // 16-bit values per row
    uint32_t row_bytes_      = 0;
    uint32_t num_fh_rows_    = 0;
    uint32_t num_buffers_    = 0;
    uint32_t fh_buffer_size_ = 0;

    // Ring cursor: which row gets the next publish.
    uint32_t next_row_ = 0;
    uint8_t  next_buf_ = 0;

    // See cbf16_scale() above for semantics. Set in open() from
    // E3_CBF16_SCALE env var (default 1.0).
    float cbf16_scale_ = 1.0f;
};

}  // namespace e3sm_spectrum
