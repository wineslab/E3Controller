/*
 * e3_config.h — E3Controller configuration, loaded from a single YAML file.
 *
 * Replaces the previous 20 command-line options. `--config <file>` is now the
 * only argument, deliberately: two configuration mechanisms invite the two to
 * disagree, and this file's whole job is to be the one place the radio geometry
 * is stated.
 *
 * WHY YAML rather than JSON (which would need no new dependency, since libe3
 * already pulls nlohmann): ocudu's own configuration is YAML, including the
 * `jbpf:` section this file must agree with. Keeping both sides in the same
 * language makes them diffable and reviewable together — which is the point,
 * because the controller/gNB geometry agreement is not otherwise enforced by
 * anything structural.
 */

#ifndef E3_CONFIG_H
#define E3_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

#include <libe3/libe3.hpp>

namespace e3config {

/*
 * Radio geometry — MUST match the running gNB.
 *
 * This used to be four `constexpr` values in e3sm_shm_writer.h, with a comment
 * requiring lockstep updates with the dApp. Two problems with that: it needed a
 * recompile, and it was already inconsistent — `--num-prbs` was accepted on the
 * command line and fed only the eCPRI PRB filter, while ShmIqWriter kept sizing
 * rows from `constexpr kShmPrbsPerSymbol = 273`.
 *
 * Treated as a BOOTSTRAP, not as truth: the region has to exist and be sized
 * before the codelet can be loaded, so something must be declared up front. But
 * the RAN reports its actual geometry in every slot (jbpf_ran_slot_ctx carries
 * nof_ports / nof_symbols / nof_subcarriers), so E3SMLayer1 validates these
 * values against the first slot it sees and refuses on mismatch. See
 * validate_against_ran().
 */
struct RadioGeometry {
    uint16_t nof_ports    = 4;   /* UL antenna ports */
    uint16_t nof_prbs     = 273; /* 100 MHz @ 30 kHz SCS */
    uint16_t nof_symbols  = 14;  /* per slot */
    uint16_t scs_khz      = 30;  /* numerology; also fixes slots-per-frame */

    /* 3GPP: 12 subcarriers per PRB, always. */
    static constexpr uint32_t kScPerPrb = 12;

    /* Complex sample sizes on each side of the conversion. */
    static constexpr uint32_t kBytesPerCbf16 = 4; /* bf16 real + bf16 imag */
    static constexpr uint32_t kBytesPerFp16Pair = 4; /* fp16 real + fp16 imag */

    uint32_t nof_subcarriers() const { return static_cast<uint32_t>(nof_prbs) * kScPerPrb; }

    /* uint16 values per antenna in the fp16 row = 2 (I,Q) per subcarrier. */
    uint32_t u16_per_ant() const { return static_cast<uint32_t>(nof_symbols) * nof_subcarriers() * 2u; }

    /* Whole-row uint16 count — this is what SharedMemoryHeader::num_fh_samples
     * advertises, and what the dApp multiplies by sizeof(int16_t). */
    uint32_t num_fh_samples() const { return static_cast<uint32_t>(nof_ports) * u16_per_ant(); }

    uint32_t row_bytes() const { return num_fh_samples() * 2u; }

    /* Bytes one antenna occupies in the codelet's cbf16 source blob. */
    uint32_t cbf16_bytes_per_ant() const
    {
        return static_cast<uint32_t>(nof_symbols) * nof_subcarriers() * kBytesPerCbf16;
    }

    /* Slots per 10 ms radio frame. 20 at 30 kHz, 10 at 15 kHz. This is the range
     * of ocudu's slot_point::slot_index(), i.e. of SlotSample::slot_id — NOT
     * slots-per-subframe. Getting this wrong is what makes a slot filter select
     * nothing. */
    uint32_t slots_per_frame() const { return 10u * (scs_khz / 15u); }

    /* Human-readable, for logs and mismatch diagnostics. */
    std::string describe() const;

    /* Reject nonsense before it becomes a wrong row stride. */
    bool validate(std::string& err) const;
};

/*
 * TODO: leave only one mode.
 * Which process converts the resource grid and writes the fp16 rows.
 * Mode: Controller/gnb
 * Either way the controller OWNS the region: it creates, sizes, zero-fills and
 * headers it, and tears it down. The helper only ever attaches.
 */
enum class ShmWriter {
    Controller,
    Gnb,
};

const char* to_string(ShmWriter w);

struct ShmConfig {
    std::string name       = "/e3_ran_buffers";
    size_t      size_bytes = static_cast<size_t>(1) << 30;

    /* Default is Gnb because it is the only mode the SHIPPED CODELET supports:
     * uplink_slot_collect.c publishes a ~64 B e3_slot_desc and calls the gNB-side
     * helper, so no IQ ever crosses the jbpf ring. Controller mode needs a
     * copy-based codelet, which does not currently exist in this repo — selecting
     * it yields "Slot blob too small: 0 bytes" on every slot. */
    ShmWriter writer = ShmWriter::Gnb;

