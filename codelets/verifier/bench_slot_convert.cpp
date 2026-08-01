/*
 * bench_slot_convert — microbenchmark for the cbf16 -> fp16 slot convert (B3).
 *
 * The one piece of workstream B that carries real risk. This loop moves from a
 * dedicated E3Controller worker core into the gNB's PHY RX thread, inline in the
 * hook, so its cost lands in the RAN's per-slot budget rather than on spare
 * controller CPU.
 *
 * What we need to know before committing:
 *   1. Does the fused convert stay memory-bound, i.e. roughly as fast as a plain
 *      memcpy of the same volume? If yes, replacing
 *      "codelet copy (56us) + controller convert (74us)" with one fused pass is
 *      a clear win. If not, the RX thread pays.
 *   2. How much does the scalar fallback cost? That quantifies the damage if the
 *      -mavx2/-mf16c flags ever go missing, which is exactly how the
 *      adaptive_cpu bimodal-latency bug happened.
 *
 * Build (must be explicit about the ISA — that is the point):
 *   g++ -O2 -std=c++17 -mavx2 -mf16c -I../include bench_slot_convert.cpp -o bench_slot_convert
 *   g++ -O2 -std=c++17 -DE3_ALLOW_SCALAR_CONVERT -DBENCH_FORCE_SCALAR \
 *       -I../include bench_slot_convert.cpp -o bench_slot_convert_scalar
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

#ifdef BENCH_FORCE_SCALAR
/* Deliberately hide the vector path so we can price the fallback. */
#undef __AVX2__
#undef __F16C__
#define E3_ALLOW_SCALAR_CONVERT 1
#endif

#include "e3_slot_convert.h"

using clk = std::chrono::steady_clock;

namespace {

struct Result {
    double us_median;
    double us_p95;
    double gbps; /* counting bytes read + bytes written */
};

/* Template rather than std::function: an indirect call inside the timing loop
 * would be measured along with the work. */
template <typename Fn>
Result
time_it(std::size_t bytes_moved, int iters, Fn&& fn)
{
    std::vector<double> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        auto t0 = clk::now();
        fn();
        auto t1 = clk::now();
        samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    Result r{};
    r.us_median = samples[samples.size() / 2];
    r.us_p95    = samples[static_cast<std::size_t>(samples.size() * 0.95)];
    r.gbps      = static_cast<double>(bytes_moved) / (r.us_median * 1e-6) / 1e9;
    return r;
}

void
run_config(std::uint32_t nof_ports, int iters)
{
    e3_convert::RowGeometry geom{14u, 3276u, nof_ports};
    const std::size_t       n_u16_total = geom.row_u16();
    const std::size_t       src_bytes   = n_u16_total * sizeof(std::uint16_t);

    std::vector<std::uint16_t> src(n_u16_total);
    std::vector<std::uint16_t> dst(n_u16_total, 0);
    std::vector<std::uint16_t> memcpy_dst(n_u16_total, 0);

    /* bf16 patterns spanning a realistic magnitude range, so the convert hits
     * normals rather than a degenerate all-zero fast case. */
    std::mt19937                                 rng(12345);
    std::uniform_int_distribution<std::uint32_t> pick(0x3000u, 0x4100u);
    for (auto& s : src) {
        s = static_cast<std::uint16_t>(pick(rng));
    }

    const float scale = 1.0f;

    /* touch both buffers once so we are not measuring first-touch page faults */
    std::memset(dst.data(), 0, src_bytes);
    std::memset(memcpy_dst.data(), 0, src_bytes);

    const std::size_t traffic = src_bytes * 2; /* read + write */

    Result conv = time_it(traffic, iters, [&] {
        e3_convert::convert_slot(dst.data(), src.data(), geom, nof_ports, scale);
    });

    Result mcpy = time_it(traffic, iters, [&] { std::memcpy(memcpy_dst.data(), src.data(), src_bytes); });

    std::printf("  %u ports  %8zu B/slot   convert %7.1f us (p95 %7.1f)  %5.1f GB/s"
                "   | memcpy %7.1f us  %5.1f GB/s   | ratio %4.2fx\n",
                nof_ports, src_bytes, conv.us_median, conv.us_p95, conv.gbps, mcpy.us_median, mcpy.gbps,
                conv.us_median / mcpy.us_median);
}

} // namespace

int
main(int argc, char** argv)
{
    int iters = (argc > 1) ? std::atoi(argv[1]) : 200;

    std::printf("bench_slot_convert — cbf16 -> fp16, 14 symbols x 3276 subcarriers\n");
    std::printf("vector path compiled in: %s\n", e3_convert::has_vector_path() ? "YES (AVX2+F16C)" : "NO (SCALAR)");
    std::printf("iterations per config: %d   (median / p95 reported)\n", iters);
    std::printf("throughput counts bytes READ + WRITTEN\n\n");

    /* Correctness: vector and scalar must agree bit-for-bit, or the two writers
     * would produce different rows and acceptance criterion 4 (byte-identical
     * dApp input) is meaningless. */
    {
        e3_convert::RowGeometry    geom{14u, 3276u, 1u};
        const std::size_t          n = geom.u16_per_ant();
        std::vector<std::uint16_t> src(n), a(n), b(n);
        for (std::size_t i = 0; i < n; ++i) {
            src[i] = static_cast<std::uint16_t>((i * 2654435761u) & 0xffffu);
        }
        e3_convert::convert_ant(a.data(), src.data(), n, 1.0f);
        for (std::size_t i = 0; i < n; ++i) {
            b[i] = e3_convert::bf16_to_fp16_scalar(src[i], 1.0f);
        }
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (a[i] != b[i]) {
                ++mismatches;
            }
        }
        std::printf("vector vs scalar agreement over %zu samples: %s (%zu mismatches)\n\n", n,
                    mismatches == 0 ? "IDENTICAL" : "MISMATCH", mismatches);
        if (mismatches) {
            return 1;
        }
    }

    for (std::uint32_t ports : {2u, 4u, 8u}) {
        run_config(ports, iters);
    }

    std::printf("\nreference points from the deployed system:\n");
    std::printf("  codelet eBPF copy of 733,824 B  : ~56 us   (gnb_to_codelet, in RX thread)\n");
    std::printf("  controller publish_row_cbf16    : ~74 us   (on a pinned worker core)\n");
    std::printf("  fusing them should cost ~ the 4-port convert figure above, in the RX thread.\n");
    return 0;
}
