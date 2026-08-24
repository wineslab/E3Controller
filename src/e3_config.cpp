/*
 * e3_config.cpp — YAML loader for the E3Controller configuration.
 */

#include "e3_config.h"

#include <libe3/latrec.h>

#include <cstdio>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace e3config {
namespace {

/*
 * Unknown keys are an ERROR, not a warning.
 *
 * A typo'd `nof_port:` that is silently ignored leaves the controller sizing
 * rows for the default 4 ports while the operator believes they configured
 * something else — which is precisely the silent-geometry-mismatch failure this
 * config exists to eliminate. Better to refuse to start.
 */
bool
check_keys(const YAML::Node& node, const char* section, const std::set<std::string>& allowed, std::string& err)
{
    if (!node || !node.IsMap()) {
        return true;
    }
    for (const auto& kv : node) {
        const std::string k = kv.first.as<std::string>();
        if (allowed.find(k) == allowed.end()) {
            std::ostringstream os;
            os << "unknown key '" << k << "' in section '" << section << "'";
            err = os.str();
            return false;
        }
    }
    return true;
}

template <typename T>
void
get(const YAML::Node& n, const char* key, T& out)
{
    if (n && n[key]) {
        out = n[key].as<T>();
    }
}

bool
parse_encoding(const std::string& s, libe3::EncodingFormat& out, std::string& err)
{
    if (s == "asn1" || s == "ASN1" || s == "aper") {
        out = libe3::EncodingFormat::ASN1;
        return true;
    }
    if (s == "json" || s == "JSON") {
        out = libe3::EncodingFormat::JSON;
        return true;
    }
    err = "e3.encoding must be 'asn1' or 'json', got '" + s + "'";
    return false;
}

bool
parse_link_layer(const std::string& s, libe3::E3LinkLayer& out, std::string& err)
{
    if (s == "zmq" || s == "ZMQ") {
        out = libe3::E3LinkLayer::ZMQ;
        return true;
    }
    if (s == "posix" || s == "POSIX") {
        out = libe3::E3LinkLayer::POSIX;
        return true;
    }
    err = "e3.link_layer must be 'zmq' or 'posix', got '" + s + "'";
    return false;
}

bool
parse_transport(const std::string& s, libe3::E3TransportLayer& out, std::string& err)
{
    if (s == "tcp" || s == "TCP") {
        out = libe3::E3TransportLayer::TCP;
        return true;
    }
    if (s == "ipc" || s == "IPC") {
        out = libe3::E3TransportLayer::IPC;
        return true;
    }
    if (s == "sctp" || s == "SCTP") {
        out = libe3::E3TransportLayer::SCTP;
        return true;
    }
    err = "e3.transport must be 'tcp', 'ipc' or 'sctp', got '" + s + "'";
    return false;
}

bool
parse_writer(const std::string& s, ShmWriter& out, std::string& err)
{
    if (s == "controller") {
        out = ShmWriter::Controller;
        return true;
    }
    if (s == "gnb") {
        out = ShmWriter::Gnb;
        return true;
    }
    err = "shm.writer must be 'controller' or 'gnb', got '" + s + "'";
    return false;
}

} // namespace

const char*
to_string(ShmWriter w)
{
    return w == ShmWriter::Gnb ? "gnb" : "controller";
}

std::string
RadioGeometry::describe() const
{
    std::ostringstream os;
    os << nof_ports << " ports x " << nof_symbols << " sym x " << nof_subcarriers() << " subc (" << nof_prbs
       << " PRB, " << scs_khz << " kHz SCS)";
    return os.str();
}

