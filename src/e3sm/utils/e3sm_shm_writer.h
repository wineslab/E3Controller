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

#include "e3_config.h"

namespace e3sm_spectrum {

// The row geometry used to live here as four `constexpr` values with a comment
// requiring lockstep updates with the dApp's N_PRBS. It is now runtime state,
// taken from the YAML config at open() (e3config::RadioGeometry) and written
// into SharedMemoryHeader for consumers to read back.
//
// Two reasons the constants had to go:
//   - changing the antenna count or bandwidth required a recompile, and
//   - they were already inconsistent with the rest of the controller:
//     `--num-prbs` was accepted on the command line and fed only the eCPRI PRB
//     filter, while this file kept sizing rows from `kShmPrbsPerSymbol = 273`.
//
// The config is a bootstrap, not the truth — E3SMLayer1 validates it against
// the geometry the RAN reports in each slot. See e3_config.h.

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
    //
    // `geom` fixes the row stride and antenna capacity; `cbf16_scale` is the
    // bf16 -> fp16 factor, previously read from the E3_CBF16_SCALE env var and
    // now part of the config because the gNB-side publish helper needs the same
    // value and a disagreement would be silently wrong data.
    bool open(const std::string& shm_name, size_t total_size,
              const e3config::RadioGeometry& geom, float cbf16_scale,
              e3config::ShmWriter writer = e3config::ShmWriter::Controller);

    // True when this process is the one converting and writing rows. False in
    // `writer: gnb` mode, where the controller owns the region but the gNB-side
    // helper writes it.
    //
    // The publish_row* methods HARD-REFUSE when this is false. Both writers keep
    // their own ring cursor, so two active writers would silently overwrite each
    // other's rows -- making that impossible is the whole point of the switch.
    bool writes_rows() const { return writer_ == e3config::ShmWriter::Controller; }

    void close();

    // Write antenna 0 of one full slot row (14 symbols × 3276 SCs × I/Q) into
    // the next ring slot. iq_int16 is laid out as [sym][sc][I,Q] (3276*2*14
    // int16 values). Returns the (fh_buffer_index, fh_write_index) that the
    // dApp will use to address the row.
    // Returns false (writing nothing) when writes_rows() is false.
    bool publish_row(const int16_t* iq_int16,
                     uint8_t& out_buffer_index,
                     uint32_t& out_write_index);

    // Sibling of publish_row for the slot-level pipeline. Input is the
    // codelet-produced cbf16_t blob (bf16 real + bf16 imag, 4 bytes per
    // complex sample) laid out as [port][sym][sc][I,Q] - matches ocudu's
    // resource_grid_reader_impl tensor layout. Writes nof_ports antennas
    // (clamped to the configured antenna capacity); antennas beyond nof_ports
    // are zeroed on every publish -- NOT merely left alone. Rows recycle through
    // the ring, so a slot with fewer ports than the row holds would otherwise
    // expose IQ from an OLDER slot as if it were a quiet antenna.
    // Converts bf16 → IEEE half (fp16) on the fly so the dApp's reader
    // sees the same wire shape it expects from the legacy int16 path.
    // Returns the (fh_buffer_index, fh_write_index) like publish_row.
    // Returns false (writing nothing) when writes_rows() is false.
    bool publish_row_cbf16(const uint8_t* iq_cbf16_bytes,
                           uint16_t nof_ports,
                           uint8_t& out_buffer_index,
                           uint32_t& out_write_index);

    uint32_t num_fh_rows()   const { return num_fh_rows_; }
    uint32_t num_buffers()   const { return num_buffers_; }
    uint32_t num_fh_samples() const { return num_fh_samples_; }
    uint32_t row_bytes()      const { return row_bytes_; }

    // Geometry this writer was opened with. E3SMLayer1 uses it instead of the
    // old compile-time constants when sizing its expectations.
    const e3config::RadioGeometry& geometry() const { return geom_; }

    // Scale factor applied to bf16 values in publish_row_cbf16 before
    // they are converted to fp16. ocudu's resource grid stores cbf16
    // values whose natural magnitude depends on the RU's BFP scale,
    // and the dApp's dBFS display is calibrated against fp16 full
    // scale (65504). Tuning this knob shifts the on-wire fp16
    // magnitudes up/down by a constant linear factor so the dApp's
    // dBFS readouts land in the displayable [-110, -10] range. Tuned
    // Supplied by the config (shm.cbf16_scale). Default 1.0 means
    // "pass through unchanged".
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

    // See cbf16_scale() above for semantics. Set in open() from the config.
    float cbf16_scale_ = 1.0f;

    // Row geometry (antenna count, symbols, subcarriers) from the config.
    e3config::RadioGeometry geom_{};

    // Who writes rows. See writes_rows().
    e3config::ShmWriter writer_ = e3config::ShmWriter::Controller;
};

}  // namespace e3sm_spectrum
