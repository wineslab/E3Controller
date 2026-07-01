#include "e3sm_shm_writer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace e3sm_spectrum {

namespace {

// int16 → IEEE half-precision (round-to-nearest-even), with a /2048 scale.
//
// The BFP-decompressed IQ samples land in int16 fixed-point. The dApp's
// reader follows the NVIDIA cuBB / Aerial convention: it reads fp16 and
// multiplies by 1/fp16_beta = 2048 to recover an int16-magnitude domain
// for the legacy detector. To stay compatible with that convention, we
// store value/2048 as fp16 here — i.e. subtract 11 from the biased
// exponent (2048 = 2^11). For all int16 inputs the resulting exponent
// stays in the FP16 normal range (biased 4..19), so no subnormal handling
// is needed.
inline uint16_t int16_to_fp16(int16_t v) {
    if (v == 0) return 0;
    uint32_t sign = (v < 0) ? 0x8000u : 0u;
    uint32_t mag  = (v < 0) ? static_cast<uint32_t>(-static_cast<int32_t>(v))
                            : static_cast<uint32_t>(v);

    // Find the leading-1 position of mag.
    int msb = 31 - __builtin_clz(mag);  // 0..14 since |v| < 2^15
    int exp_unbiased = msb - 11;        // /2048 → shift exponent down by 11
    int exp_biased   = exp_unbiased + 15;

    // Mantissa = bits below the leading 1, shifted into the 10-bit field.
    // mag has msb at position `msb`; the 10-bit mantissa is bits [msb-1 : msb-10].
    uint32_t mantissa;
    uint32_t round_bit = 0;
    uint32_t sticky    = 0;
    if (msb >= 10) {
        int shift = msb - 10;
        mantissa  = (mag >> shift) & 0x3ffu;
        round_bit = (mag >> (shift - 1)) & 0x1u;
        sticky    = (mag & ((1u << (shift - 1)) - 1u)) ? 1u : 0u;
    } else {
        mantissa = (mag << (10 - msb)) & 0x3ffu;
    }

    uint16_t half = static_cast<uint16_t>(sign | (exp_biased << 10) | mantissa);

    // Round-to-nearest, ties-to-even.
    if (round_bit && (sticky || (mantissa & 1u))) {
        half = static_cast<uint16_t>(half + 1);  // exponent rolls over naturally if mantissa overflows
    }
    return half;
}

// bf16 → IEEE half (fp16) via float32 intermediate, with a runtime
// linear scale applied between the two formats so the dApp's dBFS
// display can be calibrated.
//
// bf16 layout: 1 sign + 8 exp + 7 mantissa = top 16 bits of a float32,
// so bf16 → float32 is a single left-shift. We then multiply by `scale`
// (a runtime parameter, see ShmIqWriter::cbf16_scale()) and convert the
// product to fp16 via F16C (_mm_cvtps_ph), enabled by -march=native on
// any AVX-capable CPU.
//
// Why a scale knob exists: ocudu's resource grid stores cbf16 REs
// whose natural magnitude depends on the RU's BFP scale and antenna
// gain. The legacy int16_to_fp16 path bakes in a /2048 scale so the
// dApp sees fp16 magnitudes around [0, 16]. The slot-level path here
// needs an equivalent tuning, but the right factor depends on the
// physical deployment and isn't known a-priori. Default scale = 1.0
// (pass-through) plus an E3_CBF16_SCALE env-var override is enough
// for in-field calibration without a rebuild.
inline uint16_t bf16_to_fp16(uint16_t bf16, float scale) {
#if defined(__F16C__)
    union { uint32_t u; float f; } u32;
    u32.u = static_cast<uint32_t>(bf16) << 16;
    __m128  ss = _mm_set_ss(u32.f * scale);
    __m128i ph = _mm_cvtps_ph(ss, _MM_FROUND_TO_NEAREST_INT);
    return static_cast<uint16_t>(_mm_extract_epi16(ph, 0));
#else
    // Fallback: bf16 → float32 → scale → manual float32 → fp16.
    // We give up the bit-tricky fast path here because scale is a
    // runtime float; correctness wins on the slow path.
    union { uint32_t u; float f; } u32;
    u32.u = static_cast<uint32_t>(bf16) << 16;
    float    v   = u32.f * scale;
    union { uint32_t u; float f; } vu;
    vu.f = v;
    uint32_t sign = (vu.u >> 31) & 0x1;
    int32_t  exp8 = static_cast<int32_t>((vu.u >> 23) & 0xff);
    uint32_t man23 = vu.u & 0x7fffff;
    if (exp8 == 0) {
        return static_cast<uint16_t>(sign << 15);
    }
    if (exp8 == 0xff) {
        return static_cast<uint16_t>((sign << 15) | (0x1f << 10) | (man23 ? 1u : 0u));
    }
    int32_t exp5_signed = exp8 - 127 + 15;
    if (exp5_signed <= 0) return static_cast<uint16_t>(sign << 15);
    if (exp5_signed >= 0x1f) return static_cast<uint16_t>((sign << 15) | (0x1f << 10));
    uint32_t man10 = man23 >> 13;
    return static_cast<uint16_t>((sign << 15) | (exp5_signed << 10) | man10);
#endif
}

}  // namespace

