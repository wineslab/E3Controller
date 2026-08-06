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
#include <sys/stat.h>
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

#include "e3_config.h"
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
//
// The controller is configured by a single YAML file. This replaced 20
// command-line options; `--config` is deliberately the only argument, because
// two configuration mechanisms invite the two to disagree, and the radio
// geometry in that file has to be the one place it is stated.
//
// See include/e3_config.h for the schema and for why the geometry is treated as
// a bootstrap that gets validated against the RAN rather than as truth.

static void print_usage(const char* prog)
{
    std::printf("Usage: %s --config <file.yaml>\n"
                "\n"
                "  --config <file>   YAML configuration (required)\n"
                "  --help            this message\n"
                "\n"
                "See configs/e3_controller.yaml for a documented example.\n",
                prog);
}

int main(int argc, char** argv)
{
    // Single argument by design; see print_usage.
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unexpected argument '%s'\n\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    if (config_path.empty()) {
        std::fprintf(stderr, "--config is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    e3config::ControllerConfig config;
    std::string cfg_err;
    if (!e3config::load_config(config_path, config, cfg_err)) {
        std::fprintf(stderr, "[E3Controller] configuration error: %s\n", cfg_err.c_str());
        return 1;
    }
    e3config::print_config(config);

    /* The LCM socket is created by the gNB, so its absence usually means the gNB
     * is not up yet or the two disagree on the path. Say which, here, rather than
     * letting it surface later as a codeletset load failure whose message
     * ("is srsRAN running?") points at the wrong thing.
     *
     * Not fatal: the controller is the IPC PRIMARY and is supposed to start
     * first, so at this point the gNB legitimately may not exist yet. */
    {
        struct stat st;
        if (::stat(config.jbpf.lcm_socket_path.c_str(), &st) != 0) {
            std::fprintf(stderr,
                "[E3Controller] NOTE: LCM socket '%s' does not exist yet.\n"
                "  Codelet loading will fail until it appears. The gNB composes this path as\n"
                "  <jbpf_run_path>/<jbpf_namespace>/<jbpf_lcm_ipc_name> from its own YAML — with\n"
                "  the usual /dev/shm + jbpf + jbpf_lcm_ipc that is /dev/shm/jbpf/jbpf_lcm_ipc.\n"
                "  If the gNB is already running, jbpf.run_path here (%s) disagrees with it.\n",
                config.jbpf.lcm_socket_path.c_str(), config.jbpf.run_path.c_str());
        }
    }

    // E3 agent configuration — single encoding on a single channel.
    std::string ran_id = "ocudu-janus";

    libe3::E3Config agentConfig;
    agentConfig.ran_identifier = ran_id;
    agentConfig.link_layer = config.e3.link_layer;
    agentConfig.transport_layer = config.e3.transport;
    // One encoding, chosen via --encoding. dApps must speak the same encoding
    // and connect to setup/publisher/subscriber ports below.
    agentConfig.encoding        = config.e3.encoding;
    agentConfig.setup_port      = config.e3.setup_port;
    agentConfig.publisher_port  = config.e3.publisher_port;
    agentConfig.subscriber_port = config.e3.subscriber_port;
    agentConfig.log_level = 5;  // 0=none, 1=err, 2=warn, 3=info, 4=debug, 5=trace
    // Pin libe3's I/O threads (the RAN outbound loop in particular — it does
    // the outer encode + zmq_send) to a dedicated core if requested. Without
    // this the publisher gets scheduled out under load and it can be
    // scheduled out for hundreds of µs.
    if (config.threads.publisher_core >= 0) {
        agentConfig.io_thread_affinity = config.threads.publisher_core;
    }

    auto encoding_name = [](libe3::EncodingFormat e) {
        return e == libe3::EncodingFormat::JSON ? "JSON" : "ASN.1 (APER)";
    };

    // Derive the Spectrum SM's stats path from --stats-log by inserting
    // "_spectrum" before the extension (or appending it if there's no
    // extension). Keeps a single CLI flag while letting both SMs write
    // side-by-side without colliding on the same file. Derived here so the
    // banner below and the SM registration further down share the value.
    std::string spectrum_stats_log_path;
    if (!config.logging.stats_log_path.empty()) {
        const std::string& p = config.logging.stats_log_path;
        auto dot = p.find_last_of('.');
        auto sep = p.find_last_of('/');
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
            spectrum_stats_log_path = p.substr(0, dot) + "_spectrum" + p.substr(dot);
        } else {
            spectrum_stats_log_path = p + "_spectrum";
        }
    }

    std::cout << "=============================================\n";
    std::cout << "E3 Agent Configuration (single-encoding):\n"
              << "  RAN ID:      " << ran_id << "\n"
              << "  Encoding:    " << encoding_name(config.e3.encoding) << "\n"
              << "  Link layer:  " << libe3::link_layer_to_string(config.e3.link_layer) << "\n"
              << "  Transport:   " << libe3::transport_layer_to_string(config.e3.transport) << "\n"
              << "  Channel:     setup=" << config.e3.setup_port
              << " publisher=" << config.e3.publisher_port
              << " subscriber=" << config.e3.subscriber_port << "\n"
              << "  Stats log:   "
              << (config.logging.stats_log_path.empty() ? "(disabled)" : config.logging.stats_log_path) << "\n"
              << "  Spectrum stats: "
              << (spectrum_stats_log_path.empty() ? "(disabled)" : spectrum_stats_log_path)
              << "\n\n";

    libe3::E3Agent agent(std::move(agentConfig));

    std::printf("=============================================\n");
    std::printf("  E3Controller Codelet Configuration\n");
    std::printf("=============================================\n");
    std::printf("  IPC name:       %s\n", config.jbpf.ipc_name.c_str());
    std::printf("  Run path:       %s\n", config.jbpf.run_path.c_str());
    std::printf("  Memory size:    %zu bytes\n", config.jbpf.mem_size_bytes);
    std::printf("  Poll interval:  %d us%s\n", config.threads.poll_interval_us,
                config.threads.poll_core >= 0 ? " (ignored — busy poll on dedicated core)" : "");
    std::printf("  Poll core:      %s\n",
                config.threads.poll_core >= 0 ? std::to_string(config.threads.poll_core).c_str() : "(none — sleep-based)");
    std::printf("  Worker core:    %s\n",
                config.threads.worker_core >= 0 ? std::to_string(config.threads.worker_core).c_str() : "(none)");
    std::printf("  Publisher core: %s\n",
                config.threads.publisher_core >= 0 ? std::to_string(config.threads.publisher_core).c_str() : "(none)");
    std::printf("  Num PRBs:       %u\n", config.radio.nof_prbs);
    std::printf("  LCM socket:     %s\n", config.jbpf.lcm_socket_path.c_str());
    std::printf("  Codelet path:   %s\n",
                config.jbpf.codelet_base_path.empty() ? "(none — auto-loading disabled)" : config.jbpf.codelet_base_path.c_str());
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

    std::strncpy(io_config.jbpf_path, config.jbpf.run_path.c_str(), JBPF_RUN_PATH_LEN - 1);
    io_config.jbpf_path[JBPF_RUN_PATH_LEN - 1] = '\0';

    std::strncpy(io_config.jbpf_namespace, JBPF_DEFAULT_NAMESPACE, JBPF_NAMESPACE_LEN - 1);
    io_config.jbpf_namespace[JBPF_NAMESPACE_LEN - 1] = '\0';

    std::strncpy(io_config.ipc_config.addr.jbpf_io_ipc_name,
                 config.jbpf.ipc_name.c_str(),
                 JBPF_IO_IPC_MAX_NAMELEN - 1);
    io_config.ipc_config.addr.jbpf_io_ipc_name[JBPF_IO_IPC_MAX_NAMELEN - 1] = '\0';

    io_config.ipc_config.mem_cfg.memory_size = config.jbpf.mem_size_bytes;

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
    pipeline_cfg.lcm_socket_path    = config.jbpf.lcm_socket_path;
    pipeline_cfg.codelet_base_path  = config.jbpf.codelet_base_path;
    pipeline_cfg.expected_num_prbu  = config.radio.nof_prbs;
    pipeline_cfg.worker_core        = config.threads.worker_core;
    e3sm_pipeline::IqPipeline iq_pipeline(dispatcher, io_ctx, std::move(pipeline_cfg));

    e3sm_pipeline::SlotIqPipeline::Config slot_pipeline_cfg;
    slot_pipeline_cfg.lcm_socket_path   = config.jbpf.lcm_socket_path;
    slot_pipeline_cfg.codelet_base_path = config.jbpf.codelet_base_path;
    slot_pipeline_cfg.worker_core       = config.threads.worker_core;
    /* Gnb mode: the pipeline tells the codelet (and through it the gNB-side
     * helper) where /e3_ran_buffers is and what shape its rows are. The
     * controller owns the region, so it is the one that gets to say. */
    slot_pipeline_cfg.writer            = config.shm.writer;
    slot_pipeline_cfg.shm_name          = config.shm.name;
    slot_pipeline_cfg.radio             = config.radio;
    slot_pipeline_cfg.cbf16_scale       = config.shm.cbf16_scale;
    e3sm_pipeline::SlotIqPipeline slot_iq_pipeline(dispatcher, io_ctx,
                                                   std::move(slot_pipeline_cfg));

    // Register both service models.
    //  - RF=1 Spectrum SM: ecpri_iq IQ telemetry (Spectrum-IQDataIndication,
    //                       APER) via IqPipeline + PRB-blacklist control.
    //  - RF=2 L1-KPM SM:   slot-level SHM-pointer IQ indications in the
    //                       configured encoding. Wired to the SlotIqPipeline.
    libe3::ErrorCode sm_result;
    sm_result = agent.register_sm(std::make_unique<E3SMSpectrum>(
        iq_pipeline, agent, spectrum_stats_log_path));
    if (sm_result != libe3::ErrorCode::SUCCESS) {
        std::cerr << "Failed to register Spectrum SM (RF=1): "
                  << libe3::error_code_to_string(sm_result) << "\n";
        return 1;
    }
    /* Keep a borrowed pointer so the shutdown path can print the drop
     * accounting. The agent owns the SM and outlives this scope's use of the
     * pointer (the summary is printed before `agent` is destroyed), so this is a
     * read-only borrow, not a lifetime claim. */
    auto        layer1_sm  = std::make_unique<E3SMLayer1>(slot_iq_pipeline, agent, config);
    E3SMLayer1* layer1_ptr = layer1_sm.get();
    sm_result = agent.register_sm(std::move(layer1_sm));
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
    bool busy_poll = (config.threads.poll_core >= 0);
    if (busy_poll) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(config.threads.poll_core, &mask);
        if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
            std::fprintf(stderr,
                "[E3Controller] WARNING: failed to pin polling thread to core %d (%s); "
                "falling back to sleep-based polling\n",
                config.threads.poll_core, std::strerror(errno));
            busy_poll = false;
        } else {
            std::printf("[E3Controller] Polling thread pinned to core %d (busy-poll)\n",
                        config.threads.poll_core);
        }
    }

    // Main polling loop — the dispatcher routes buffers to registered SMs
    while (g_running) {
        dispatcher.poll();

        if (busy_poll) {
#if defined(__x86_64__) || defined(__i386__)
            _mm_pause();  // hyperthread-friendly hint
#endif
        } else if (config.threads.poll_interval_us > 0) {
            usleep(config.threads.poll_interval_us);
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

    /* Drop accounting, printed AFTER both pipelines have joined their workers so
     * the counters are final and no thread is still incrementing them.
     *
     * Two independent sources, because a slot can be lost in two places and
     * conflating them would point at the wrong fix:
     *
     *   SlotIqPipeline  the SPSC queue between the jbpf dispatcher and the
     *                   worker was full -- the controller could not keep up.
     *   E3SMLayer1      the slot reached the SM but did not become an
     *                   indication, broken out by reason.
     *
     * Neither can see a slot the RAN never produced: under TDD only UL slots
     * fire the hook, so a missing slot index is not necessarily a loss. */
    std::printf("\n[E3Controller] --- drop accounting ---\n");
    const uint64_t queue_drops = slot_iq_pipeline.dropped_samples();
    std::printf("[SlotIqPipeline] slots dropped on a full SPSC queue: %lu\n",
                static_cast<unsigned long>(queue_drops));
    std::printf("%s\n", layer1_ptr->drops_summary().c_str());

    std::printf("[E3Controller] Stopped.\n");

    return 0;
}
