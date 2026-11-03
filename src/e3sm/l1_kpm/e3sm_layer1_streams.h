/*
 * The telemetry identifiers of E3SM-L1-KPM version 2 (clause 7.3 of the
 * specification), for the stream descriptors of the RAN function data. The
 * specification lists every identifier there, also the ones a RAN node does not
 * produce, so a dApp can tell "not supported" from "unknown".
 *
 * Taken from the tables of clause 7.3 (report/e3sm-l1-kpm/spec/
 * 03-ran-function-description.md): the stream name is the JSON key of the field,
 * and the data type is the one of the table.
 */
#pragma once

#include <cstdint>

namespace e3sm_layer1 {

struct StreamInfo {
    uint32_t    telemetry_id;
    const char* stream_id;
    const char* data_type;
};

inline constexpr StreamInfo kStreams[] = {
    {   1, "iq_samples",           "array(int16)"},
    {   2, "pdu_data",             "array(uint8)"},
    {   3, "h_estimates",          "array(complex64)"},
    {   4, "timestamp",            "uint64"},
    {   5, "sfn",                  "uint16"},
    {   6, "slot",                 "uint16"},
    {   7, "cell_id",              "uint16"},
    {   8, "n_rx_ant",             "uint16"},
    {   9, "n_rx_ant_srs",         "uint16"},
    {  10, "n_cells",              "uint16"},
    {  11, "n_bs_ants",            "uint8"},
    {  12, "n_layers",             "uint8"},
    {  13, "n_subcarriers",        "uint16"},
    {  14, "n_dmrs_estimates",     "uint8"},
    {  15, "dmrs_symb_pos",        "uint16"},
    {  16, "tb_crc_fail",          "uint8"},
    {  17, "cb_errors",            "uint32"},
    {  18, "rsrp",                 "float32"},
    {  19, "noise_var",            "float32"},
    {  20, "cb_count",             "uint16"},
    {  21, "rssi",                 "float32"},
    {  22, "qam_mod_order",        "uint8"},
    {  23, "mcs_index",            "uint8"},
    {  24, "mcs_table_index",      "uint8"},
    {  25, "rb_start",             "uint16"},
    {  26, "rb_size",              "uint16"},
    {  27, "start_symbol_index",   "uint8"},
    {  28, "nr_of_symbols",        "uint8"},
    {  29, "tb_size",              "uint32"},
    {  30, "pdu_len",              "uint32"},
    {  31, "target_code_rate",     "uint16"},
    {  32, "new_data_indicator",   "uint8"},
    {  33, "rnti",                 "uint16"},
    {  34, "n_ue",                 "uint16"},
    {  35, "layer_offset",         "uint16"},
    {  36, "ue_grp_idx",           "uint16"},
    {  37, "h_offset",             "uint32"},
    {  38, "h_size",               "uint32"},
    {  39, "sinr",                 "float32"},
    {  40, "timing_advance",       "float32"},
    {  41, "harq_process_id",      "uint8"},
    {  42, "rv_index",             "uint8"},
    {  43, "cfo_hz",               "float32"},
    {  44, "srs_iq_samples",       "array(int16)"},
    {  45, "srs_hest",             "array(int16)"},
    {  46, "srs_hest_n_prb_grps",  "uint16"},
    {  47, "srs_hest_offset",      "uint32"},
    {  48, "srs_hest_size",        "uint32"},
    {  49, "srs_rb_snr",           "array(float32)"},
    {  50, "srs_rb_snr_offset",    "uint32"},
    {  51, "srs_rb_snr_size",      "uint32"},
    {  52, "srs_cell_start_sym",   "uint8"},
    {  53, "srs_cell_n_srs_sym",   "uint8"},
    {  54, "n_srs_ue",             "uint8"},
    {  55, "srs_wideband_snr",     "float32"},
    {  56, "srs_signal_energy",    "float32"},
    {  57, "srs_noise_energy",     "float32"},
    {  58, "srs_toa",              "float32"},
    {  59, "srs_hd_ant_flag",      "uint8"},
    {  60, "srs_sc_corr",          "array(float32)"},
    {  61, "srs_cs_corr_ratio_db", "float32"},
    {  62, "srs_ant_ports",        "uint8"},
    {  63, "srs_n_syms",           "uint8"},
    {  64, "srs_n_repetitions",    "uint8"},
    {  65, "srs_comb_size",        "uint8"},
    {  66, "srs_comb_offset",      "uint8"},
    {  67, "srs_start_sym",        "uint8"},
    {  68, "srs_cyclic_shift",     "uint8"},
    {  69, "srs_freq_position",    "uint8"},
    {  70, "srs_freq_shift",       "uint16"},
    {  71, "srs_freq_hopping",     "uint8"},
    {  72, "srs_resource_type",    "uint8"},
    {  73, "srs_t_srs",            "uint16"},
    {  74, "srs_t_offset",         "uint16"},
    {  75, "srs_usage",            "uint32"},
    {  76, "srs_n_valid_prg",      "uint16"},
    {  77, "srs_prg_size",         "uint16"},
    {  78, "timestamp_tai",        "uint64"},
    {  79, "pdu_offset",           "uint32"},
    {1000, "valid_symbol_mask",    "uint16"},
};

/* The identifiers this E3 Agent produces. Every other one is "unavailable". */
inline constexpr uint32_t kTelemetryIqSamples       = 1;
inline constexpr uint32_t kTelemetryTimestamp       = 4;
inline constexpr uint32_t kTelemetrySfn             = 5;
inline constexpr uint32_t kTelemetrySlot            = 6;
inline constexpr uint32_t kTelemetryNRxAnt          = 8;
inline constexpr uint32_t kTelemetryNCells          = 10;
inline constexpr uint32_t kTelemetryValidSymbolMask = 1000;

inline constexpr uint32_t kServedTelemetryIds[] = {
    kTelemetryIqSamples, kTelemetryTimestamp, kTelemetrySfn,   kTelemetrySlot,
    kTelemetryNRxAnt,    kTelemetryNCells,    kTelemetryValidSymbolMask,
};

inline constexpr bool is_served(uint32_t telemetry_id)
{
    for (const uint32_t id : kServedTelemetryIds) {
        if (id == telemetry_id) {
            return true;
        }
    }
    return false;
}

}  // namespace e3sm_layer1
