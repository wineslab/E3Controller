/*
 * What does recording a slot cost?
 *
 * This exists because the mechanism it replaces got the answer wrong. The
 * per-slot stage CSV that used to live at the tail of E3SMLayer1::on_sample
 * opened an ofstream, wrote a row and *flushed* it, once per slot, on the
 * pipeline worker thread - so every number it reported included the cost of
 * reporting it. Replacing it is only justified if the replacement is cheap, and
 * "cheap" has to be measured rather than asserted.
 *
 * Three arms, one binary, no slot work:
 *
 *   none    the loop and the argument marshalling, nothing else - the floor
 *   latrec  exactly what on_sample now emits per slot, from the same header
 *   csv     the removed writer, reproduced field for field
 *
 * Deliberately no "real slot work" arm. Under the default `shm.writer: gnb` the
 * gNB converts the grid and writes the shared-memory row itself, so the
 * controller does no data movement at all - a slot-work arm here would measure
 * something this process does not do. And against real work the stamps were
 * never resolvable anyway: they are a fraction of a percent of it, so the only
 * honest output was a bound. The interesting number lives here, where a
 * per-slot write(2) and a handful of stores differ by an order of magnitude and
 * separate cleanly.
 *
 * Method notes that matter for believing the output:
 *   - Batch timing, not per-iteration. One clock_gettime costs more than a
 *     latrec stamp, so timing each iteration would measure the timing.
 *   - Arms are interleaved and rotated across batches, so a runner that slows
 *     down partway through penalises every arm equally instead of whichever one
 *     ran last.
 *   - Median across batches, with min/max, rather than a mean: one descheduled
 *     batch should not move the answer.
 *   - The clock's own cost is measured and reported, since it is the floor
 *     under every latrec stamp.
 *
 * The `csv` arm is a benchmark reference, not a recording facility: nothing in
 * the controller writes a per-slot CSV any more. It is kept so the mechanism
 * can still be priced after it is gone.
 *
 * Build with a libe3 configured -DLIBE3_ENABLE_LATREC=ON, or the latrec arm
 * measures the no-op stubs - which is itself worth checking, see --mode
 * compiled-out.
 */
#include "e3sm/l1_kpm/l1_kpm_trace.h"

#include <libe3/latrec.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <getopt.h>

namespace trace = e3sm_l1kpm_trace;

