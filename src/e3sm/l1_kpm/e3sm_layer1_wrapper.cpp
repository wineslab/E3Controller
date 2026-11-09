/*
 * APER encoder implementation for L1-KPM Indication, E3SM-L1-KPM version 1.
 *
 * Payload schema derived from NVIDIA Aerial's public E3 message schema
 * (see e3sm/asn/e3sm_layer1.asn for attribution).
 *
 * Mirrors e3sm_spect_wrapper.cpp's encode_spectrum_iq_indication() style: use
 * the asn1c-generated C struct, fill it with calloc'd OPTIONAL pointers,
 * call aper_encode_to_buffer, then free the allocations. The asn1c runtime
 * itself lives in libe3 (asn1_e3ap) — we only depend on the L1KPM-* type
 * files generated under build/asn1c_generated/.
 */

#include "e3sm_layer1_wrapper.h"
#include "e3sm_layer1_streams.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "L1KPM-CellData.h"
#include "L1KPM-Delivery.h"
#include "L1KPM-Indication.h"
#include "L1KPM-RanFunctionData.h"
#include "L1KPM-ShmRef.h"
#include "L1KPM-StreamDescriptor.h"
#include "aper_encoder.h"

#ifdef __cplusplus
}
#endif

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>

namespace e3sm_layer1 {

namespace {

/* Room for one indication: one cell and no UE list, well under 100 bytes. */
constexpr size_t kIndicationMaxBytes = 256;

/* Room for the RAN function data with all 80 stream descriptors, about 3 kB. */
constexpr size_t kRanFunctionDataMaxBytes = 16384;

/* Allocates an OPTIONAL member and sets it. ASN_STRUCT_FREE_CONTENTS_ONLY of the
 * enclosing structure releases it. */
template <typename T, typename V>
bool set_optional(T*& field, V value)
{
    field = static_cast<T*>(std::calloc(1, sizeof(T)));
    if (field == nullptr) {
        return false;
    }
    *field = static_cast<T>(value);
    return true;
}

bool set_octets(OCTET_STRING_t& field, const char* value)
{
    return OCTET_STRING_fromBuf(&field, value, static_cast<int>(std::strlen(value))) == 0;
}

bool set_octets(OCTET_STRING_t& field, const std::string& value)
{
    return OCTET_STRING_fromBuf(&field, value.data(), static_cast<int>(value.size())) == 0;
}

bool encode_aper(const asn_TYPE_descriptor_t& type, const void* value, size_t max_bytes, std::vector<uint8_t>& out)
{
    std::vector<uint8_t> buffer(max_bytes);
    const asn_enc_rval_t ret = aper_encode_to_buffer(&type, nullptr, value, buffer.data(), buffer.size());
    if (ret.encoded == -1) {
        return false;
    }
    buffer.resize((static_cast<size_t>(ret.encoded) + 7) / 8);
    out = std::move(buffer);
    return true;
}

bool fill_indication(L1KPM_Indication_t& ind,
                     uint64_t timestamp_ns,
                     uint16_t sfn,
                     uint16_t slot,
                     const std::string& shm_name,
                     uint8_t fh_buffer_index,
                     uint32_t fh_write_index,
                     uint16_t nof_ports,
                     uint16_t valid_symbol_mask)
{
    // Slot scope: IDs 4, 5, 6 and 10. asn1c maps the unconstrained
    // L1KPM-TimestampNs to `long`, which holds the uint64 ns of CLOCK_MONOTONIC
    // on 64-bit Linux.
    if (!set_optional(ind.timestamp, static_cast<long>(timestamp_ns)) || !set_optional(ind.sfn, sfn) ||
        !set_optional(ind.slot, slot) || !set_optional(ind.nCells, 1)) {
        return false;
    }

    // Cell scope: the one cell this RAN node serves.
    ind.cells = static_cast<decltype(ind.cells)>(std::calloc(1, sizeof(*ind.cells)));
    if (ind.cells == nullptr) {
        return false;
    }
    auto* cell = static_cast<L1KPM_CellData_t*>(std::calloc(1, sizeof(L1KPM_CellData_t)));
    if (cell == nullptr) {
        return false;
    }
    if (ASN_SEQUENCE_ADD(&ind.cells->list, cell) != 0) {
        std::free(cell);
        return false;
    }

    // ID 1: the row of /e3_ran_buffers that holds the IQ samples of the slot.
    cell->iqSamples = static_cast<L1KPM_ShmRef_t*>(std::calloc(1, sizeof(L1KPM_ShmRef_t)));
    if (cell->iqSamples == nullptr || !set_octets(cell->iqSamples->shmName, shm_name)) {
        return false;
    }
    cell->iqSamples->bufferIndex = fh_buffer_index;
    cell->iqSamples->writeIndex  = fh_write_index;

    // ID 8, INTEGER (1..64): the antennas the row holds.
    if (nof_ports > 0 && !set_optional(cell->nRxAnt, nof_ports)) {
        return false;
    }

    // ID 1000: the gNB's uplink symbols of this slot. Always set; 0 is valid
    // (no uplink symbol).
    return set_optional(cell->validSymbolMask, valid_symbol_mask & 0x3FFFu);
}

bool fill_stream(L1KPM_StreamDescriptor_t& d, const StreamInfo& stream, const IqStreamGeometry& iq)
{
    d.telemetryId = stream.telemetry_id;
    if (!set_octets(d.streamId, stream.stream_id) || !set_octets(d.dataType, stream.data_type)) {
        return false;
    }

    if (!is_served(stream.telemetry_id)) {
        // The delivery is present only for an available stream.
        d.status = L1KPM_StreamStatus_unavailable;
        return true;
    }

    d.status   = L1KPM_StreamStatus_available;
    d.delivery = static_cast<L1KPM_Delivery_t*>(std::calloc(1, sizeof(L1KPM_Delivery_t)));
    if (d.delivery == nullptr) {
        return false;
    }
    if (stream.telemetry_id == kTelemetryIqSamples) {
        d.delivery->present = L1KPM_Delivery_PR_sharedMemory;
        auto& shm           = d.delivery->choice.sharedMemory;
        shm.memorySizeBytes = iq.buffer_bytes;
        shm.numRows         = iq.num_rows;
        return set_octets(shm.memoryKey, iq.shm_name);
    }
    d.delivery->present = L1KPM_Delivery_PR_protocolData;
    return set_octets(d.delivery->choice.protocolData.keyword, stream.stream_id);
}

}  // namespace

bool encode_iq_indication_aper(uint64_t timestamp_ns,
                               uint16_t sfn,
                               uint16_t slot,
                               const std::string& shm_name,
                               uint8_t fh_buffer_index,
                               uint32_t fh_write_index,
                               uint16_t nof_ports,
                               uint16_t valid_symbol_mask,
                               std::vector<uint8_t>& out)
{
    L1KPM_Indication_t ind;
    std::memset(&ind, 0, sizeof(ind));

    const bool ok = fill_indication(ind, timestamp_ns, sfn, slot, shm_name, fh_buffer_index, fh_write_index,
                                    nof_ports, valid_symbol_mask) &&
                    encode_aper(asn_DEF_L1KPM_Indication, &ind, kIndicationMaxBytes, out);

    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_L1KPM_Indication, &ind);
    return ok;
}

