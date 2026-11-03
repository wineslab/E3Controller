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
                               uint16_t nof_ports,
                               uint16_t valid_symbol_mask,
                               std::vector<uint8_t>& out)
{
    // Field set + key spelling per E3SM-L1-KPM version 2, which takes NVIDIA's
    // schema v1.2.0: timestamp, sfn, slot and n_cells at the root, the rest of
    // the one cell this RAN node serves in cells[0]. The dApp matches sfn/slot
    // against other telemetry of the slot and forwards them to the visualizer
    // for TDD-aligned rendering. n_rx_ant tells it how many antennas the shm row
    // holds; valid_symbol_mask which symbols are uplink (DL/guard symbols of a
    // mixed slot are 0).
    nlohmann::json cell = {
        {"iq_samples", {
            {"shm_name",        shm_name},
            {"fh_buffer_index", fh_buffer_index},
            {"fh_write_index",  fh_write_index},
        }},
        {"valid_symbol_mask", valid_symbol_mask},
    };
    if (nof_ports > 0) {
        cell["n_rx_ant"] = nof_ports;
    }

    nlohmann::json j = {
        {"timestamp", timestamp_ns},
        {"sfn",       sfn},
        {"slot",      slot},
        {"n_cells",   1},
        {"cells",     nlohmann::json::array({cell})},
    };
    std::string s = j.dump();
    out.assign(s.begin(), s.end());
    return true;
}

}  // namespace e3sm_layer1