namespace {

using clock_type = std::chrono::steady_clock;

/* One slot's worth of plausible stage inputs. Values are arbitrary but
 * realistic in magnitude, so the CSV arm formats the same number of digits it
 * would in production (integer formatting cost scales with digit count). */
struct SlotFacts {
    uint64_t seq;
    uint32_t sfn;
    uint16_t abs_slot;
    /* Absolute times fed to the back-dated stamps. Offsets from `now` are tiny
     * on purpose - see slot_facts(). */
    uint64_t gnb_ts_ns;
    uint64_t codelet_entry_ts_ns;
    uint64_t codelet_ts_ns;
    uint64_t dispatch_ts_ns;
    uint32_t bytes_written;
    uint32_t nof_subc;
    std::size_t encoded_bytes;
    std::size_t subscribers;
};

SlotFacts slot_facts(uint64_t i)
{
    /* The two A1 stamps are back-dated, so they have to ascend across
     * iterations or the ring's monotone floor kicks in and we would be timing
     * the clamp path instead of the normal one.
     *
     * In production that is free: slots are >=500 us apart and A1 is ~70 us
     * deep, so slot N+1's gnb stamp is comfortably later than slot N's last
     * record. This loop iterates ~130 ns apart, so a 70 us back-date could not
     * possibly ascend. The offsets below are therefore nanoseconds, not
     * microseconds - and that costs nothing in fidelity, because what a stamp
     * costs does not depend on the magnitude of the timestamp it carries. It
     * depends on the code path, and this is the same one.
     *
     * The CSV arm does not use these at all; it formats fixed realistic
     * durations, so its integer-formatting cost stays representative. */
    const uint64_t now = latrec_tnow();
    SlotFacts f{};
    f.seq                 = i + 1;
    f.sfn                 = static_cast<uint32_t>(i % 1024);
    f.abs_slot            = static_cast<uint16_t>(i % 20);
    f.gnb_ts_ns           = now - 4;
    f.codelet_entry_ts_ns = now - 3;
    f.codelet_ts_ns       = now - 2;
    f.dispatch_ts_ns      = now - 1;
    f.bytes_written       = 733824;
    f.nof_subc            = 3276;
    f.encoded_bytes       = 96;
    f.subscribers         = 1;
    return f;
}

/* ---------------- the three ways of recording a slot ---------------- */

/* Arm `none`: the loop and the argument marshalling, nothing else. This is the
 * floor the other two are measured against, not zero. */
void record_none(const SlotFacts& f)
{
    asm volatile("" : : "r"(&f) : "memory");
}

/* Arm `latrec`: exactly what E3SMLayer1::on_sample emits per slot - the same
 * inline functions, from the same header, in the same order. */
void record_latrec(const SlotFacts& f)
{
    trace::record_begin(f.seq, f.sfn, f.abs_slot, f.gnb_ts_ns, f.codelet_entry_ts_ns);
    trace::process_begin(f.seq, f.codelet_ts_ns, f.dispatch_ts_ns, f.bytes_written);
    trace::encode_begin(f.seq, f.bytes_written);
    trace::encode_done(f.seq, f.encoded_bytes);
    trace::bind_libe3(f.seq);
    trace::emit_tail(f.seq, f.subscribers);
}

/* Arm `csv`: the removed mechanism, reproduced field for field - including the
 * saturating subtraction it used, and the flush that is the whole point. */
class CsvArm {
public:
    explicit CsvArm(const std::string& path) : out_(path, std::ios::out | std::ios::trunc)
    {
        out_ << "slot_seq,"
                "gnb_to_codelet_us,codelet_publish_us,"
                "codelet_to_dispatch_us,dispatch_to_handler_us,"
                "shm_ns,encode_ns,emit_ns,nof_subc,iq_bytes\n";
    }

    bool ok() const { return out_.is_open(); }