    /*
     * bf16 -> fp16 scale. Was the E3_CBF16_SCALE environment variable.
     *
     * Promoted into the config because it is now part of a cross-process
     * contract: it is pushed down to the gNB-side publish helper via
     * e3_shm_cfg, and if the two writers ever disagreed on it the rows would
     * differ with no error at all — bf16 and fp16 are both 2 bytes, so a wrong
     * scale is silently wrong data rather than a failure.
     */
    float cbf16_scale = 1.0f;
};

struct JbpfConfig {
    std::string ipc_name          = "e3_controller";
    std::string run_path          = "/dev/shm";
    size_t      mem_size_bytes    = static_cast<size_t>(1) << 30;
    std::string lcm_socket_path   = "/tmp/jbpf/jbpf_lcm_ipc";
    std::string codelet_base_path = "";
};

struct E3Config {
    libe3::EncodingFormat   encoding        = libe3::EncodingFormat::ASN1;
    libe3::E3LinkLayer      link_layer      = libe3::E3LinkLayer::ZMQ;
    libe3::E3TransportLayer transport       = libe3::E3TransportLayer::TCP;
    uint16_t                setup_port      = 9990;
    uint16_t                publisher_port  = 9991;
    uint16_t                subscriber_port = 9999;
};

struct ThreadConfig {
    int poll_core        = -1;
    int worker_core      = -1;
    int publisher_core   = -1;
    int poll_interval_us = 100;
};

struct LoggingConfig {
    /* Throttled drop-accounting CSV (<=1 row/s, cumulative). Empty disables it.
     * This is the only file the L1-KPM SM writes; per-slot stage timing is
     * latrec's job -- see src/e3sm/l1_kpm/l1_kpm_trace.h. */
    std::string drops_log_path;

    /* Per-slot stage CSV for the LEGACY eCPRI Service Model (RF=1) only.
     * Empty disables it.
     *
     * The slot-path SM (RF=2) no longer has one -- see latrec_dir below. This
     * one is left as it was: it is on a different data path, and it has the same
     * per-slot-flush problem, so it should get the same treatment when that path
     * is next touched. */
    std::string spectrum_stats_log_path;

    /* Where latrec writes its per-thread stage-record rings. Empty leaves the
     * directory compiled into libe3 (LATREC_DEFAULT_DIR, /tmp/latrec unless the
     * build overrode it).
     *
     * Placement only, NOT a switch: whether anything is recorded is decided
     * when libe3 is built, by -DLIBE3_ENABLE_LATREC (./build.sh --latrec). A
     * build without it ignores this entirely. */
    std::string latrec_dir;
};

struct ControllerConfig {
    RadioGeometry radio;
    ShmConfig     shm;
    JbpfConfig    jbpf;
    E3Config      e3;
    ThreadConfig  threads;
    LoggingConfig logging;

    /*
     * Legacy single-slot filter, kept for compatibility.
     *
     * Absolute slot index within a 10 ms frame (0..slots_per_frame()-1). -1
     * disables. NOTE this filters in the controller, AFTER the RAN has already
     * converted and published the slot — it saves the controller work and the
     * RAN nothing. Workstream F's codelet-side slot_mask supersedes it by making
     * the same decision before any data moves; fold this into that when F lands.
     */
    int target_slot = -1;
};

/*
 * Parse a YAML file. Returns false and fills `err` on a parse error, an unknown
 * key, or a geometry that fails validate(). Unknown keys are an ERROR rather
 * than a warning: a silently ignored `nof_ports` is exactly the class of failure
 * this file exists to prevent.
 */
bool load_config(const std::string& path, ControllerConfig& out, std::string& err);

/* Emit the effective configuration, so what the controller actually used is in
 * the log next to the run it produced. */
void print_config(const ControllerConfig& cfg);

/*
 * Compare the configured geometry against what the RAN reports in a slot.
 *
 * Returns true when they agree. On disagreement fills `err` with both sides.
 *
 * The two mismatch directions are NOT symmetric, which is why this exists:
 *
 *   - config UNDER-declares (says 2 ports, gNB sends 4): already safe. The
 *     gNB-side helper refuses src_len > row_bytes and the codelet reports
 *     E3_SLOT_FLAG_TRUNCATED, so it is loud.
 *
 *   - config OVER-declares (says 8 ports, gNB sends 4): silently misleading.
 *     The helper zero-fills the unused antennas, so the dApp reads 4 real ports
 *     plus 4 of silence and cannot tell without inspecting nof_ports. This is
 *     the case that produces a plausible-looking wrong spectrum, and the only
 *     one that needs an explicit check.
 */
bool validate_against_ran(const RadioGeometry& cfg, uint16_t ran_nof_ports, uint16_t ran_nof_symbols,
                          uint32_t ran_nof_subcarriers, std::string& err);

} // namespace e3config

#endif /* E3_CONFIG_H */
