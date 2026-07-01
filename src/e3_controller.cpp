/*
 * e3_controller.cpp
 *
 * E3Controller: a standalone daemon that acts as a jbpf IPC primary process
 * and an E3 agent. It connects to ocudu's jbpf shared memory to receive
 * codelet output data, and uses libe3 to expose the data as E3
 * Service Model indications to subscribed dApps.
 *
 * Architecture:
 *   - A single JbpfDispatcher polls jbpf shared memory in the main loop
 *   - Service models register their stream_ids with the dispatcher
 *   - The dispatcher routes buffers to the right SM and releases them
 *
 *
 * The IPC primary must start BEFORE ocudu (IPC secondary).
 */

#include <iostream>
#include <memory>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <getopt.h>
#include <atomic>
#include <pthread.h>
#include <sched.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#include <libe3/libe3.hpp>

extern "C" {
#include "jbpf_io.h"
#include "jbpf_io_channel.h"
#include "jbpf_common.h"
#include "jbpf_mem_mgmt.h"
}

#include "jbpf_dispatcher.h"
#include "e3sm/sm_spectrum/e3sm_spectrum.h"
#include "e3sm/l1_kpm/e3sm_layer_1.h"
#include "e3sm/iq_pipeline.h"
#include "e3sm/slot_iq_pipeline.h"

// ---- Global state ----

static std::atomic<bool> g_running{true};

// ---- Signal handler ----

static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        std::printf("\n[E3Controller] Received signal %d, shutting down...\n", signo);
        g_running = false;
    }
}

// // ---- APER self-test ----
// // Tests that asn1c can encode and decode an E3-PDU at startup.
// // If this crashes, the issue is in the asn1c build/version.

// extern "C" {
// #include "E3-PDU.h"
// #include "E3-SetupRequest.h"
// #include "aper_encoder.h"
// #include "aper_decoder.h"
// #include "OCTET_STRING.h"
// }

// static bool aper_self_test()
// {
//     std::printf("[SelfTest] Running APER encode/decode self-test...\n");

//     // 1. Encode a test SetupRequest PDU using asn1c APER
//     E3_PDU_t pdu;
//     memset(&pdu, 0, sizeof(pdu));
//     pdu.id = 1;
//     pdu.msg.present = E3_PDU__msg_PR_setupRequest;
//     pdu.msg.choice.setupRequest = (E3_SetupRequest_t*)calloc(1, sizeof(E3_SetupRequest_t));
//     OCTET_STRING_fromBuf(&pdu.msg.choice.setupRequest->e3apProtocolVersion, "1.0.0", 5);
//     OCTET_STRING_fromBuf(&pdu.msg.choice.setupRequest->dAppName, "SelfTest", 8);
//     OCTET_STRING_fromBuf(&pdu.msg.choice.setupRequest->dAppVersion, "0.0.1", 5);
//     OCTET_STRING_fromBuf(&pdu.msg.choice.setupRequest->vendor, "Test", 4);

//     uint8_t buf[512];
//     asn_enc_rval_t enc = aper_encode_to_buffer(&asn_DEF_E3_PDU, NULL, &pdu, buf, sizeof(buf));
//     ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_E3_PDU, &pdu);

//     if (enc.encoded < 0) {
//         std::printf("[SelfTest] FAIL: APER encode returned %zd\n", enc.encoded);
//         return false;
//     }
//     size_t nbytes = (enc.encoded + 7) / 8;
//     std::printf("[SelfTest] APER encoded %zu bytes (%zd bits): ", nbytes, enc.encoded);
//     for (size_t i = 0; i < nbytes; i++) std::printf("%02x", buf[i]);
//     std::printf("\n");

//     // 2. Decode it back
//     E3_PDU_t *dec_pdu = NULL;
//     asn_dec_rval_t dec = aper_decode(NULL, &asn_DEF_E3_PDU, (void**)&dec_pdu, buf, nbytes, 0, 0);
//     if (dec.code != RC_OK) {
//         std::printf("[SelfTest] FAIL: APER decode returned code=%d consumed=%zu\n", dec.code, dec.consumed);
//         if (dec_pdu) ASN_STRUCT_FREE(asn_DEF_E3_PDU, dec_pdu);
//         return false;
//     }
//     std::printf("[SelfTest] APER decode OK: id=%ld, msg.present=%d, consumed=%zu bits\n",
//                 dec_pdu->id, dec_pdu->msg.present, dec.consumed);
//     ASN_STRUCT_FREE(asn_DEF_E3_PDU, dec_pdu);