    void record(const SlotFacts& f)
    {
        /* Fixed, realistic stage durations - the measured shape at 30 kHz SCS:
         * ~5 us jbpf dispatch, ~66 us convert + row write, then the ring
         * transit and the queue wait. Literals rather than differences of
         * f's timestamps, because those are nanoseconds apart here (see
         * slot_facts) and would format far fewer digits than production. */
        out_ << f.seq << ','
             << 5      << ','
             << 66     << ','
             << 9      << ','
             << 15     << ','
             << 0      << ','
             << 1850   << ','
             << 2400   << ','
             << f.nof_subc << ','
             << f.bytes_written << '\n';
        out_.flush();  // the whole point: one flush per slot, on the data path
    }

private:
    std::ofstream out_;
};

/* ---------------- statistics ---------------- */

struct Summary {
    double median_ns;
    double min_ns;
    double max_ns;
};

Summary summarise(std::vector<double> batch_ns_per_iter)
{
    std::sort(batch_ns_per_iter.begin(), batch_ns_per_iter.end());
    Summary s{};
    const std::size_t n = batch_ns_per_iter.size();
    s.median_ns = (n % 2) ? batch_ns_per_iter[n / 2]
                          : 0.5 * (batch_ns_per_iter[n / 2 - 1] + batch_ns_per_iter[n / 2]);
    s.min_ns = batch_ns_per_iter.front();
    s.max_ns = batch_ns_per_iter.back();
    return s;
}

template <typename Fn>
double time_batch_ns_per_iter(Fn&& fn, uint64_t iters, uint64_t& counter)
{
    const auto t0 = clock_type::now();
    for (uint64_t i = 0; i < iters; ++i) {
        fn(counter++);
    }
    const auto t1 = clock_type::now();
    const double total =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return total / static_cast<double>(iters);
}

/* Percentage of one slot's budget at 30 kHz SCS. The number that actually
 * answers "was the old mechanism a problem?". */
double pct_of_slot_budget(double ns) { return 100.0 * ns / 500000.0; }

void print_row(const char* name, const Summary& s)
{
    std::printf("| %-22s | %12.1f | %10.1f | %10.1f | %8.4f%% |\n",
                name, s.median_ns, s.min_ns, s.max_ns, pct_of_slot_budget(s.median_ns));
}

bool latrec_compiled_in()
{
#ifdef LIBE3_ENABLE_LATREC
    return true;
#else
    return false;
#endif
}

int run_instrumentation(uint64_t iters, uint64_t batches, const std::string& csv_path)
{
    CsvArm csv(csv_path);
    if (!csv.ok()) {
        std::fprintf(stderr, "cannot open %s for the csv arm\n", csv_path.c_str());
        return 1;
    }

    std::vector<double> none_b, latrec_b, csv_b;
    uint64_t counter = 0;

    /* Warm up every arm before measuring anything: first-touch page faults on
     * the ring mapping and the ofstream buffer would otherwise be charged to
     * whichever arm ran first. */
    for (uint64_t i = 0; i < 512; ++i) {
        const SlotFacts f = slot_facts(counter++);
        record_none(f);
        record_latrec(f);
        csv.record(f);
    }

    for (uint64_t b = 0; b < batches; ++b) {
        /* Rotate the order so no arm is permanently first or last. */
        const int order[3] = {static_cast<int>(b % 3),
                              static_cast<int>((b + 1) % 3),
                              static_cast<int>((b + 2) % 3)};
        for (int slot = 0; slot < 3; ++slot) {
            switch (order[slot]) {
            case 0:
                none_b.push_back(time_batch_ns_per_iter(
                    [](uint64_t i) { record_none(slot_facts(i)); }, iters, counter));
                break;
            case 1:
                latrec_b.push_back(time_batch_ns_per_iter(
                    [](uint64_t i) { record_latrec(slot_facts(i)); }, iters, counter));
                break;
            default:
                csv_b.push_back(time_batch_ns_per_iter(
                    [&csv](uint64_t i) { csv.record(slot_facts(i)); }, iters, counter));
                break;
            }
        }
    }

    std::printf("\n### Recording one slot\n\n");
    std::printf("| %-22s | %12s | %10s | %10s | %9s |\n",
                "arm", "median ns", "min ns", "max ns", "of slot");
    std::printf("|------------------------|--------------|------------|------------|-----------|\n");
    const Summary sn = summarise(none_b);
    const Summary sl = summarise(latrec_b);
    const Summary sc = summarise(csv_b);
    print_row("none (loop floor)", sn);
    print_row(latrec_compiled_in() ? "latrec (5 records)" : "latrec (STUBS - off)", sl);
    print_row("csv (removed)", sc);

    const double latrec_net = sl.median_ns - sn.median_ns;
    const double csv_net    = sc.median_ns - sn.median_ns;
    std::printf("\nNet of the loop floor: latrec %.1f ns/slot, csv %.1f ns/slot.\n",
                latrec_net, csv_net);
    if (latrec_net > 0.0) {
        std::printf("The removed CSV cost %.0fx what the stamps cost.\n", csv_net / latrec_net);
    }
    std::printf("Clock read (the floor under every stamp): %u ns.\n",
                latrec_measure_clock_ns());

    /* A clamp here would mean the synthetic times did not ascend, i.e. the
     * bench is exercising the wrong path and its latrec arm is not comparable
     * with production. Report it rather than letting it pass silently. */
    if (const uint64_t clamped = trace::clamped(); clamped != 0) {
        std::printf("\nWARNING: %llu stamp(s) were clamped. The synthetic slot times are not\n"
                    "         ascending, so the latrec arm above is not measuring the normal\n"
                    "         path. Treat the number as invalid.\n",
                    static_cast<unsigned long long>(clamped));
        return 1;
    }
    if (!latrec_compiled_in()) {
        std::printf("\nNOTE: built without LIBE3_ENABLE_LATREC, so the latrec arm above is\n"
                    "      measuring the no-op stubs, not the recorder.\n");
    }
    return 0;
}

int run_compiled_out()
{
    /* A timing test cannot prove absence - only a build can. This reports what
     * the binary was built with so CI can assert on it; the companion check is
     * `nm` over a latrec-OFF build finding no global latrec symbols. */
    std::printf("LIBE3_ENABLE_LATREC=%s\n", latrec_compiled_in() ? "ON" : "OFF");
#ifdef LATREC_DEFAULT_DIR
    std::printf("LATREC_DEFAULT_DIR=%s\n", LATREC_DEFAULT_DIR);
#else
    std::printf("LATREC_DEFAULT_DIR=(unset)\n");
#endif
    return 0;
}

void usage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s [--mode instrumentation|compiled-out|all]\n"
        "          [--iters N] [--batches K] [--csv-path P] [--latrec-dir D]\n\n"
        "  --mode          which measurement to run (default: all)\n"
        "  --iters         iterations per batch (default: 2000)\n"
        "  --batches       batches per arm (default: 15)\n"
        "  --csv-path      scratch file for the csv arm (default: ./bench_csv_arm.log)\n"
        "  --latrec-dir    where to write stage-record rings\n", prog);
}

}  // namespace

