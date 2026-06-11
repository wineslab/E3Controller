// SPDX-License-Identifier: Apache-2.0
//
// encode_iq_indication_json() reproduces the indicationMessage.protocolData
// JSON schema from NVIDIA Aerial's public E3 message schema (see header).
//   SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
//   https://github.com/NVIDIA/aerial-sample-apps (dapps/docs/e3_message_schemas.json)
#include "e3sm_layer1_json.h"

#include <nlohmann/json.hpp>

namespace e3sm_layer1 {

bool encode_iq_indication_json(uint64_t timestamp_ns,
                               uint16_t sfn,
                               uint16_t slot,
                               const std::string& shm_name,
                               uint8_t fh_buffer_index,
                               uint32_t fh_write_index,
                               std::vector<uint8_t>& out)
{
    // Field set + key spelling per NVIDIA aerial-sample-apps
    // dapps/docs/e3_message_examples.json indicationMessage.protocolData. The dApp reads
    // sfn/slot at protocolData top level (payload.value("sfn"/"slot", 0u))
    // and forwards them to the visualizer for TDD-aligned rendering.
    nlohmann::json j = {
        {"timestamp", timestamp_ns},
        {"sfn",       sfn},
        {"slot",      slot},
        {"iq_samples", {
            {"shm_name",        shm_name},
            {"fh_buffer_index", fh_buffer_index},
            {"fh_write_index",  fh_write_index},
        }},
    };
    std::string s = j.dump();
    out.assign(s.begin(), s.end());
    return true;
}

}  // namespace e3sm_layer1