bool encode_ran_function_data_aper(const IqStreamGeometry& iq, std::vector<uint8_t>& out)
{
    L1KPM_RanFunctionData_t rfd;
    std::memset(&rfd, 0, sizeof(rfd));

    rfd.version = LAYER1_RAN_FUNCTION_VERSION;
    bool ok = set_octets(rfd.name, LAYER1_RAN_FUNCTION_NAME) &&
              set_octets(rfd.description, LAYER1_RAN_FUNCTION_DESCRIPTION);

    for (const StreamInfo& stream : kStreams) {
        if (!ok) {
            break;
        }
        auto* d = static_cast<L1KPM_StreamDescriptor_t*>(std::calloc(1, sizeof(L1KPM_StreamDescriptor_t)));
        if (d == nullptr || ASN_SEQUENCE_ADD(&rfd.streams.list, d) != 0) {
            std::free(d);
            ok = false;
            break;
        }
        ok = fill_stream(*d, stream, iq);
    }

    ok = ok && encode_aper(asn_DEF_L1KPM_RanFunctionData, &rfd, kRanFunctionDataMaxBytes, out);

    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_L1KPM_RanFunctionData, &rfd);
    return ok;
}

bool encode_ran_function_data_json(const IqStreamGeometry& iq, std::vector<uint8_t>& out)
{
    // ordered_json keeps the ASN.1 field order; plain json would sort the keys.
    nlohmann::ordered_json streams = nlohmann::ordered_json::array();
    for (const StreamInfo& stream : kStreams) {
        nlohmann::ordered_json d = {
            {"telemetryIdentifier", stream.telemetry_id},
            {"stream_id",           stream.stream_id},
            {"data_type",           stream.data_type},
        };
        if (!is_served(stream.telemetry_id)) {
            d["status"] = "unavailable";
        } else if (stream.telemetry_id == kTelemetryIqSamples) {
            d["status"]          = "available";
            d["delivery_method"] = {
                {"transport_type", "shared_memory"},
                {"shared_memory_info", {
                    {"memory_key",        iq.shm_name},
                    {"memory_size_bytes", iq.buffer_bytes},
                    {"num_rows",          iq.num_rows},
                }},
            };
        } else {
            d["status"]          = "available";
            d["delivery_method"] = {
                {"transport_type", "protocolData"},
                {"keyword",        stream.stream_id},
            };
        }
        streams.push_back(std::move(d));
    }

    nlohmann::ordered_json j = {
        {"name",        LAYER1_RAN_FUNCTION_NAME},
        {"version",     LAYER1_RAN_FUNCTION_VERSION},
        {"description", LAYER1_RAN_FUNCTION_DESCRIPTION},
        {"streams",     std::move(streams)},
    };
    const std::string s = j.dump();
    out.assign(s.begin(), s.end());
    return true;
}

}  // namespace e3sm_layer1
