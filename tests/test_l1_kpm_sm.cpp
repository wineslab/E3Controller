/*
 * Tests of the L1-KPM SM codecs (src/e3sm/l1_kpm/e3sm_layer1_wrapper.*,
 * e3sm_layer1_json.*), E3SM-L1-KPM version 2.
 *
 * Build with -DE3C_BUILD_TESTS=ON and run with ctest, or directly:
 *   out/bin/test_l1_kpm_sm
 *
 * No other encoder of version 2 exists yet, so the APER checks decode what the
 * controller encoded and compare the fields with the inputs. The JSON checks use
 * the keys of the specification (clause 8.6), written out here rather than taken
 * from the encoder.
 */

#include "e3sm/l1_kpm/e3sm_layer1_json.h"
#include "e3sm/l1_kpm/e3sm_layer1_streams.h"
#include "e3sm/l1_kpm/e3sm_layer1_wrapper.h"

extern "C" {
#include "L1KPM-CellData.h"
#include "L1KPM-Delivery.h"
#include "L1KPM-Indication.h"
#include "L1KPM-RanFunctionData.h"
#include "L1KPM-ShmRef.h"
#include "L1KPM-StreamDescriptor.h"
#include "aper_decoder.h"
}

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace e3sm_layer1;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
    if (condition) {
        std::printf("  ok    %s\n", what.c_str());
    } else {
        std::printf("  FAIL  %s\n", what.c_str());
        g_failures++;
    }
}

static std::string octets(const OCTET_STRING_t& value)
{
    return std::string(reinterpret_cast<const char*>(value.buf), static_cast<size_t>(value.size));
}

template <typename T>
static T* decode_aper(const asn_TYPE_descriptor_t& type, const std::vector<uint8_t>& bytes)
{
    void*               decoded = nullptr;
    const asn_dec_rval_t rc     = aper_decode_complete(nullptr, &type, &decoded, bytes.data(), bytes.size());
    if (rc.code != RC_OK) {
        ASN_STRUCT_FREE(type, decoded);
        return nullptr;
    }
    return static_cast<T*>(decoded);
}

static const std::string kShmName = "/e3_ran_buffers";

// ---------------------------------------------------------------------------
// Indication
// ---------------------------------------------------------------------------

static void test_indication_aper()
{
    std::printf("Indication, APER\n");

    std::vector<uint8_t> bytes;
    check(encode_iq_indication_aper(1234567890123ULL, 1023, 19, kShmName, 1, 730, 4, 0x3C00, bytes), "encodes");
    check(bytes.size() < 100, "fits well under 100 bytes");

    auto* ind = decode_aper<L1KPM_Indication_t>(asn_DEF_L1KPM_Indication, bytes);
    check(ind != nullptr, "decodes");
    if (ind == nullptr) {
        return;
    }
    check(ind->timestamp && *ind->timestamp == 1234567890123L, "timestamp (ID 4)");
    check(ind->sfn && *ind->sfn == 1023, "sfn (ID 5)");
    check(ind->slot && *ind->slot == 19, "slot (ID 6)");
    check(ind->nCells && *ind->nCells == 1, "nCells (ID 10) = 1");
    check(ind->timestampTai == nullptr, "timestampTai (ID 78) absent");
    check(ind->cells && ind->cells->list.count == 1, "one cell");
    if (ind->cells && ind->cells->list.count == 1) {
        const L1KPM_CellData_t* cell = ind->cells->list.array[0];
        check(cell->iqSamples != nullptr, "iqSamples (ID 1) present");
        if (cell->iqSamples != nullptr) {
            check(octets(cell->iqSamples->shmName) == kShmName, "iqSamples.shmName");
            check(cell->iqSamples->bufferIndex == 1, "iqSamples.bufferIndex");
            check(cell->iqSamples->writeIndex == 730, "iqSamples.writeIndex");
            check(cell->iqSamples->rowByteOffset == nullptr, "iqSamples.rowByteOffset absent (ID 1)");
        }
        check(cell->nRxAnt && *cell->nRxAnt == 4, "nRxAnt (ID 8)");
        check(cell->validSymbolMask && *cell->validSymbolMask == 0x3C00, "validSymbolMask (ID 1000)");
        check(cell->cellId == nullptr, "cellId (ID 7) absent");
        check(cell->nUe == nullptr && cell->ues == nullptr, "nUe (ID 34) and ues absent");
    }
    ASN_STRUCT_FREE(asn_DEF_L1KPM_Indication, ind);
}

