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