ShmIqWriter::~ShmIqWriter() {
    close();
}

bool ShmIqWriter::open(const std::string& shm_name, size_t total_size) {
    if (mapped_ != nullptr) {
        std::fprintf(stderr, "[ShmIqWriter] open() called twice\n");
        return false;
    }
    shm_name_ = shm_name;

    // Pick up the cbf16→fp16 scale factor from the env. We parse here
    // rather than in publish_row_cbf16 so the strtof cost (and the
    // "no env var → default" message) happens once at startup, not
    // per slot. Invalid / non-positive values fall back to 1.0 with
    // a warning - 0 or negative would zero the published row and
    // silently break the dApp.
    if (const char* env = std::getenv("E3_CBF16_SCALE"); env != nullptr) {
        char*  end   = nullptr;
        float  parsed = std::strtof(env, &end);
        if (end != env && std::isfinite(parsed) && parsed > 0.0f) {
            cbf16_scale_ = parsed;
            std::printf("[ShmIqWriter] cbf16 → fp16 scale = %g (from E3_CBF16_SCALE)\n",
                        static_cast<double>(cbf16_scale_));
        } else {
            std::fprintf(stderr,
                "[ShmIqWriter] WARNING: invalid E3_CBF16_SCALE=%s, "
                "using default %g\n",
                env, static_cast<double>(cbf16_scale_));
        }
    } else {
        std::printf("[ShmIqWriter] cbf16 → fp16 scale = %g "
                    "(default; set E3_CBF16_SCALE to tune)\n",
                    static_cast<double>(cbf16_scale_));
    }

    fd_ = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
        std::fprintf(stderr, "[ShmIqWriter] shm_open(%s) failed: %s\n",
                     shm_name.c_str(), std::strerror(errno));
        return false;
    }

    if (::ftruncate(fd_, static_cast<off_t>(total_size)) != 0) {
        std::fprintf(stderr, "[ShmIqWriter] ftruncate(%zu) failed: %s\n",
                     total_size, std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    void* m = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
        std::fprintf(stderr, "[ShmIqWriter] mmap(%zu) failed: %s\n",
                     total_size, std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    mapped_ = m;
    mapped_size_ = total_size;
    std::memset(mapped_, 0, mapped_size_);  // zero-fill so unused antenna slots
                                            // and pristine rows read as silence
    header_ = static_cast<SharedMemoryHeader*>(mapped_);

    // Layout sizing.
    num_fh_samples_ = static_cast<uint32_t>(kShmAntsLayout) * kShmSymbolsPerRow
                    * kShmScPerSymbol * 2;          // 366912 fp16's per row
    row_bytes_      = num_fh_samples_ * sizeof(uint16_t);
    num_buffers_    = 2;                            // double-buffered ring

    size_t usable = (total_size > sizeof(SharedMemoryHeader))
                    ? (total_size - sizeof(SharedMemoryHeader)) : 0;
    size_t per_buf = usable / num_buffers_;
    num_fh_rows_ = static_cast<uint32_t>(per_buf / row_bytes_);
    if (num_fh_rows_ == 0) {
        std::fprintf(stderr,
            "[ShmIqWriter] SHM size %zu too small for one row (%u bytes) "
            "across %u buffers\n", total_size, row_bytes_, num_buffers_);
        close();
        return false;
    }
    fh_buffer_size_ = num_fh_rows_ * row_bytes_;

    header_->version                  = 1;
    header_->fh_buffer_size           = fh_buffer_size_;
    header_->pusch_buffer_size        = 0;
    header_->hest_buffer_size         = 0;
    header_->num_fh_samples           = num_fh_samples_;
    header_->num_fh_rows              = num_fh_rows_;
    header_->num_pusch_rows           = 0;
    header_->num_hest_rows            = 0;
    header_->max_hest_samples_per_row = 0;
    std::memset(header_->reserved, 0, sizeof(header_->reserved));

    buffers_base_ = static_cast<uint8_t*>(mapped_) + sizeof(SharedMemoryHeader);
    next_row_ = 0;
    next_buf_ = 0;

    std::printf("[ShmIqWriter] %s opened (%zu bytes): %u buffers × %u rows × "
                "%u bytes/row (num_fh_samples=%u)\n",
                shm_name.c_str(), total_size,
                num_buffers_, num_fh_rows_, row_bytes_, num_fh_samples_);
    return true;
}

void ShmIqWriter::close() {
    if (mapped_) {
        ::munmap(mapped_, mapped_size_);
        mapped_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!shm_name_.empty()) {
        ::shm_unlink(shm_name_.c_str());
        shm_name_.clear();
    }
    header_ = nullptr;
    buffers_base_ = nullptr;
    mapped_size_ = 0;
}

void ShmIqWriter::publish_row(const int16_t* iq_int16,
                              uint8_t& out_buffer_index,
                              uint32_t& out_write_index)
{
    out_buffer_index = next_buf_;
    out_write_index  = next_row_;

    // Antenna-0 region of the target row. The remaining 3 antennas are kept
    // at zero (set once in open()); the dApp only reads antenna 0.
    uint8_t* row_base = buffers_base_
                     + static_cast<size_t>(next_buf_) * fh_buffer_size_
                     + static_cast<size_t>(next_row_) * row_bytes_;
    uint16_t* ant0 = reinterpret_cast<uint16_t*>(row_base);

    const size_t n_pairs = static_cast<size_t>(kShmSymbolsPerRow) * kShmScPerSymbol;
    for (size_t i = 0; i < n_pairs; ++i) {
        ant0[i * 2 + 0] = int16_to_fp16(iq_int16[i * 2 + 0]);
        ant0[i * 2 + 1] = int16_to_fp16(iq_int16[i * 2 + 1]);
    }

    // Advance the ring. Buffer roll-over only when we wrap past the last row.
    if (++next_row_ >= num_fh_rows_) {
        next_row_ = 0;
        next_buf_ = static_cast<uint8_t>((next_buf_ + 1) % num_buffers_);
    }
}

void ShmIqWriter::publish_row_cbf16(const uint8_t* iq_cbf16_bytes,
                                    uint16_t nof_ports,
                                    uint8_t& out_buffer_index,
                                    uint32_t& out_write_index)
{
    out_buffer_index = next_buf_;
    out_write_index  = next_row_;

    uint8_t* row_base = buffers_base_
                     + static_cast<size_t>(next_buf_) * fh_buffer_size_
                     + static_cast<size_t>(next_row_) * row_bytes_;
    uint16_t* row_u16 = reinterpret_cast<uint16_t*>(row_base);

    // Source cbf16 layout: [port][sym][sc] of (bf16 real, bf16 imag) - 4 bytes
    // per complex sample. Each bf16 is read as a little-endian uint16.
    // The 2-byte alignment of cbf16 matches uint16, so we can just
    // reinterpret_cast safely.
    const uint16_t* src = reinterpret_cast<const uint16_t*>(iq_cbf16_bytes);

    // Hoist scale out of the loop so it's a constant for the SIMD path.
    const float scale = cbf16_scale_;
    // n_u16 = uint16 samples PER ANTENNA = 2 * (sym * sc) — real + imag in one
    // linear pass (the per-pair structure doesn't matter for the bit-wise
    // bf16 -> fp16 transformation). This is also the per-antenna fp16 stride
    // within the row (== kShmAntStride): both src ([port][..]) and dst
    // ([ant][..]) advance by n_u16 per antenna, so port p maps to antenna p.
    const size_t n_u16 = static_cast<size_t>(kShmSymbolsPerRow) * kShmScPerSymbol * 2u;

    // Write every delivered antenna; antennas >= nof_ports stay zero (the row
    // was zero-filled once in open()). Clamp to the row's antenna capacity.
    uint16_t ports = nof_ports ? nof_ports : 1;
    if (ports > kShmAntsLayout) ports = kShmAntsLayout;

    for (uint16_t a = 0; a < ports; ++a) {
        uint16_t*       dst  = row_u16 + static_cast<size_t>(a) * n_u16;
        const uint16_t* srca = src     + static_cast<size_t>(a) * n_u16;
#if defined(__AVX2__) && defined(__F16C__)
        // Fast path: 8 bf16 -> 8 fp16 per iteration via AVX2 + F16C:
        //   1. load 8 bf16 (16 bytes); 2. zero-extend to 8 x uint32;
        //   3. shift left 16 -> bf16 promoted to float32; 4. multiply by scale;
        //   5. convert 8 float32 -> 8 fp16 (_mm256_cvtps_ph); 6. store 16 bytes.
        const __m256 v_scale = _mm256_set1_ps(scale);
        size_t i = 0;
        for (; i + 8 <= n_u16; i += 8) {
            __m128i b16 = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(srca + i));
            __m256i u32_ext = _mm256_cvtepu16_epi32(b16);
            __m256i u32_sh  = _mm256_slli_epi32(u32_ext, 16);
            __m256  f32     = _mm256_castsi256_ps(u32_sh);
            __m256  fscaled = _mm256_mul_ps(f32, v_scale);
            __m128i ph      = _mm256_cvtps_ph(fscaled, _MM_FROUND_TO_NEAREST_INT);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), ph);
        }
        // n_u16 is a multiple of 8 per antenna (273 PRB -> 91728), so the AVX2
        // loop handles every element; assert it and skip a scalar tail.
        static_assert((kShmSymbolsPerRow * kShmScPerSymbol * 2) % 8 == 0,
                      "n_u16 must be a multiple of 8 for the AVX2 path");
        (void)i;
#else
        for (size_t i = 0; i < n_u16; ++i) {
            dst[i] = bf16_to_fp16(srca[i], scale);
        }
#endif
    }

    if (++next_row_ >= num_fh_rows_) {
        next_row_ = 0;
        next_buf_ = static_cast<uint8_t>((next_buf_ + 1) % num_buffers_);
    }
}

}  // namespace e3sm_spectrum