bool
RadioGeometry::validate(std::string& err) const
{
    if (nof_ports == 0 || nof_ports > 8) {
        err = "radio.nof_ports must be 1..8";
        return false;
    }
    if (nof_prbs == 0 || nof_prbs > 275) {
        err = "radio.nof_prbs must be 1..275";
        return false;
    }
    if (nof_symbols != 12 && nof_symbols != 14) {
        err = "radio.nof_symbols must be 12 or 14";
        return false;
    }
    if (scs_khz != 15 && scs_khz != 30 && scs_khz != 60 && scs_khz != 120) {
        err = "radio.scs_khz must be 15, 30, 60 or 120";
        return false;
    }
    /*
     * The gNB-side convert loop processes 8 uint16 lanes per AVX2 iteration and
     * relies on there being no tail. 273 PRB x 14 sym x 2 gives 91,728 = 8 x
     * 11,466. A geometry that breaks that would silently take the scalar tail
     * path for the remainder, so refuse it here rather than discover it in the
     * PHY RX thread.
     */
    if ((u16_per_ant() % 8u) != 0u) {
        err = "radio geometry gives " + std::to_string(u16_per_ant()) +
              " uint16/antenna, which is not a multiple of 8 (required by the AVX2 convert)";
        return false;
    }
    return true;
}

