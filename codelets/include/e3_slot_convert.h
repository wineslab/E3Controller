/*
 * e3_slot_convert.h — cbf16 -> fp16 row conversion, shared by BOTH writers.
 *
 * Extracted verbatim from E3Controller's ShmIqWriter::publish_row_cbf16 so the
 * controller and the gNB-side jbpf helper produce BYTE-IDENTICAL rows. This is
 * the single most important invariant in the publish-direct design: bf16 and
 * fp16 are both 2 bytes, so if the two paths ever disagree on the transform or
 * the scale, nothing errors — the dApp silently computes a wrong spectrum.
 *
 * Header-only on purpose: the gNB helper and the controller live in different
 * repos and different build systems, so a shared .cpp would need a shared build
 * target. See codelets/include/jbpf_e3_slot_api.h for the rest of the contract.
 *
 * ---------------------------------------------------------------------------
 * bf16 vs fp16, and why the scale exists
 *
 *   bf16: 1 sign /  8 exponent / 7 mantissa, bias 127 — the top 16 bits of an
 *         fp32. Huge range (~3.4e38), coarse precision.
 *   fp16: 1 sign /  5 exponent / 10 mantissa, bias  15 — max finite 65504,
 *         min normal ~6.1e-5. Narrow range, finer precision.
 *
 * bf16 therefore holds values fp16 cannot: above 65504 you get inf, below
 * ~6.1e-5 you decay into subnormals or zero. Grid IQ magnitudes depend on gain
 * settings, so `scale` shifts them into fp16's representable window before the
 * convert. A zero or negative scale zeroes the row and silently breaks the dApp,
 * which is why the caller validates it.
 *
 * The transform is bf16 -> fp32 (a 16-bit shift, free) -> x scale ->
 * fp32 -> fp16 (needs rounding + range clamp, hence F16C).
 * ---------------------------------------------------------------------------
 */

#ifndef E3_SLOT_CONVERT_H
#define E3_SLOT_CONVERT_H

#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

/*
 * VECTOR PATH REQUIRED.
 *
 * This loop runs in the gNB's PHY RX thread, inline in the hook. A scalar
 * fallback there is not a graceful degradation, it is a latency regression in
 * the RAN's slot budget.
 *
 * This exact trap has already cost us once: the adaptive_cpu dApp's bimodal
 * latency turned out to be an x86-scalar fp16 convert, because its fast loop was
 * `#if __aarch64__` with no x86 branch and the build carried no -mavx2/-mf16c.
 * Nothing warned; it just ran slow, 36% of slots landing in a 600-899us tail.
 *
 * So: fail the build rather than quietly emit the scalar path. Define
 * E3_ALLOW_SCALAR_CONVERT to opt out (host tooling, non-x86, or the
 * microbenchmark's deliberate scalar baseline).
 */
#if !defined(E3_ALLOW_SCALAR_CONVERT)
#if !(defined(__AVX2__) && defined(__F16C__)) && !defined(__aarch64__)
#error "e3_slot_convert.h: no vector path. Compile with -mavx2 -mf16c (or -march=native). \
Define E3_ALLOW_SCALAR_CONVERT only if a scalar convert in the RX thread is genuinely acceptable."
#endif
#endif

namespace e3_convert {

/* ---- Row geometry ----
 *
 * Must match SharedMemoryHeader's advertised dimensions. Kept as explicit
 * parameters rather than compile-time constants so the same code serves 2/4/8
 * ports and different bandwidths (see the plan's D2: strides derived from the
 * header at attach time, not from constexpr).
 */
struct RowGeometry {
    std::uint32_t nof_symbols;     /* 14 */
    std::uint32_t nof_subcarriers; /* 3276 = 273 PRB * 12 */
    std::uint32_t nof_ants;        /* row capacity, i.e. antennas the row holds */

    /* uint16 samples per antenna = 2 * symbols * subcarriers (real + imag).
     * Also the per-antenna fp16 stride within the row, so source port p maps to
     * destination antenna p. */
    constexpr std::size_t u16_per_ant() const
    {
        return static_cast<std::size_t>(nof_symbols) * nof_subcarriers * 2u;
    }

