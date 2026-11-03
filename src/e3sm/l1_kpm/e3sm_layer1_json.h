/*
 * JSON encoder for the L1-KPM (RF=2) indication payload, E3SM-L1-KPM version 2.
 *
 * The indicationMessage.protocolData JSON schema produced by
 * encode_iq_indication_json() is the one of NVIDIA Aerial's public E3 message
 * schema, version 1.2.0: slot-shared scalars at the root, everything of a cell in
 * one entry of `cells`. The stock NVIDIA Aerial dApps read it as is:
 *
 *   {"timestamp", "sfn", "slot", "n_cells": 1,
 *    "cells": [{"iq_samples": {"shm_name", "fh_buffer_index", "fh_write_index"},
 *               "n_rx_ant", "valid_symbol_mask"}]}
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
 * n_rx_ant (antennas in the row) and valid_symbol_mask (bit s set = symbol s is uplink per the gNB's TDD
 * pattern; a mixed slot's DL and guard symbols are 0).
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

// timestamp_ns   = gNB hand-off timestamp in nanoseconds (CLOCK_MONOTONIC).
// sfn            = 3GPP system frame number, 0..1023.
// slot           = slot index within the radio frame, 0..159.
// shm_name       = POSIX SHM segment name (e.g. "/e3_ran_buffers"); echoed
//                  back so the dApp can sanity-check against its own mmap.
// fh_buffer_*    = ring-buffer coordinates the dApp uses to address the row.
// nof_ports      = number of RX antennas in the shm row (emitted as n_rx_ant,
//                  1..64; 0 leaves it out).
// valid_symbol_mask = bit s set = symbol s is uplink per the gNB's TDD pattern
//                  (emitted as valid_symbol_mask; DL/guard symbols are 0).
bool encode_iq_indication_json(uint64_t timestamp_ns,
                               uint16_t sfn,
                               uint16_t slot,
                               const std::string& shm_name,
                               uint8_t fh_buffer_index,
                               uint32_t fh_write_index,
                               uint16_t nof_ports,
                               uint16_t valid_symbol_mask,
                               std::vector<uint8_t>& out);

}  // namespace e3sm_layer1
