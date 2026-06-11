/*
 * JSON encoder for the L1-KPM (RF=2) indication payload.
 *
 * The indicationMessage.protocolData JSON schema produced by
 * encode_iq_indication_json() — iq_samples{shm_name, fh_buffer_index,
 * fh_write_index}, timestamp, sfn, slot — is taken from NVIDIA Aerial's public
 * E3 message schema, so the stock NVIDIA Aerial dApps interoperate unchanged:
 *
 *   https://github.com/NVIDIA/aerial-sample-apps
 *     dapps/docs/e3_message_schemas.json   (indicationMessage.protocolData)
 *     dapps/docs/e3_message_examples.json   (indicationMessage example)
 *
 *   SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 *   SPDX-License-Identifier: Apache-2.0
 *
 * Fields we don't have from the codelet (h_estimates, PUSCH metadata,
 * mcs_index, ...) are omitted — the dApp's payload.value(k, dflt) pattern
 * tolerates absent keys. Bulk IQ travels out of band through
 * /dev/shm/<shm_name>; the JSON payload carries only the SHM coordinates plus
 * 5G time-position metadata (sfn/slot) for TDD-aligned visualizer rendering.
 *
 * We emit a UTF-8 JSON object so libe3's outer JsonE3Encoder embeds it inline
 * via bytes_to_json_payload (libe3/src/encoder/json_encoder.cpp). It is the
 * JSON twin of e3sm_layer1::encode_iq_indication_aper() (e3sm_layer1_wrapper.h).
 */
#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace e3sm_layer1 {

// timestamp_ns   = codelet timestamp in nanoseconds (jbpf_time_get_ns()).
// sfn            = 3GPP system frame number (codelet's frame_id, 0..255).
// slot           = absolute slot index within the radio frame
//                  (subframe_id * slots_per_subframe + slot_id).
// shm_name       = POSIX SHM segment name (e.g. "/e3_ran_buffers"); echoed
//                  back so the dApp can sanity-check against its own mmap.
// fh_buffer_*    = ring-buffer coordinates the dApp uses to address the row.
bool encode_iq_indication_json(uint64_t timestamp_ns,
                               uint16_t sfn,
                               uint16_t slot,
                               const std::string& shm_name,
                               uint8_t fh_buffer_index,
                               uint32_t fh_write_index,
                               std::vector<uint8_t>& out);

}  // namespace e3sm_layer1
