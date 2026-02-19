/*
 * e3_data_handler.h
 *
 * Abstract interface for data handlers in E3Manager.
 * Each handler processes data from a specific codelet output stream,
 * identified by its jbpf_io_stream_id.
 */

#ifndef E3_DATA_HANDLER_H
#define E3_DATA_HANDLER_H

#include <cstddef>
#include <string>

#include "jbpf_io_channel_defs.h"

class E3DataHandler {
public:
    virtual ~E3DataHandler() = default;

    /* Process a batch of data buffers from a codelet output channel */
    virtual void handle(void** bufs, int num_bufs) = 0;

    /* Human-readable name for logging */
    virtual const char* name() const = 0;

    /* The stream_id this handler is registered for */
    virtual const struct jbpf_io_stream_id& stream_id() const = 0;
};

#endif /* E3_DATA_HANDLER_H */
