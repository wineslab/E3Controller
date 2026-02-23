/*
 * ecpri_iq_handler.cpp
 *
 * Handler for eCPRI I/Q sample data from the ecpri_iq_collect codelet.
 * Logs received I/Q sample metadata and encodes it using ASN.1 APER
 * into Spectrum-IQDataIndication.
 */

#include "ecpri_iq_handler.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "Spectrum-IQDataIndication.h"
#include "per_encoder.h"
}

EcpriIqHandler::EcpriIqHandler(const struct jbpf_io_stream_id& sid)
{
    std::memcpy(&sid_, &sid, sizeof(struct jbpf_io_stream_id));
}

void EcpriIqHandler::handle(void** bufs, int num_bufs)
{
    for (int i = 0; i < num_bufs; i++) {
        auto* sample = static_cast<struct iq_sample_data*>(bufs[i]);
        total_samples_received_++;

        std::printf("[EcpriIQ] #%lu | ts=%lu dir=%s frame=%u sf=%u slot=%u sym=%u "
                    "sec=%u prb_start=%u prb_num=%u comp=%u iq_w=%u payload=%u bytes\n",
                    total_samples_received_,
                    sample->timestamp,
                    sample->direction == 0 ? "DL" : "UL",
                    sample->frame_id,
                    sample->subframe_id,
                    sample->slot_id,
                    sample->symbol_id,
                    sample->section_id,
                    sample->start_prbu,
                    sample->num_prbu,
                    sample->comp_method,
                    sample->iq_width,
                    sample->payload_size);

        // --- ASN.1 APER encoding ---
        Spectrum_IQDataIndication_t asn_msg;
        std::memset(&asn_msg, 0, sizeof(asn_msg));

        // Fill iqSamples (OCTET STRING) from the raw payload
        uint16_t iq_len = sample->payload_size;
        if (iq_len > MAX_IQ_PAYLOAD_BYTES) {
            iq_len = MAX_IQ_PAYLOAD_BYTES;
        }
        OCTET_STRING_fromBuf(&asn_msg.iqSamples,
                             reinterpret_cast<const char*>(sample->iq_payload),
                             iq_len);

        // Fill sampleCount
        asn_msg.sampleCount = static_cast<long>(sample->num_prbu);

        // Fill optional timestamp
        long ts_val = static_cast<long>(sample->timestamp / 1000000000ULL); // ns -> seconds
        asn_msg.timestamp = &ts_val;

        // Encode to APER (Aligned PER)
        asn_enc_rval_t enc_rval = aper_encode_to_buffer(
            &asn_DEF_Spectrum_IQDataIndication,
            nullptr,  // constraints (use defaults)
            &asn_msg,
            encode_buf_.data(),
            encode_buf_.size());

        if (enc_rval.encoded > 0) {
            // enc_rval.encoded is in bits for PER; convert to bytes
            size_t encoded_bytes = (enc_rval.encoded + 7) / 8;
            std::printf("[EcpriIQ] APER encoded: %zu bytes\n", encoded_bytes);
        } else {
            std::fprintf(stderr, "[EcpriIQ] APER encoding failed at %s\n",
                         enc_rval.failed_type ? enc_rval.failed_type->name : "unknown");
        }

        // Clean up dynamically allocated ASN.1 members (iqSamples buffer)
        // Do NOT free timestamp since it points to a stack variable
        asn_msg.timestamp = nullptr;
        ASN_STRUCT_RESET(asn_DEF_Spectrum_IQDataIndication, &asn_msg);
    }
}

const char* EcpriIqHandler::name() const
{
    return "EcpriIqHandler";
}

const struct jbpf_io_stream_id& EcpriIqHandler::stream_id() const
{
    return sid_;
}
