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
 *       nRxAnt          INTEGER (0..65535) OPTIONAL
 *   }
 *
 * The shm reference fields (shm_name, fh_buffer_index, fh_write_index) are
 * passed unconditionally — encode_iq_indication_aper populates iqSamplesRef
 * for every indication, matching the JSON path's behavior. cellId / nRxAnt
 * are not emitted yet (they'd be wired from the codelet's eAxC metadata when
 * we surface them); leaving them OPTIONAL-absent costs only one preamble bit.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define LAYER1_RAN_FUNCTION_NAME        "l1_kpm_sm"
#define LAYER1_RAN_FUNCTION_DESCRIPTION "Layer-1 KPM service model: per-slot SHM-pointer IQ indications"
#define LAYER1_RAN_FUNCTION_VERSION     1

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

// Encode the Layer-1 SM's descriptive RanFunctionData. Uses the
// Spectrum-RanFunctionData ASN.1 type for the wire shape (a name/version/
// description SEQUENCE) so both SMs hit the same dApp-side decoder. Only
// the string values differ.
bool encode_ran_function_data_aper(std::vector<uint8_t>& out);

// JSON twin of encode_ran_function_data_aper(). Emits {name, version,
// description} as an inline JSON object for libe3's JsonE3Encoder to nest
// inside ranFunctionList[].ranFunctionData.
bool encode_ran_function_data_json(std::vector<uint8_t>& out);

}  // namespace e3sm_layer1