    constexpr std::size_t row_u16() const { return u16_per_ant() * nof_ants; }
    constexpr std::size_t row_bytes() const { return row_u16() * sizeof(std::uint16_t); }
};

/* ---- Scalar reference ----
 *
 * Correctness reference and the microbenchmark's baseline. Not for the RX path.
 */
inline std::uint16_t
bf16_to_fp16_scalar(std::uint16_t bf16, float scale)
{
#if defined(__F16C__)
    union {
        std::uint32_t u;
        float         f;
    } u32;
    u32.u      = static_cast<std::uint32_t>(bf16) << 16;
    __m128  ss = _mm_set_ss(u32.f * scale);
    __m128i ph = _mm_cvtps_ph(ss, _MM_FROUND_TO_NEAREST_INT);
    return static_cast<std::uint16_t>(_mm_extract_epi16(ph, 0));
#else
    union {
        std::uint32_t u;
        float         f;
    } u32;
    u32.u   = static_cast<std::uint32_t>(bf16) << 16;
    float v = u32.f * scale;
    union {
        std::uint32_t u;
        float         f;
    } vu;
    vu.f                = v;
    std::uint32_t sign  = (vu.u >> 31) & 0x1u;
    std::int32_t  exp8  = static_cast<std::int32_t>((vu.u >> 23) & 0xffu);
    std::uint32_t man23 = vu.u & 0x7fffffu;
    if (exp8 == 0) {
        return static_cast<std::uint16_t>(sign << 15);
    }
    if (exp8 == 0xff) {
        return static_cast<std::uint16_t>((sign << 15) | (0x1fu << 10) | (man23 ? 0x200u : 0u));
    }
    std::int32_t exp5 = exp8 - 127 + 15;
    if (exp5 >= 0x1f) {
        return static_cast<std::uint16_t>((sign << 15) | (0x1fu << 10));
    }
    if (exp5 <= 0) {
        return static_cast<std::uint16_t>(sign << 15);
    }
    std::uint32_t man10 = man23 >> 13;
    /* round-to-nearest-even on the dropped bits */
    std::uint32_t rem = man23 & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (man10 & 1u))) {
        ++man10;
        if (man10 == 0x400u) {
            man10 = 0;
            ++exp5;
            if (exp5 >= 0x1f) {
                return static_cast<std::uint16_t>((sign << 15) | (0x1fu << 10));
            }
        }
    }
    return static_cast<std::uint16_t>((sign << 15) | (static_cast<std::uint32_t>(exp5) << 10) | man10);
#endif
}

/*
 * Convert one antenna's worth of bf16 to fp16, applying `scale`.
 *
 * `n_u16` MUST be a multiple of 8 on the AVX2 path. For 273 PRB / 14 symbols it
 * is 91,728 = 8 * 11,466, so there is no tail; asserted by the caller.
 */
inline void
convert_ant(std::uint16_t* dst, const std::uint16_t* src, std::size_t n_u16, float scale)
{
#if defined(__AVX2__) && defined(__F16C__)
    /* 8 bf16 -> 8 fp16 per iteration:
     *   load 8 bf16 (16 B) -> zero-extend to 8x u32 -> shift left 16 (bf16
     *   promoted to fp32) -> multiply by scale -> cvtps_ph -> store 16 B. */
    const __m256 v_scale = _mm256_set1_ps(scale);
    std::size_t  i       = 0;
    for (; i + 8 <= n_u16; i += 8) {
        __m128i b16     = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        __m256i u32_ext = _mm256_cvtepu16_epi32(b16);
        __m256i u32_sh  = _mm256_slli_epi32(u32_ext, 16);
        __m256  f32     = _mm256_castsi256_ps(u32_sh);
        __m256  fscaled = _mm256_mul_ps(f32, v_scale);
        __m128i ph      = _mm256_cvtps_ph(fscaled, _MM_FROUND_TO_NEAREST_INT);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), ph);
    }
    for (; i < n_u16; ++i) { /* tail; empty for 273 PRB */
        dst[i] = bf16_to_fp16_scalar(src[i], scale);
    }
#else
    for (std::size_t i = 0; i < n_u16; ++i) {
        dst[i] = bf16_to_fp16_scalar(src[i], scale);
    }
#endif
}

/*
 * Convert a whole slot into one row.
 *
 * `src` is the gNB resource grid's contiguous cbf16 storage, laid out
 * [port][symbol][subcarrier]. `nof_ports` antennas are written; antennas beyond
 * that are left untouched (the row is zero-filled once at region creation).
 *
 * Returns bytes written to `dst`.
 */
inline std::size_t
convert_slot(std::uint16_t* dst, const std::uint16_t* src, const RowGeometry& geom, std::uint32_t nof_ports,
             float scale)
{
    std::uint32_t ports = nof_ports ? nof_ports : 1u;
    if (ports > geom.nof_ants) {
        ports = geom.nof_ants;
    }
    const std::size_t n = geom.u16_per_ant();
    for (std::uint32_t a = 0; a < ports; ++a) {
        convert_ant(dst + static_cast<std::size_t>(a) * n, src + static_cast<std::size_t>(a) * n, n, scale);
    }
    return static_cast<std::size_t>(ports) * n * sizeof(std::uint16_t);
}

/* True when a vector path was actually compiled in. Log this at startup: it is
 * the difference between ~60us and a latency regression, and it is otherwise
 * invisible. */
inline constexpr bool
has_vector_path()
{
#if defined(__AVX2__) && defined(__F16C__)
    return true;
#else
    return false;
#endif
}

} // namespace e3_convert

#endif /* E3_SLOT_CONVERT_H */
