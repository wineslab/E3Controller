/*
 * APER encoder for the L1-KPM Service Model inner payload, E3SM-L1-KPM version 1.
 *
 * Payload schema DERIVED FROM NVIDIA Aerial's public E3 message schema
 * (indicationMessage.protocolData) — see e3sm/asn/e3sm_layer1.asn for the full
 * attribution and https://github.com/NVIDIA/aerial-sample-apps.
 *
 * Field-for-field equivalent to encode_iq_indication_json() in
 * e3sm_layer1_json.h, so the layer-1 SM can pick the wire format per
 * subscriber without diverging on payload semantics. One indication carries:
 *
 *   L1KPM-Indication: timestamp (ID 4), sfn (5), slot (6), nCells (10) = 1,
 *     cells[0] = L1KPM-CellData: iqSamples (1), nRxAnt (8), validSymbolMask (1000)
 *
 * cellId (ID 7) is not emitted (the codelet does not surface the PCI), and
 * nUe (ID 34) is not either: it counts the ues entries an indication carries,
 * and this controller carries none yet.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/* RAN-function identity advertised at setup, as E3SM-L1-KPM version 1 sets it:
 * name "L1-KPM", version 1. Sizes are checked against L1KPM-RanFunctionData
 * below. */
#define LAYER1_RAN_FUNCTION_NAME        "L1-KPM"
#define LAYER1_RAN_FUNCTION_DESCRIPTION "L1-KPM SM: post-FFT IQ telemetry."
#define LAYER1_RAN_FUNCTION_VERSION     1

/* L1KPM-RanFunctionData: name 1..32 bytes, version 0..65535, description
 * 1..128 bytes. Checked at build time so the JSON form can never carry what
 * the ASN.1 form would refuse. */
static_assert(sizeof(LAYER1_RAN_FUNCTION_NAME) - 1 >= 1 &&
              sizeof(LAYER1_RAN_FUNCTION_NAME) - 1 <= 32,
              "LAYER1_RAN_FUNCTION_NAME must be 1..32 bytes (L1KPM-RanFunctionData.name)");
static_assert(sizeof(LAYER1_RAN_FUNCTION_DESCRIPTION) - 1 >= 1 &&
              sizeof(LAYER1_RAN_FUNCTION_DESCRIPTION) - 1 <= 128,
              "LAYER1_RAN_FUNCTION_DESCRIPTION must be 1..128 bytes (L1KPM-RanFunctionData.description)");
static_assert(LAYER1_RAN_FUNCTION_VERSION >= 0 && LAYER1_RAN_FUNCTION_VERSION <= 65535,
              "LAYER1_RAN_FUNCTION_VERSION must be 0..65535 (L1KPM-RanFunctionData.version)");

namespace e3sm_layer1 {

/* Where the IQ stream (ID 1) lives, for its stream descriptor: one buffer of the
 * shared memory region, as the region's header gives it (Annex A). */
struct IqStreamGeometry {
    std::string shm_name;      /* memoryKey */
    uint32_t    buffer_bytes;  /* memorySizeBytes: size of one buffer */
    uint32_t    num_rows;      /* numRows: rows of one buffer, 1..65535 */
};

// APER twin of e3sm_layer1::encode_iq_indication_json().
//
// timestamp_ns   = gNB hand-off timestamp in nanoseconds (CLOCK_MONOTONIC).
// sfn            = 3GPP system frame number, 0..1023.
// slot           = slot index within the radio frame, 0..159.
// shm_name       = POSIX SHM segment name (e.g. "/e3_ran_buffers"); echoed
//                  back so the dApp can sanity-check against its own mmap.
// fh_buffer_*    = ring-buffer coordinates the dApp uses to address the row.
// nof_ports      = number of RX antennas in the shm row (emitted as nRxAnt,
//                  1..64; 0 leaves it out).
// valid_symbol_mask = bit s set = symbol s is uplink per the gNB's TDD pattern
//                  (emitted as validSymbolMask; DL/guard symbols are 0).
bool encode_iq_indication_aper(uint64_t timestamp_ns,
                               uint16_t sfn,
                               uint16_t slot,
                               const std::string& shm_name,
                               uint8_t fh_buffer_index,
                               uint32_t fh_write_index,
                               uint16_t nof_ports,
                               uint16_t valid_symbol_mask,
                               std::vector<uint8_t>& out);

// Encode the Layer-1 SM's RanFunctionData as L1KPM-RanFunctionData: name,
// version and description, then one stream descriptor for every telemetry
// identifier of the specification (e3sm_layer1_streams.h). The ones this
// controller produces are "available", with their delivery; the IQ samples go
// through the shared memory region `iq`, the others inline.
bool encode_ran_function_data_aper(const IqStreamGeometry& iq, std::vector<uint8_t>& out);

// JSON twin of encode_ran_function_data_aper(), with the JSON keys of the
// specification:
//   {"name":"L1-KPM","version":1,"description":"...","streams":[
//     {"telemetryIdentifier":1,"stream_id":"iq_samples","data_type":"array(int16)",
//      "status":"available","delivery_method":{"transport_type":"shared_memory",
//        "shared_memory_info":{"memory_key":...,"memory_size_bytes":...,"num_rows":...}}},
//     ...]}
// libe3 0.2.x writes ranFunctionData as a hex string of these bytes; libe3 with
// wineslab/libe3#118 nests the object.
bool encode_ran_function_data_json(const IqStreamGeometry& iq, std::vector<uint8_t>& out);

}  // namespace e3sm_layer1
