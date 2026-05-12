#ifndef ECPRI_IQ_DATA_H
#define ECPRI_IQ_DATA_H

#include <stdint.h>

/* Maximum I/Q payload size in bytes.
 * With 9-bit BFP compression: each PRB = 1 byte exponent + 27 bytes I/Q = 28 bytes.
 * 273 PRBs max -> 7,644 bytes. 8192 covers jumbo frames.
 */
#define MAX_IQ_PAYLOAD_BYTES 8192

struct iq_sample_data {
    uint64_t timestamp;      /* Timestamp in nanoseconds */
    uint8_t  direction;      /* 0=DL, 1=UL */
    uint8_t  frame_id;       /* 3GPP frame ID (0-255, wraps every 2.56s) */
    uint8_t  comp_method;    /* Compression method: 0=none, 1=BFP, 2=block scaling, 3=mu-law, 4=modulation */
    uint8_t  iq_width;       /* Bit width per I/Q component (e.g., 9 for 9-bit BFP) */
    uint16_t subframe_id;    /* 3GPP subframe ID within 10ms frame */
    uint16_t slot_id;        /* 3GPP slot ID within subframe */
    uint16_t symbol_id;      /* OFDM symbol ID within slot */
    uint16_t section_id;     /* Section identifier */
    uint16_t start_prbu;     /* Starting PRB of this section */
    uint16_t num_prbu;       /* Number of contiguous PRBs */
    uint16_t payload_size;   /* Actual number of bytes in iq_payload */
    uint8_t  iq_payload[MAX_IQ_PAYLOAD_BYTES]; /* Raw compressed I/Q sample bytes */
};

/* Configuration for PRB-based filtering in the codelet.
 * Sent via control input channel from the E3Controller. */
struct prb_filter_config {
    uint16_t expected_num_prbu;  /* 0 = no filtering (pass all), >0 = filter to this PRB count */
};

#endif /* ECPRI_IQ_DATA_H */