int main(int argc, char** argv)
{
    std::string mode = "all";
    std::string csv_path = "./bench_csv_arm.log";
    std::string latrec_dir;
    uint64_t iters = 2000;
    uint64_t batches = 15;

    enum { OPT_MODE = 1000, OPT_ITERS, OPT_BATCHES, OPT_CSV_PATH, OPT_LATREC_DIR };
    static struct option opts[] = {
        {"mode",       required_argument, nullptr, OPT_MODE},
        {"iters",      required_argument, nullptr, OPT_ITERS},
        {"batches",    required_argument, nullptr, OPT_BATCHES},
        {"csv-path",   required_argument, nullptr, OPT_CSV_PATH},
        {"latrec-dir", required_argument, nullptr, OPT_LATREC_DIR},
        {"help",       no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int o;
    while ((o = getopt_long(argc, argv, "h", opts, nullptr)) != -1) {
        switch (o) {
        case OPT_MODE:       mode = optarg; break;
        case OPT_ITERS:      iters = std::strtoull(optarg, nullptr, 10); break;
        case OPT_BATCHES:    batches = std::strtoull(optarg, nullptr, 10); break;
        case OPT_CSV_PATH:   csv_path = optarg; break;
        case OPT_LATREC_DIR: latrec_dir = optarg; break;
        default:             usage(argv[0]); return o == 'h' ? 0 : 2;
        }
    }
    if (mode != "all" && mode != "instrumentation" && mode != "compiled-out") {
        usage(argv[0]);
        return 2;
    }

    if (!latrec_dir.empty()) {
        latrec_set_output_dir(latrec_dir.c_str());
    }
    /* Single-threaded, so one ring covers every arm. Opened here, before any
     * measurement, so the mapping is faulted in off the measured path. */
    trace::open_ring();

    std::printf("## Stage-recording cost\n");
    std::printf("\nlatrec: %s. Batches: %llu, iterations/batch: %llu.\n",
                latrec_compiled_in() ? "compiled in" : "NOT compiled in",
                static_cast<unsigned long long>(batches),
                static_cast<unsigned long long>(iters));

    int rc = 0;
    if (mode == "all" || mode == "compiled-out")    rc |= run_compiled_out();
    if (mode == "all" || mode == "instrumentation") rc |= run_instrumentation(iters, batches, csv_path);
    return rc;
}
