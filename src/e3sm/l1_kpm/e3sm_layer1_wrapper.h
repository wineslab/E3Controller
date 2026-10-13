/*
 * APER encoder for the L1-KPM Service Model inner payload.
 *
 * Payload schema DERIVED FROM NVIDIA Aerial's public E3 message schema
 * (indicationMessage.protocolData) — see e3sm/asn/e3sm_layer1.asn for the full
 * attribution and https://github.com/NVIDIA/aerial-sample-apps.
 *
 * Field-for-field equivalent to encode_iq_indication_json() in
 * e3sm_layer1_json.h, so the layer-1 SM can pick the wire format per
 * subscriber without diverging on payload semantics:
 *
 *   L1KPM-Indication ::= SEQUENCE {
 *       iqSamplesRef    L1KPM-ShmRef OPTIONAL,
 *       timestamp       INTEGER,
 *       sfn             INTEGER (0..65535),
 *       slot            INTEGER (0..65535),
 *       cellId          INTEGER (0..65535) OPTIONAL,
 *       nRxAnt          INTEGER (0..65535) OPTIONAL,
 *       validSymbolMask INTEGER (0..16383) OPTIONAL
 *   }
 *
 * The shm reference fields (shm_name, fh_buffer_index, fh_write_index) are
 * passed unconditionally — encode_iq_indication_aper populates iqSamplesRef
 * for every indication, matching the JSON path's behavior. cellId and
 * validSymbolMask are not emitted yet (they'd be wired from the codelet's
 * metadata and the TDD config when we surface them); leaving them
 * OPTIONAL-absent costs only one preamble bit each.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/* RAN-function identity advertised at setup, the same values as OAI's L1-KPM
 * SM (openair2/E3AP/service_models/l1_kpm_sm), so a dApp sees one SM whichever
 * RAN serves it. Sizes are checked against L1KPM-RanFunctionData below. */
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

// APER twin of e3sm_layer1::encode_iq_indication_json().
//
// timestamp_ns   = codelet timestamp in nanoseconds (jbpf_time_get_ns()).
// sfn            = 3GPP system frame number (codelet's frame_id, 0..65535
//                  in the schema; 0..255 in practice).
// slot           = absolute slot index within the radio frame
//                  (subframe_id * slots_per_subframe + slot_id).
// shm_name       = POSIX SHM segment name (e.g. "/e3_ran_buffers"); echoed
//                  back so the dApp can sanity-check against its own mmap.
// fh_buffer_*    = ring-buffer coordinates the dApp uses to address the row.
// nof_ports      = number of RX antennas in the shm row (emitted as nRxAnt).
bool encode_iq_indication_aper(uint64_t timestamp_ns,
                               uint16_t sfn,
                               uint16_t slot,
                               const std::string& shm_name,
                               uint8_t fh_buffer_index,
                               uint32_t fh_write_index,
                               uint16_t nof_ports,
                               std::vector<uint8_t>& out);

// Encode the Layer-1 SM's descriptive RanFunctionData as
// L1KPM-RanFunctionData (a name/version/description SEQUENCE). Same shape
// and constraints as Spectrum-RanFunctionData, so one dApp-side decoder
// reads both SMs; only the string values differ.
bool encode_ran_function_data_aper(std::vector<uint8_t>& out);

// JSON twin of encode_ran_function_data_aper(): the same three fields, in the
// ASN.1 order, as one JSON object:
//   {"name":"L1-KPM","version":1,"description":"L1-KPM SM: post-FFT IQ telemetry."}
// Byte-identical to OAI's l1_kpm_enc.c. libe3 0.2.x writes ranFunctionData as
// a hex string of these bytes; libe3 with wineslab/libe3#118 nests the object.
bool encode_ran_function_data_json(std::vector<uint8_t>& out);

}  // namespace e3sm_layer1