//     // 3. Now test decoding the exact bytes that asn1tools "per" codec produces
//     // (the 40-byte setup request from the dApp)
//     uint8_t dapp_data[] = {
//         0x00, 0x00, 0x00, 0x05, 0x31, 0x2e, 0x30, 0x2e,
//         0x30, 0x0f, 0x53, 0x70, 0x65, 0x63, 0x74, 0x72,
//         0x75, 0x6d, 0x53, 0x68, 0x61, 0x72, 0x69, 0x6e,
//         0x67, 0x05, 0x31, 0x2e, 0x30, 0x2e, 0x30, 0x08,
//         0x57, 0x69, 0x6e, 0x65, 0x73, 0x4c, 0x61, 0x62
//     };
//     E3_PDU_t *dapp_pdu = NULL;
//     asn_dec_rval_t dapp_dec = aper_decode(NULL, &asn_DEF_E3_PDU,
//                                           (void**)&dapp_pdu, dapp_data, sizeof(dapp_data), 0, 0);
//     if (dapp_dec.code != RC_OK) {
//         std::printf("[SelfTest] FAIL: dApp data APER decode returned code=%d consumed=%zu\n",
//                     dapp_dec.code, dapp_dec.consumed);
//         if (dapp_pdu) ASN_STRUCT_FREE(asn_DEF_E3_PDU, dapp_pdu);
//         return false;
//     }
//     std::printf("[SelfTest] dApp data decode OK: id=%ld, msg.present=%d\n",
//                 dapp_pdu->id, dapp_pdu->msg.present);
//     ASN_STRUCT_FREE(asn_DEF_E3_PDU, dapp_pdu);

//     std::printf("[SelfTest] PASS: All APER encode/decode tests passed.\n");
//     return true;
// }

// ---- Configuration ----

struct E3ControllerConfig {
    std::string ipc_name = "e3_controller";
    std::string run_path = "/dev/shm";
    size_t mem_size = JBPF_HUGEPAGE_SIZE_1GB;
    int poll_interval_us = 100;  // microseconds between polls (ignored when poll_core >= 0)
    int poll_core = -1;          // CPU to pin the polling thread to (-1 = no pinning, sleep-based)
    int worker_core = -1;        // CPU to pin the SM worker thread to (-1 = no pinning)
    int publisher_core = -1;     // CPU to pin libe3's publisher_thread_ to (-1 = no pinning)
    uint16_t num_prbs = 106;     // expected number of PRBs per symbol
    std::string lcm_socket_path = "/tmp/jbpf/jbpf_lcm_ipc";  // LCM IPC socket for codelet loading
    std::string codelet_base_path;  // base dir for codelet binaries (empty = no auto-loading)

    // Wire encoding for the (single) E3 channel. The controller serves exactly
    // one encoding at a time; dApps must speak the same one. ASN.1 (APER) is
    // the O-RAN default; JSON matches the NVIDIA_L1 cuBB convention used by the
    // adaptive_cpu dApp.
    //
    // libe3 is built with BOTH encoders (LIBE3_ENABLE_ASN1 + LIBE3_ENABLE_JSON),
    // so this is a pure runtime choice — the encoder factory picks the matching
    // encoder. (If libe3 was instead built with only one encoder, this must
    // match it or the outbound encoder rejects the PDU.)
    libe3::EncodingFormat encoding = libe3::EncodingFormat::ASN1;

    // Link layer libe3 uses to move E3AP PDUs. ZMQ is the default (and the only
    // one exercised here); POSIX selects raw sockets.
    libe3::E3LinkLayer link_layer = libe3::E3LinkLayer::ZMQ;

    // Transport under the link layer. TCP matches what most dApps expect
    // (host+ports); IPC uses UNIX-domain sockets for a same-host dApp; SCTP is
    // the O-RAN standard.
    libe3::E3TransportLayer transport = libe3::E3TransportLayer::TCP;

    // E3 channel ports (libe3 defaults; for a JSON/cuBB dApp use 5555/5556/5557).
    uint16_t setup_port = 9990;
    uint16_t publisher_port = 9991;
    uint16_t subscriber_port = 9999;