bool
load_config(const std::string& path, ControllerConfig& out, std::string& err)
{
    /*
     * Start from defaults rather than from whatever the caller passed in.
     *
     * Keys absent from the YAML are left untouched by design (that is what makes
     * them optional), so without this reset a reused ControllerConfig would
     * inherit fields from a previous load — and on failure the caller would hold
     * a half-populated config that looks usable. Partially-applied configuration
     * is precisely the kind of silent wrongness this file exists to prevent.
     */
    out = ControllerConfig{};

    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const std::exception& e) {
        err = std::string("cannot load '") + path + "': " + e.what();
        return false;
    }

    if (!root.IsMap()) {
        err = "top level of '" + path + "' must be a mapping";
        return false;
    }

    if (!check_keys(root, "<root>", {"radio", "shm", "jbpf", "e3", "threads", "logging", "target_slot"}, err)) {
        return false;
    }

    bool lcm_explicit = false;

    try {
        /* ---- radio ---- */
        if (const auto n = root["radio"]) {
            if (!check_keys(n, "radio", {"nof_ports", "nof_prbs", "nof_symbols", "scs_khz"}, err)) {
                return false;
            }
            get(n, "nof_ports", out.radio.nof_ports);
            get(n, "nof_prbs", out.radio.nof_prbs);
            get(n, "nof_symbols", out.radio.nof_symbols);
            get(n, "scs_khz", out.radio.scs_khz);
        }

        /* ---- shm ---- */
        if (const auto n = root["shm"]) {
            if (!check_keys(n, "shm", {"name", "size_bytes", "cbf16_scale", "writer"}, err)) {
                return false;
            }
            get(n, "name", out.shm.name);
            get(n, "size_bytes", out.shm.size_bytes);
            get(n, "cbf16_scale", out.shm.cbf16_scale);
            if (n["writer"] && !parse_writer(n["writer"].as<std::string>(), out.shm.writer, err)) {
                return false;
            }
        }

        /* ---- jbpf ---- */
        if (const auto n = root["jbpf"]) {
            if (!check_keys(
                    n, "jbpf",
                    {"ipc_name", "run_path", "mem_size_bytes", "lcm_socket_path", "codelet_base_path"}, err)) {
                return false;
            }
            get(n, "ipc_name", out.jbpf.ipc_name);
            get(n, "run_path", out.jbpf.run_path);
            get(n, "mem_size_bytes", out.jbpf.mem_size_bytes);
            /* Derive from run_path unless explicitly set. The gNB composes
             * <jbpf_run_path>/<jbpf_namespace>/<jbpf_lcm_ipc_name>, so an
             * independent absolute default here (it used to be /tmp/...) drifts
             * from the gNB the moment run_path is anything but /tmp — which is
             * exactly what happens in the standard /dev/shm deployment. */
            if (n["lcm_socket_path"]) {
                out.jbpf.lcm_socket_path = n["lcm_socket_path"].as<std::string>();
                lcm_explicit = true;
            }
            get(n, "codelet_base_path", out.jbpf.codelet_base_path);
        }

        /* ---- e3 ---- */
        if (const auto n = root["e3"]) {
            if (!check_keys(n, "e3",
                            {"encoding", "link_layer", "transport", "setup_port", "publisher_port",
                             "subscriber_port"},
                            err)) {
                return false;
            }
            if (n["encoding"] && !parse_encoding(n["encoding"].as<std::string>(), out.e3.encoding, err)) {
                return false;
            }
            if (n["link_layer"] && !parse_link_layer(n["link_layer"].as<std::string>(), out.e3.link_layer, err)) {
                return false;
            }
            if (n["transport"] && !parse_transport(n["transport"].as<std::string>(), out.e3.transport, err)) {
                return false;
            }
            get(n, "setup_port", out.e3.setup_port);
            get(n, "publisher_port", out.e3.publisher_port);
            get(n, "subscriber_port", out.e3.subscriber_port);
        }

        /* ---- threads ---- */
        if (const auto n = root["threads"]) {
            if (!check_keys(n, "threads", {"poll_core", "worker_core", "publisher_core", "poll_interval_us"},
                            err)) {
                return false;
            }
            get(n, "poll_core", out.threads.poll_core);
            get(n, "worker_core", out.threads.worker_core);
            get(n, "publisher_core", out.threads.publisher_core);
            get(n, "poll_interval_us", out.threads.poll_interval_us);
        }

        /* ---- logging ---- */
        if (const auto n = root["logging"]) {
            if (!check_keys(n, "logging",
                            {"drops_log_path", "spectrum_stats_log_path", "latrec_dir"}, err)) {
                return false;
            }
            get(n, "drops_log_path", out.logging.drops_log_path);
            get(n, "spectrum_stats_log_path", out.logging.spectrum_stats_log_path);
            get(n, "latrec_dir", out.logging.latrec_dir);
        }

        get(root, "target_slot", out.target_slot);
    } catch (const std::exception& e) {
        err = std::string("error parsing '") + path + "': " + e.what();
        return false;
    }

    if (!lcm_explicit) {
        out.jbpf.lcm_socket_path = out.jbpf.run_path + "/jbpf/jbpf_lcm_ipc";
    }

    if (!out.radio.validate(err)) {
        return false;
    }

    /* The SHM region must hold at least one row per buffer, or the ring degenerates. */
    const size_t kHeaderBytes = 64;
    const size_t kNumBuffers  = 2;
    if (out.shm.size_bytes <= kHeaderBytes ||
        (out.shm.size_bytes - kHeaderBytes) / kNumBuffers < out.radio.row_bytes()) {
        std::ostringstream os;
        os << "shm.size_bytes " << out.shm.size_bytes << " is too small for " << kNumBuffers << " buffers of one "
           << out.radio.row_bytes() << "-byte row (" << out.radio.describe() << ")";
        err = os.str();
        return false;
    }

    if (!(out.shm.cbf16_scale > 0.0f)) {
        err = "shm.cbf16_scale must be > 0 (a zero or negative scale zeroes the row and "
              "silently breaks the dApp)";
        return false;
    }

    if (out.target_slot >= 0 && static_cast<uint32_t>(out.target_slot) >= out.radio.slots_per_frame()) {
        std::ostringstream os;
        os << "target_slot " << out.target_slot << " is outside 0.." << (out.radio.slots_per_frame() - 1)
           << " for " << out.radio.scs_khz << " kHz SCS — it would never match";
        err = os.str();
        return false;
    }

    return true;
}

