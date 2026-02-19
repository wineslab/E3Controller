/*
 * ecpri_iq_handler.h
 *
 * Handler for eCPRI I/Q sample data from the ecpri_iq_collect codelet.
 */

#ifndef E3_ECPRI_IQ_HANDLER_H
#define E3_ECPRI_IQ_HANDLER_H

#include "e3_data_handler.h"
#include "ecpri_iq_data.h"

class EcpriIqHandler : public E3DataHandler {
public:
    /*
     * Construct with the stream_id that matches the codelet's output_map.
     * This stream_id is assigned when the codelet is loaded via jbpf_lcm_cli.
     */
    explicit EcpriIqHandler(const struct jbpf_io_stream_id& sid);

    void handle(void** bufs, int num_bufs) override;
    const char* name() const override;
    const struct jbpf_io_stream_id& stream_id() const override;

private:
    struct jbpf_io_stream_id sid_;
    uint64_t total_samples_received_ = 0;
};

#endif /* E3_ECPRI_IQ_HANDLER_H */