static void test_indication_json()
{
    std::printf("Indication, JSON\n");

    std::vector<uint8_t> bytes;
    check(encode_iq_indication_json(1234567890123ULL, 1023, 19, kShmName, 1, 730, 4, 0x3C00, bytes), "encodes");

    const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    check(!j.is_discarded() && j.is_object(), "is a JSON object");
    if (j.is_discarded()) {
        return;
    }
    check(j.value("timestamp", 0ULL) == 1234567890123ULL, "timestamp at the root");
    check(j.value("sfn", 0) == 1023 && j.value("slot", 0) == 19, "sfn and slot at the root");
    check(j.value("n_cells", 0) == 1, "n_cells = 1");
    check(!j.contains("iq_samples") && !j.contains("n_rx_ant"), "no cell field at the root");
    check(j.contains("cells") && j["cells"].is_array() && j["cells"].size() == 1, "cells holds one entry");
    if (!j.contains("cells") || j["cells"].size() != 1) {
        return;
    }
    const auto& cell = j["cells"][0];
    check(cell.contains("iq_samples"), "cells[0].iq_samples");
    if (cell.contains("iq_samples")) {
        const auto& iq = cell["iq_samples"];
        check(iq.value("shm_name", std::string()) == kShmName, "iq_samples.shm_name");
        check(iq.value("fh_buffer_index", -1) == 1, "iq_samples.fh_buffer_index");
        check(iq.value("fh_write_index", -1) == 730, "iq_samples.fh_write_index");
    }
    check(cell.value("n_rx_ant", 0) == 4, "cells[0].n_rx_ant");
    check(cell.value("valid_symbol_mask", 0) == 0x3C00, "cells[0].valid_symbol_mask");
    check(!cell.contains("n_ue") && !cell.contains("ues"), "no n_ue and no ues");
}

// ---------------------------------------------------------------------------
// RAN function data
// ---------------------------------------------------------------------------

static const IqStreamGeometry kIq{kShmName, 731u * 733824u, 731u};

static void test_stream_table()
{
    std::printf("Stream table\n");

    std::set<uint32_t> ids;
    for (const StreamInfo& s : kStreams) {
        ids.insert(s.telemetry_id);
    }
    check(std::size(kStreams) == 80 && ids.size() == 80, "80 identifiers, no duplicate");
    bool contiguous = true;
    for (uint32_t id = 1; id <= 79; ++id) {
        contiguous = contiguous && ids.count(id) == 1;
    }
    check(contiguous && ids.count(1000) == 1, "IDs 1..79 and 1000");
    bool served_known = true;
    for (const uint32_t id : kServedTelemetryIds) {
        served_known = served_known && ids.count(id) == 1;
    }
    check(served_known, "every served ID is in the table");
}