void
print_config(const ControllerConfig& cfg)
{
    std::printf("E3Controller configuration:\n");
    std::printf("  radio:      %s\n", cfg.radio.describe().c_str());
    std::printf("              row = %u uint16 (%u bytes), %u slots/frame\n", cfg.radio.num_fh_samples(),
                cfg.radio.row_bytes(), cfg.radio.slots_per_frame());
    std::printf("  shm:        %s, %zu bytes, cbf16_scale=%g\n", cfg.shm.name.c_str(), cfg.shm.size_bytes,
                static_cast<double>(cfg.shm.cbf16_scale));
    std::printf("  row writer: %s%s\n", to_string(cfg.shm.writer),
                cfg.shm.writer == ShmWriter::Gnb
                    ? "  (gNB helper converts + writes; controller owns the region only)"
                    : "  (controller converts + writes)");
    std::printf("  jbpf:       ipc=%s run=%s lcm=%s\n", cfg.jbpf.ipc_name.c_str(), cfg.jbpf.run_path.c_str(),
                cfg.jbpf.lcm_socket_path.c_str());
    std::printf("  codelets:   %s\n",
                cfg.jbpf.codelet_base_path.empty() ? "(none — no auto-loading)"
                                                   : cfg.jbpf.codelet_base_path.c_str());
    std::printf("  e3:         ports %u/%u/%u\n", cfg.e3.setup_port, cfg.e3.publisher_port,
                cfg.e3.subscriber_port);
    std::printf("  threads:    poll=%d worker=%d publisher=%d\n", cfg.threads.poll_core, cfg.threads.worker_core,
                cfg.threads.publisher_core);
    if (cfg.target_slot >= 0) {
        std::printf("  target_slot: %d (controller-side filter)\n", cfg.target_slot);
    }
    if (!cfg.logging.drops_log_path.empty()) {
        std::printf("  drop log:   %s\n", cfg.logging.drops_log_path.c_str());
    }
    /* Three distinguishable states, worth telling apart on startup: not
     * compiled in, compiled in and writing where libe3 was built to write, or
     * compiled in and redirected. Otherwise "I set latrec_dir and got no rings"
     * is indistinguishable from a wrong path. */
#ifdef LIBE3_ENABLE_LATREC
    std::printf("  stage recs: enabled -> %s\n",
                cfg.logging.latrec_dir.empty()
                    ? LATREC_DEFAULT_DIR " (libe3 default)"
                    : cfg.logging.latrec_dir.c_str());
#else
    if (!cfg.logging.latrec_dir.empty()) {
        std::printf("  stage recs: IGNORED (%s): libe3 built without "
                    "-DLIBE3_ENABLE_LATREC\n", cfg.logging.latrec_dir.c_str());
    } else {
        std::printf("  stage recs: not compiled in\n");
    }
#endif
}

bool
validate_against_ran(const RadioGeometry& cfg, uint16_t ran_nof_ports, uint16_t ran_nof_symbols,
                     uint32_t ran_nof_subcarriers, std::string& err)
{
    std::ostringstream os;

    /* The dangerous direction: we advertise more antennas than the RAN delivers,
     * so the surplus is written as silence and the dApp cannot distinguish it
     * from a genuinely quiet antenna. */
    if (ran_nof_ports != 0 && cfg.nof_ports > ran_nof_ports) {
        os << "config declares " << cfg.nof_ports << " antenna ports but the RAN reports " << ran_nof_ports
           << ". The extra " << (cfg.nof_ports - ran_nof_ports)
           << " antenna(s) would be published as silence and the dApp could not tell. "
              "Set radio.nof_ports to " << ran_nof_ports << ".";
        err = os.str();
        return false;
    }

    if (ran_nof_symbols != 0 && cfg.nof_symbols != ran_nof_symbols) {
        os << "config declares " << cfg.nof_symbols << " symbols/slot but the RAN reports " << ran_nof_symbols
           << " — row stride would be wrong.";
        err = os.str();
        return false;
    }

    if (ran_nof_subcarriers != 0 && cfg.nof_subcarriers() != ran_nof_subcarriers) {
        os << "config declares " << cfg.nof_subcarriers() << " subcarriers (" << cfg.nof_prbs
           << " PRB) but the RAN reports " << ran_nof_subcarriers << " (" << (ran_nof_subcarriers / 12)
           << " PRB) — row stride would be wrong.";
        err = os.str();
        return false;
    }

    return true;
}

} // namespace e3config