    // POSIX SHM segment for IQ data. Layout matches SharedMemoryHeader in
    // spear-aerial-sample-apps' e3_manager.h so the dApp reads our data with no
    // code change.
    std::string shm_name = "/e3_ran_buffers";
    size_t shm_size = static_cast<size_t>(1) << 30;  // 1 GiB

    // --- Timing logs (all disabled unless a path is given) ---
    // Per-slot RAN-side stage CSV written by E3SMLayer1 (gnb→codelet→dispatch→
    // handler durations + shm/encode/emit costs). Empty = no log.
    std::string stats_log_path;

    // Optional single-slot filter. The value is the absolute slot index within
    // a 10 ms frame (0..19 for 30 kHz SCS, 0..9 for 15 kHz). UL symbols whose
    // computed (subframe_id*2 + slot_id) doesn't match are dropped at the top
    // of process_sample_json — no BFP decompress, no slot accumulation. -1
    // (default) disables the filter and every UL slot is forwarded.
    // NOTE: assumes 30 kHz SCS (2 slots/subframe). For a different numerology
    // the divider would need to change.
    int target_slot = -1;
};

static void print_usage(const char* prog)
{
    std::printf("Usage: %s [options]\n"
                "Options:\n"
                "  --ipc-name <name>     IPC shared memory name (default: e3_controller)\n"
                "  --run-path <path>     jbpf run path (default: /dev/shm)\n"
                "  --mem-size <bytes>    Shared memory size in bytes (default: 1GB)\n"
                "  --poll-interval <us>  Poll interval in microseconds (default: 100; ignored if --poll-core is set)\n"
                "  --poll-core <cpu>     Pin the polling thread to <cpu> and busy-poll (default: -1, no pinning)\n"
                "  --worker-core <cpu>   Pin the SM worker (decompress/encode/emit) to <cpu> (default: -1, no pinning)\n"
                "  --publisher-core <cpu> Pin libe3's publisher thread (outer encode + ZMQ send) to <cpu> (default: -1, no pinning)\n"
                "  --num-prbs <n>        Expected number of PRBs per symbol (default: 106)\n"
                "  --lcm-socket <path>   LCM IPC socket path for codelet loading (default: /tmp/jbpf/jbpf_lcm_ipc)\n"
                "  --codelet-path <dir>  Base directory for codelet binaries (enables auto-loading)\n"
                "  --encoding <name>     Wire encoding for the E3 channel: 'asn1' (default) or\n"
                "                          'json'. Must match how libe3 was compiled.\n"
                "  --link-layer <name>   Link layer: 'zmq' (default) or 'posix'.\n"
                "  --transport <name>    Transport layer: 'tcp' (default), 'ipc', or 'sctp'.\n"
                "  --setup-port <p>      E3 channel setup REP port (default: 9990)\n"
                "  --publisher-port <p>  E3 channel indication PUB port (default: 9991)\n"
                "  --subscriber-port <p> E3 channel control SUB port    (default: 9999)\n"
                "  --shm-name <name>     POSIX SHM name for IQ data (default: /e3_ran_buffers)\n"
                "  --shm-size <bytes>    POSIX SHM size in bytes (default: 1GiB)\n"
                "  --target-slot <N>     Forward ONLY UL slot N (absolute slot within a frame,\n"
                "                          0..19 for 30 kHz SCS). Assumes 2 slots/subframe.\n"
                "                          Default -1 = forward every UL slot.\n"
                "  --stats-log <path>    Write the per-slot RAN-side stage CSV to <path>\n"
                "                          (gnb/codelet/dispatch/handler + shm/encode/emit).\n"
                "                          Default: disabled.\n"
                "  --help                Show this help\n",
                prog);
}

