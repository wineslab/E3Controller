/*
 * e3_controller.cpp
 *
 * E3Controller: a standalone daemon that acts as a jbpf IPC primary process
 * and an E3 agent. It connects to srsRAN's jbpf shared memory to receive
 * codelet output data, and uses spear-libe3 to expose the data as E3
 * Service Model indications to subscribed dApps.
 *
 * Architecture:
 *   - A single JbpfDispatcher polls jbpf shared memory in the main loop
 *   - Service models register their stream_ids with the dispatcher
 *   - The dispatcher routes buffers to the right SM and releases them
 *
 * Usage:
 *   e3_controller [--ipc-name <name>] [--mem-size <bytes>]
 *
 * The IPC primary must start BEFORE srsRAN (IPC secondary).
 */

#include <iostream>
#include <memory>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <getopt.h>
#include <atomic>
#include <libe3/libe3.hpp>

extern "C" {
#include "jbpf_io.h"
#include "jbpf_io_channel.h"
#include "jbpf_common.h"
#include "jbpf_mem_mgmt.h"
}

#include "jbpf_dispatcher.h"
#include "e3sm_spectrum.h"

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
    int poll_interval_us = 100;  // microseconds between polls
};

static void print_usage(const char* prog)
{
    std::printf("Usage: %s [options]\n"
                "Options:\n"
                "  --ipc-name <name>     IPC shared memory name (default: e3_controller)\n"
                "  --run-path <path>     jbpf run path (default: /dev/shm)\n"
                "  --mem-size <bytes>    Shared memory size in bytes (default: 1GB)\n"
                "  --poll-interval <us>  Poll interval in microseconds (default: 100)\n"
                "  --help                Show this help\n",
                prog);
}

static E3ControllerConfig parse_args(int argc, char** argv)
{
    E3ControllerConfig config;

    static struct option long_options[] = {
        {"ipc-name",      required_argument, nullptr, 'n'},
        {"run-path",      required_argument, nullptr, 'r'},
        {"mem-size",      required_argument, nullptr, 'm'},
        {"poll-interval", required_argument, nullptr, 'p'},
        {"help",          no_argument,       nullptr, 'h'},
        {nullptr,         0,                 nullptr,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:r:m:p:h", long_options, nullptr)) != -1) {
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

    // E3 agent configuration
    libe3::E3LinkLayer link_layer = libe3::E3LinkLayer::ZMQ;
    libe3::E3TransportLayer transport_layer = libe3::E3TransportLayer::TCP;
    libe3::EncodingFormat encoding = libe3::EncodingFormat::ASN1;
    std::string ran_id = "srsRan-janus";

    libe3::E3Config agentConfig;
    agentConfig.ran_identifier = ran_id;
    agentConfig.link_layer = link_layer;
    agentConfig.transport_layer = transport_layer;
    agentConfig.encoding = encoding;
    agentConfig.log_level = 4;
    
    std::cout << "=============================================\n";
    std::cout << "E3 Agent Configuration:\n"
              << "  RAN ID: " << ran_id << "\n"
              << "  Link layer: " << libe3::link_layer_to_string(link_layer) << "\n"
              << "  Transport layer: " << libe3::transport_layer_to_string(transport_layer) << "\n"
              << "  Encoding: " << (encoding == libe3::EncodingFormat::JSON ? "json" : "asn1") << "\n\n";
    
    libe3::E3Agent agent(std::move(agentConfig));

    std::printf("=============================================\n");
    std::printf("  E3Controller Codelet Configuration\n");
    std::printf("=============================================\n");
    std::printf("  IPC name:       %s\n", config.ipc_name.c_str());
    std::printf("  Run path:       %s\n", config.run_path.c_str());
    std::printf("  Memory size:    %zu bytes\n", config.mem_size);
    std::printf("  Poll interval:  %d us\n", config.poll_interval_us);
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
    
    // Register service models (each SM registers its stream_ids with the dispatcher)
    libe3::ErrorCode sm_result = agent.register_sm(std::make_unique<E3SMSpectrum>(dispatcher));
    if (sm_result != libe3::ErrorCode::SUCCESS) {
        std::cerr << "Failed to register Spectrum SM: "
                  << libe3::error_code_to_string(sm_result) << "\n";
        return 1;
    }

    // TODO: For testing only — eagerly start the SM so the dispatcher receives
    // buffers immediately, without waiting for a dApp to subscribe.
    // In production, remove this and let libe3 start the SM on first subscription.
    /*{
	auto* spectrum_sm = dynamic_cast<E3SMSpectrum*>(
        libe3::SmRegistry::instance().get_by_ran_function(E3SMSpectrum::RAN_FUNCTION_ID));

         if (spectrum_sm) {
             auto rc = spectrum_sm->start();
             if (rc != libe3::ErrorCode::SUCCESS) {
                 std::cerr << "Failed to eagerly start Spectrum SM: "
                           << libe3::error_code_to_string(rc) << "\n";
                 return 1;
             }
             std::printf("[E3Controller] Spectrum SM started eagerly (test mode).\n");
         }
     }*/

    // Start the agent (calls SM::start(), which registers streams with dispatcher)
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
    
    // Main polling loop — the dispatcher routes buffers to registered SMs
    while (g_running) {
        dispatcher.poll();

        if (config.poll_interval_us > 0) {
            usleep(config.poll_interval_us);
        }
    }

    // Cleanup
    agent.stop();
    std::printf("[E3Controller] Shutting down...\n");
    jbpf_io_stop();
    std::printf("[E3Controller] Stopped.\n");

    return 0;
}