static void test_ran_function_data_aper()
{
    std::printf("RAN function data, APER\n");

    std::vector<uint8_t> bytes;
    check(encode_ran_function_data_aper(kIq, bytes), "encodes");
    check(!bytes.empty() && bytes.size() <= 32768, "fits E3AP ranFunctionData (1..32768 octets)");

    auto* rfd = decode_aper<L1KPM_RanFunctionData_t>(asn_DEF_L1KPM_RanFunctionData, bytes);
    check(rfd != nullptr, "decodes");
    if (rfd == nullptr) {
        return;
    }
    check(octets(rfd->name) == "L1-KPM", "name = L1-KPM");
    check(rfd->version == 2, "version = 2");
    check(rfd->streams.list.count == 80, "one stream descriptor for each identifier");

    for (int i = 0; i < rfd->streams.list.count; ++i) {
        const L1KPM_StreamDescriptor_t* d = rfd->streams.list.array[i];
        const uint32_t                  id = static_cast<uint32_t>(d->telemetryId);
        if (id == kTelemetryIqSamples) {
            check(octets(d->streamId) == "iq_samples" && octets(d->dataType) == "array(int16)",
                  "ID 1: iq_samples, array(int16)");
            check(d->status == L1KPM_StreamStatus_available, "ID 1 available");
            const bool shm = d->delivery && d->delivery->present == L1KPM_Delivery_PR_sharedMemory;
            check(shm, "ID 1 delivered through shared memory");
            if (shm) {
                const auto& info = d->delivery->choice.sharedMemory;
                check(octets(info.memoryKey) == kShmName, "ID 1 memoryKey");
                check(info.memorySizeBytes == kIq.buffer_bytes && info.numRows == static_cast<long>(kIq.num_rows),
                      "ID 1 memorySizeBytes and numRows: one buffer");
            }
        } else if (id == kTelemetryTimestamp) {
            const bool inline_data = d->status == L1KPM_StreamStatus_available && d->delivery &&
                                     d->delivery->present == L1KPM_Delivery_PR_protocolData;
            check(inline_data, "ID 4 available, inline");
            if (inline_data) {
                check(octets(d->delivery->choice.protocolData.keyword) == "timestamp", "ID 4 keyword = timestamp");
            }
        } else if (id == 34) {
            check(d->status == L1KPM_StreamStatus_unavailable && d->delivery == nullptr,
                  "ID 34 (n_ue) unavailable, no delivery");
        }
    }
    ASN_STRUCT_FREE(asn_DEF_L1KPM_RanFunctionData, rfd);
}

static void test_ran_function_data_json()
{
    std::printf("RAN function data, JSON\n");

    std::vector<uint8_t> bytes;
    check(encode_ran_function_data_json(kIq, bytes), "encodes");
    check(bytes.size() <= 32768, "fits E3AP ranFunctionData (1..32768 octets)");

    const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    check(!j.is_discarded() && j.is_object(), "is a JSON object, not the legacy array");
    if (j.is_discarded()) {
        return;
    }
    check(j.value("name", std::string()) == "L1-KPM" && j.value("version", 0) == 2, "name and version");
    check(j.contains("streams") && j["streams"].size() == 80, "80 stream descriptors");
    if (!j.contains("streams")) {
        return;
    }
    for (const auto& d : j["streams"]) {
        const int id = d.value("telemetryIdentifier", 0);
        if (id == 1) {
            check(d.value("stream_id", std::string()) == "iq_samples" && d.value("status", std::string()) == "available",
                  "ID 1: iq_samples, available");
            const auto& dm = d.value("delivery_method", nlohmann::json::object());
            check(dm.value("transport_type", std::string()) == "shared_memory", "ID 1 transport_type shared_memory");
            const auto& info = dm.value("shared_memory_info", nlohmann::json::object());
            check(info.value("memory_key", std::string()) == kShmName &&
                      info.value("memory_size_bytes", 0U) == kIq.buffer_bytes &&
                      info.value("num_rows", 0U) == kIq.num_rows,
                  "ID 1 shared_memory_info");
        } else if (id == 1000) {
            const auto& dm = d.value("delivery_method", nlohmann::json::object());
            check(d.value("status", std::string()) == "available" &&
                      dm.value("transport_type", std::string()) == "protocolData" &&
                      dm.value("keyword", std::string()) == "valid_symbol_mask",
                  "ID 1000 available, inline as valid_symbol_mask");
        } else if (id == 2) {
            check(d.value("status", std::string()) == "unavailable" && !d.contains("delivery_method"),
                  "ID 2 (pdu_data) unavailable, no delivery_method");
        }
    }
}

int main()
{
    test_indication_aper();
    test_indication_json();
    test_stream_table();
    test_ran_function_data_aper();
    test_ran_function_data_json();

    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