static E3ControllerConfig parse_args(int argc, char** argv)
{
    E3ControllerConfig config;

    // Long-only options (no short flag) get codes >= 256 so they don't collide
    // with single-char optstring values.
    enum LongOpt {
        OPT_TRANSPORT = 256,
        OPT_ENCODING,
        OPT_LINK_LAYER,
        OPT_SETUP_PORT,
        OPT_PUBLISHER_PORT,
        OPT_SUBSCRIBER_PORT,
        OPT_SHM_NAME,
        OPT_SHM_SIZE,
        OPT_TARGET_SLOT,
        OPT_STATS_LOG,
    };

    static struct option long_options[] = {
        {"ipc-name",        required_argument, nullptr, 'n'},
        {"run-path",        required_argument, nullptr, 'r'},
        {"mem-size",        required_argument, nullptr, 'm'},
        {"poll-interval",   required_argument, nullptr, 'p'},
        {"poll-core",       required_argument, nullptr, 'C'},
        {"worker-core",     required_argument, nullptr, 'W'},
        {"publisher-core",  required_argument, nullptr, 'P'},
        {"num-prbs",        required_argument, nullptr, 'b'},
        {"lcm-socket",      required_argument, nullptr, 'l'},
        {"codelet-path",    required_argument, nullptr, 'c'},
        {"encoding",        required_argument, nullptr, OPT_ENCODING},
        {"link-layer",      required_argument, nullptr, OPT_LINK_LAYER},
        {"transport",       required_argument, nullptr, OPT_TRANSPORT},
        {"setup-port",      required_argument, nullptr, OPT_SETUP_PORT},
        {"publisher-port",  required_argument, nullptr, OPT_PUBLISHER_PORT},
        {"subscriber-port", required_argument, nullptr, OPT_SUBSCRIBER_PORT},
        {"shm-name",        required_argument, nullptr, OPT_SHM_NAME},
        {"shm-size",        required_argument, nullptr, OPT_SHM_SIZE},
        {"target-slot",     required_argument, nullptr, OPT_TARGET_SLOT},
        {"stats-log",       required_argument, nullptr, OPT_STATS_LOG},
        {"help",            no_argument,       nullptr, 'h'},
        {nullptr,           0,                 nullptr,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:r:m:p:C:W:P:b:l:c:h", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'n':
            config.ipc_name = optarg;
            break;
        case 'r':
            config.run_path = optarg;
            break;
        case 'm':
            config.mem_size = std::strtoull(optarg, nullptr, 0);
            break;
        case 'p':
            config.poll_interval_us = std::atoi(optarg);
            break;
        case 'C':
            config.poll_core = std::atoi(optarg);
            break;
        case 'W':
            config.worker_core = std::atoi(optarg);
            break;
        case 'P':
            config.publisher_core = std::atoi(optarg);
            break;
        case 'b':
            config.num_prbs = static_cast<uint16_t>(std::atoi(optarg));
            break;
        case 'l':
            config.lcm_socket_path = optarg;
            break;
        case 'c':
            config.codelet_base_path = optarg;
            break;
        case OPT_ENCODING: {
            std::string v(optarg);
            if (v == "asn1" || v == "ASN1" || v == "aper" || v == "APER") {
                config.encoding = libe3::EncodingFormat::ASN1;
            } else if (v == "json" || v == "JSON") {
                config.encoding = libe3::EncodingFormat::JSON;
            } else {
                std::fprintf(stderr, "[E3Controller] Unknown --encoding value '%s' (expected asn1 or json)\n",
                             optarg);
                std::exit(1);
            }
            break;
        }
        case OPT_LINK_LAYER: {
            std::string v(optarg);
            if (v == "zmq" || v == "ZMQ") {
                config.link_layer = libe3::E3LinkLayer::ZMQ;
            } else if (v == "posix" || v == "POSIX") {
                config.link_layer = libe3::E3LinkLayer::POSIX;
            } else {
                std::fprintf(stderr, "[E3Controller] Unknown --link-layer value '%s' (expected zmq or posix)\n",
                             optarg);
                std::exit(1);
            }
            break;
        }
        case OPT_TRANSPORT: {
            std::string v(optarg);
            if (v == "tcp" || v == "TCP") {
                config.transport = libe3::E3TransportLayer::TCP;
            } else if (v == "ipc" || v == "IPC") {
                config.transport = libe3::E3TransportLayer::IPC;
            } else if (v == "sctp" || v == "SCTP") {
                config.transport = libe3::E3TransportLayer::SCTP;
            } else {
                std::fprintf(stderr, "[E3Controller] Unknown --transport value '%s' (expected tcp, ipc, or sctp)\n",
                             optarg);
                std::exit(1);
            }
            break;
        }
        case OPT_SETUP_PORT:
            config.setup_port = static_cast<uint16_t>(std::atoi(optarg));
            break;
        case OPT_PUBLISHER_PORT:
            config.publisher_port = static_cast<uint16_t>(std::atoi(optarg));
            break;
        case OPT_SUBSCRIBER_PORT:
            config.subscriber_port = static_cast<uint16_t>(std::atoi(optarg));
            break;
        case OPT_SHM_NAME:
            config.shm_name = optarg;
            break;
        case OPT_SHM_SIZE:
            config.shm_size = std::strtoull(optarg, nullptr, 0);
            break;
        case OPT_TARGET_SLOT:
            config.target_slot = std::atoi(optarg);
            break;
        case OPT_STATS_LOG:
            config.stats_log_path = optarg;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            std::exit(opt == 'h' ? 0 : 1);
        }
    }

    return config;
}

// ---- Main ----

int main(int argc, char** argv)
{
    E3ControllerConfig config = parse_args(argc, argv);

    // E3 agent configuration — single encoding on a single channel.
    std::string ran_id = "ocudu-janus";

    libe3::E3Config agentConfig;
    agentConfig.ran_identifier = ran_id;
    agentConfig.link_layer = config.link_layer;
    agentConfig.transport_layer = config.transport;
    // One encoding, chosen via --encoding. dApps must speak the same encoding
    // and connect to setup/publisher/subscriber ports below.
    agentConfig.encoding        = config.encoding;
    agentConfig.setup_port      = config.setup_port;
    agentConfig.publisher_port  = config.publisher_port;
    agentConfig.subscriber_port = config.subscriber_port;
    agentConfig.log_level = 5;  // 0=none, 1=err, 2=warn, 3=info, 4=debug, 5=trace
    // Pin libe3's I/O threads (the RAN outbound loop in particular — it does
    // the outer encode + zmq_send) to a dedicated core if requested. Without
    // this the publisher gets scheduled out under load and queue_us in the
    // publisher-stage CSV climbs into hundreds of µs.
    if (config.publisher_core >= 0) {
        agentConfig.io_thread_affinity = config.publisher_core;
    }

    auto encoding_name = [](libe3::EncodingFormat e) {
        return e == libe3::EncodingFormat::JSON ? "JSON" : "ASN.1 (APER)";
    };

    std::cout << "=============================================\n";
    std::cout << "E3 Agent Configuration (single-encoding):\n"
              << "  RAN ID:      " << ran_id << "\n"
              << "  Encoding:    " << encoding_name(config.encoding) << "\n"
              << "  Link layer:  " << libe3::link_layer_to_string(config.link_layer) << "\n"
              << "  Transport:   " << libe3::transport_layer_to_string(config.transport) << "\n"
              << "  Channel:     setup=" << config.setup_port
              << " publisher=" << config.publisher_port
              << " subscriber=" << config.subscriber_port << "\n"
              << "  Stats log:   "
              << (config.stats_log_path.empty() ? "(disabled)" : config.stats_log_path) << "\n\n";

    libe3::E3Agent agent(std::move(agentConfig));

    std::printf("=============================================\n");
    std::printf("  E3Controller Codelet Configuration\n");
    std::printf("=============================================\n");
    std::printf("  IPC name:       %s\n", config.ipc_name.c_str());
    std::printf("  Run path:       %s\n", config.run_path.c_str());
    std::printf("  Memory size:    %zu bytes\n", config.mem_size);
    std::printf("  Poll interval:  %d us%s\n", config.poll_interval_us,
                config.poll_core >= 0 ? " (ignored — busy poll on dedicated core)" : "");
    std::printf("  Poll core:      %s\n",
                config.poll_core >= 0 ? std::to_string(config.poll_core).c_str() : "(none — sleep-based)");
    std::printf("  Worker core:    %s\n",
                config.worker_core >= 0 ? std::to_string(config.worker_core).c_str() : "(none)");
    std::printf("  Publisher core: %s\n",
                config.publisher_core >= 0 ? std::to_string(config.publisher_core).c_str() : "(none)");
    std::printf("  Num PRBs:       %u\n", config.num_prbs);
    std::printf("  LCM socket:     %s\n", config.lcm_socket_path.c_str());
    std::printf("  Codelet path:   %s\n",
                config.codelet_base_path.empty() ? "(none — auto-loading disabled)" : config.codelet_base_path.c_str());
    std::printf("=============================================\n\n");

    // Install signal handlers
    if (signal(SIGINT, signal_handler) == SIG_ERR ||
        signal(SIGTERM, signal_handler) == SIG_ERR) {
        std::perror("[E3Controller] Failed to install signal handlers");
        return 1;
    }

    // Initialize jbpf IO as IPC primary
    struct jbpf_io_config io_config = {};
    io_config.type = JBPF_IO_IPC_PRIMARY;

    std::strncpy(io_config.jbpf_path, config.run_path.c_str(), JBPF_RUN_PATH_LEN - 1);
    io_config.jbpf_path[JBPF_RUN_PATH_LEN - 1] = '\0';

    std::strncpy(io_config.jbpf_namespace, JBPF_DEFAULT_NAMESPACE, JBPF_NAMESPACE_LEN - 1);
    io_config.jbpf_namespace[JBPF_NAMESPACE_LEN - 1] = '\0';

    std::strncpy(io_config.ipc_config.addr.jbpf_io_ipc_name,
                 config.ipc_name.c_str(),
                 JBPF_IO_IPC_MAX_NAMELEN - 1);
    io_config.ipc_config.addr.jbpf_io_ipc_name[JBPF_IO_IPC_MAX_NAMELEN - 1] = '\0';

    io_config.ipc_config.mem_cfg.memory_size = config.mem_size;

    std::printf("[E3Controller] Initializing jbpf IO (IPC primary)...\n");
    auto* io_ctx = jbpf_io_init(&io_config);
    if (!io_ctx) {
        std::fprintf(stderr, "[E3Controller] ERROR: Failed to initialize jbpf IO.\n"
                             "  Make sure no other IPC primary is running with the same name.\n");
        return 1;
    }
    std::printf("[E3Controller] jbpf IO initialized successfully.\n");

    // Register this thread for jbpf IO operations
    if (!jbpf_io_register_thread()) {
        std::fprintf(stderr, "[E3Controller] ERROR: Failed to register IO thread.\n");
        jbpf_io_stop();
        return 1;
    }
    
    // Initialize the E3 agent
    auto init_result = agent.init();
    if (init_result != libe3::ErrorCode::SUCCESS && 
        init_result != libe3::ErrorCode::ALREADY_INITIALIZED) {
        std::cerr << "Failed to initialize agent: " 
                  << libe3::error_code_to_string(init_result) << "\n";
        return 1;
    }

    // Create the central dispatcher — routes jbpf buffers to service models
    JbpfDispatcher dispatcher(io_ctx);

    // Two data-plane pipelines, both constructed eagerly but lazy-started
    // by their consumer SMs on first subscription:
    //
    //   - IqPipeline:              ecpri_iq_samples codelet, BFP-9
    //     decompress, per-section output. Consumed by E3SMSpectrum (RF=1),
    //     which FFT-pads + APER-encodes Spectrum-IQDataIndication. Lazy-
    //     started by libe3 on the first dApp subscription to RF=1.
    //
    //   - SlotIqPipeline (new):    uplink_slot_samples codelet, hooks
    //     ocudu's capture_uplink_slot, per-slot cbf16_t output. Consumed
    //     by E3SMLayer1 (RF=2). Lazy-started by libe3 on the first dApp
    //     subscription to RF=2.
    e3sm_pipeline::IqPipeline::Config pipeline_cfg;
    pipeline_cfg.lcm_socket_path    = config.lcm_socket_path;
    pipeline_cfg.codelet_base_path  = config.codelet_base_path;
    pipeline_cfg.expected_num_prbu  = config.num_prbs;
    pipeline_cfg.worker_core        = config.worker_core;
    e3sm_pipeline::IqPipeline iq_pipeline(dispatcher, io_ctx, std::move(pipeline_cfg));

    e3sm_pipeline::SlotIqPipeline::Config slot_pipeline_cfg;
    slot_pipeline_cfg.lcm_socket_path   = config.lcm_socket_path;
    slot_pipeline_cfg.codelet_base_path = config.codelet_base_path;
    slot_pipeline_cfg.worker_core       = config.worker_core;
    e3sm_pipeline::SlotIqPipeline slot_iq_pipeline(dispatcher, io_ctx,
                                                   std::move(slot_pipeline_cfg));

    // Register both service models.
    //  - RF=1 Spectrum SM: ecpri_iq IQ telemetry (Spectrum-IQDataIndication,
    //                       APER) via IqPipeline + PRB-blacklist control.
    //  - RF=2 L1-KPM SM:   slot-level SHM-pointer IQ indications in the
    //                       configured encoding. Wired to the SlotIqPipeline.
    libe3::ErrorCode sm_result;
    sm_result = agent.register_sm(std::make_unique<E3SMSpectrum>(iq_pipeline, agent));
    if (sm_result != libe3::ErrorCode::SUCCESS) {
        std::cerr << "Failed to register Spectrum SM (RF=1): "
                  << libe3::error_code_to_string(sm_result) << "\n";
        return 1;
    }
    sm_result = agent.register_sm(std::make_unique<E3SMLayer1>(
        slot_iq_pipeline, agent,
        config.shm_name, config.shm_size, config.target_slot,
        config.stats_log_path));
    if (sm_result != libe3::ErrorCode::SUCCESS) {
        std::cerr << "Failed to register L1-KPM SM (RF=2): "
                  << libe3::error_code_to_string(sm_result) << "\n";
        return 1;
    }

    // Both pipelines are deferred until first subscription. Each SM's start()
    // boots its pipeline (codelet load via LCM IPC + worker thread) on the
    // first dApp subscribe: E3SMSpectrum -> iq_pipeline (RF=1),
    // E3SMLayer1 -> slot_iq_pipeline (RF=2). Each SM's init() (called by
    // agent.register_sm above) already registered its consumer, so when the SM
    // is started later the callback is wired and no samples are missed.

    // Start the agent (libe3 spawns its setup/subscriber/publisher threads
    // and begins accepting dApp connections on both channels).
    libe3::ErrorCode start_result = agent.start();
    if (start_result != libe3::ErrorCode::SUCCESS) {
        std::cerr << "Failed to start agent: "
                  << libe3::error_code_to_string(start_result) << "\n";
        return 1;
    }
    
    std::cout << "Agent started successfully\n";
    std::cout << "State: " << libe3::agent_state_to_string(agent.state()) << "\n";

    // // Run APER self-test before accepting connections
    // if (!aper_self_test()) {
    //     std::fprintf(stderr, "[E3Controller] APER self-test FAILED — aborting.\n");
    //     agent.stop();
    //     jbpf_io_stop();
    //     return 1;
    // }

    std::cout << "Press Ctrl+C to stop...\n\n";

    // Pin polling thread to a dedicated core (if requested) AFTER agent.start():
    // libe3 spawns its IO threads inside start() and they inherit the parent's
    // affinity at the time of spawn, so pinning here keeps them on the unpinned
    // set and reserves our core for the jbpf shared-memory drain.
    bool busy_poll = (config.poll_core >= 0);
    if (busy_poll) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(config.poll_core, &mask);
        if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
            std::fprintf(stderr,
                "[E3Controller] WARNING: failed to pin polling thread to core %d (%s); "
                "falling back to sleep-based polling\n",
                config.poll_core, std::strerror(errno));
            busy_poll = false;
        } else {
            std::printf("[E3Controller] Polling thread pinned to core %d (busy-poll)\n",
                        config.poll_core);
        }
    }

    // Main polling loop — the dispatcher routes buffers to registered SMs
    while (g_running) {
        dispatcher.poll();

        if (busy_poll) {
#if defined(__x86_64__) || defined(__i386__)
            _mm_pause();  // hyperthread-friendly hint
#endif
        } else if (config.poll_interval_us > 0) {
            usleep(config.poll_interval_us);
        }
    }

    // Cleanup: stop the agent (so no more dApp messages flow), then both
    // pipelines (each joins its worker, unregisters its dispatcher
    // stream, and unloads its codelet if it was started), then jbpf_io.
    // Order matters — jbpf_io_stop tears down the IPC channel the
    // dispatcher reads from. Both pipeline.stop() calls are idempotent
    // and safe if the pipeline was never started.
    agent.stop();
    std::printf("[E3Controller] Stopping slot IQ pipeline...\n");
    slot_iq_pipeline.stop();
    std::printf("[E3Controller] Stopping legacy IQ pipeline...\n");
    iq_pipeline.stop();
    std::printf("[E3Controller] Shutting down jbpf IO...\n");
    jbpf_io_stop();
    std::printf("[E3Controller] Stopped.\n");

    return 0;
}
