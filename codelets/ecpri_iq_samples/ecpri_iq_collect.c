/*
 * ecpri_iq_collect.c
 *
 * This codelet extracts I/Q samples from eCPRI user-plane packets.
 * It attaches to the capture_xran_packet hook and processes each
 * eCPRI IQ Data message, extracting:
 *   - Radio application metadata (frame, subframe, slot, symbol, section, PRBs)
 *   - Compression info (method, IQ width)
 *   - Raw compressed I/Q payload bytes
 *
 * A configurable sampling parameter controls output rate:
 *   - sampling_period = 0: output every packet (no sampling)
 *   - sampling_period = N: output every Nth symbol (skip N-1, send 1)
 */

#include "jbpf_defs.h"
#include "jbpf_helper.h"
#include "../utils/misc_utils.h"
#include "../utils/net_utils.h"
#include "../xran_packets/xran_format.h"
#include "jbpf_srsran_contexts.h"
#include "ecpri_iq_data.h"

/* ---- Maps ---- */

/* Ringbuf output map for I/Q sample data */
jbpf_ringbuf_map(output_map, struct iq_sample_data, 64);

/* Temporary storage for building the output struct (verifier requires map-backed memory) */
struct jbpf_load_map_def SEC("maps") output_tmp_map = {
    .type = JBPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(int),
    .value_size = sizeof(struct iq_sample_data),
    .max_entries = 1,
};

/* Sampling configuration: sampling_period (0 = all packets, N = every Nth symbol) */
struct jbpf_load_map_def SEC("maps") sampling_config = {
    .type = JBPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(int),
    .value_size = sizeof(uint32_t),
    .max_entries = 1,
};

/* Symbol counter for sampling */
struct jbpf_load_map_def SEC("maps") symbol_counter = {
    .type = JBPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(int),
    .value_size = sizeof(uint32_t),
    .max_entries = 1,
};

/* Control input channel for PRB filter configuration from E3Controller */
jbpf_control_input_map(prb_config_map, struct prb_filter_config, 1);

/* Persistent state for PRB filter (survives across codelet invocations) */
struct jbpf_load_map_def SEC("maps") prb_filter_state = {
    .type = JBPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(int),
    .value_size = sizeof(struct prb_filter_config),
    .max_entries = 1,
};


/* ---- Constants ---- */

#define ECPRI_ETH_TYPE (0xaefe)

/* Maximum number of bytes to copy in the bounded loop.
 * Must match MAX_IQ_PAYLOAD_BYTES from ecpri_iq_data.h.
 * The loop is bounded to this value for the verifier. */
#define COPY_LOOP_MAX MAX_IQ_PAYLOAD_BYTES

/* Symbol-count filter (compile-time, edit-and-rebuild).
 * UL-only codelet. Keep the LAST NUM_SYMBOLS symbols of each slot.
 * Valid range: 4..14.
 *   4  -> threshold sym_id >= 10: S-slot (sym 10..13) passes fully,
 *         full UL slot contributes only sym 10..13.
 *   14 -> threshold sym_id >= 0:  everything passes.
 * The 4-floor is physical: the S-slot has exactly 4 UL symbols (10..13),
 * so dropping below 4 would start cutting into it. */
#define SLOT_SYMBOLS  14
#define NUM_SYMBOLS   14
#define SYMBOL_FLOOR  (SLOT_SYMBOLS - NUM_SYMBOLS)

/* Absolute-slot filter (compile-time, edit-and-rebuild).
 * At 30 kHz SCS the eCPRI radio-app header encodes slot-within-subframe
 * in slotId (0..1) and subframe (0..9); combine them into slot-in-frame
 * (0..19) via subframe*2 + slot. Set to -1 to disable and pass every
 * slot. Configured for the 7D-S-2U TDD pattern: full-UL slots are
 * {8,9,18,19}; slot 19 is the tail UL of the second half-frame. Keeping
 * one slot per frame cuts arrival rate to 100 slots/s from ~2000. */
#define TARGET_ABS_SLOT  19
#define SLOTS_PER_SUBFRAME 2  /* 30 kHz SCS, numerology mu=1 */

/* Exact-symbol filter (compile-time, edit-and-rebuild). Stacks on top
 * of TARGET_ABS_SLOT: after slot 19 is picked, keep only symbol
 * TARGET_SYMBOL_ID (0..13). Set to -1 to fall back to the range-based
 * SYMBOL_FLOOR filter (keep the last NUM_SYMBOLS symbols). Symbol 13 is
 * the tail UL symbol, chosen so the RU has finished delivering the full
 * slot's payload by the time we sample. Combined with slot 19 this
 * yields 1 codelet fire per antenna per frame = 400/s at 4T. */
#define TARGET_SYMBOL_ID 13

/* Single-antenna filter (compile-time, edit-and-rebuild). eCPRI's
 * xtc_id field carries the "physical channel ID" (a.k.a. eAxC ID); in
 * the Foxconn 4T4R config the UL antennas are ul_port_id [0,1,2,3].
 * Set to -1 to keep all antennas. Stacks on top of slot+symbol filters,
 * so with 7D-S-2U + slot 19 + symbol 13 + one antenna the arrival rate
 * collapses to 1 codelet fire per frame (100/s). */
#define TARGET_EAXC -1


/* ---- Main codelet entry ---- */

SEC("jbpf_ran_ofh")
uint64_t jbpf_main(void *state)
{
    struct jbpf_ran_ofh_ctx *ctx;
    ctx = (struct jbpf_ran_ofh_ctx *)state;
    int zero_index = 0;

    /* Capture entry timestamp for codelet -> dApp latency measurement.
     * Microseconds-mod-2^31: fits the existing 32-bit ASN.1 timestamp field
     * (Spectrum-IQDataIndication.timestamp) and wraps every ~35 min. */
    uint32_t entry_ts_us = (uint32_t)((jbpf_time_get_ns() / 1000ULL) & 0x7FFFFFFFULL);

    void *pkt_start = (void *)ctx->data;
    void *pkt_end = (void *)ctx->data_end;

    /* --- Parse Ethernet header --- */
    struct ethhdr *eh = (struct ethhdr *)pkt_start;
    if ((void *)(eh + 1) >= pkt_end) {
        return JBPF_CODELET_FAILURE;
    }

    void *next_hdr = (__u8 *)eh + sizeof(struct ethhdr);
    uint16_t ether_type = jbpf_ntohs(eh->h_proto);

    /* --- Handle optional VLAN tag --- */
    if (ether_type == ETH_P_8021Q) {
        struct vlan_hdr *vl_hdr = (struct vlan_hdr *)next_hdr;
        if ((void *)(vl_hdr + 1) >= pkt_end) {
            return JBPF_CODELET_FAILURE;
        }
        ether_type = jbpf_ntohs(vl_hdr->h_vlan_encapsulated_proto);
        next_hdr = (__u8 *)next_hdr + sizeof(struct vlan_hdr);
    }

    /* Only process eCPRI packets */
    if (ether_type != ECPRI_ETH_TYPE) {
        return JBPF_CODELET_FAILURE;
    }

    /* --- Parse eCPRI header --- */
    struct xran_ecpri_hdr *ecpri_hdr = (struct xran_ecpri_hdr *)next_hdr;
    if ((void *)(ecpri_hdr + 1) >= pkt_end) {
        return JBPF_CODELET_FAILURE;
    }
    next_hdr = (__u8 *)next_hdr + sizeof(struct xran_ecpri_hdr);

    /* Only process IQ Data messages (user plane), skip control plane */
    if (ecpri_hdr->cmnhdr.bits.ecpri_mesg_type != ECPRI_IQ_DATA) {
        return JBPF_CODELET_SUCCESS;
    }

    /* --- Single-antenna filter (early drop) ---
     * ecpri_xtc_id is __u16 in network byte order; carries the eAxC ID
     * that maps to antenna port (ul_port_id in gnb config). Filter here
     * before the app-hdr / section-hdr parses, so wrong-antenna packets
     * exit with a byte-swap + compare. */
#if TARGET_EAXC >= 0
    {
        uint16_t eaxc = jbpf_ntohs(ecpri_hdr->ecpri_xtc_id);
        if (eaxc != TARGET_EAXC) {
            return JBPF_CODELET_SUCCESS;
        }
    }
#endif

    /* --- Parse Radio Application Common Header --- */
    struct radio_app_common_hdr *app_hdr = (struct radio_app_common_hdr *)next_hdr;
    if ((void *)(app_hdr + 1) >= pkt_end) {
        return JBPF_CODELET_FAILURE;
    }
    next_hdr = (__u8 *)next_hdr + sizeof(struct radio_app_common_hdr);

    /* --- Symbol/slot filters (early drop) ---
     * sf_slot_sym is 16 bits network-order: [subframeId:4][slotId:6][symbolId:6].
     * Order matters here: we drop wrong-slot packets FIRST so a full-frame
     * of DL/other UL-slot chatter is discarded before any map lookups or
     * verifier-heavier work below. For 7D-S-2U keeping only slot 19 that
     * gets us from ~20 slots/frame of UL noise down to 1. */
    uint16_t sf_slot_sym = jbpf_ntohs(app_hdr->sf_slot_sym.value);
    uint16_t subframe_id_early = (sf_slot_sym >> 12) & 0xF;
    uint16_t slot_id_early     = (sf_slot_sym >> 6)  & 0x3F;
    uint16_t abs_slot          = subframe_id_early * SLOTS_PER_SUBFRAME + slot_id_early;
#if TARGET_ABS_SLOT >= 0
    if (abs_slot != TARGET_ABS_SLOT) {
        return JBPF_CODELET_SUCCESS;
    }
#endif
    uint16_t symbol_id = sf_slot_sym & 0x3F;
#if TARGET_SYMBOL_ID >= 0
    /* Exact-symbol match. Stacks on the slot filter above: any packet
     * not carrying symbol TARGET_SYMBOL_ID of slot TARGET_ABS_SLOT gets
     * dropped before we touch any maps. */
    if (symbol_id != TARGET_SYMBOL_ID) {
        return JBPF_CODELET_SUCCESS;
    }
#else
    /* Fallback: range-based filter (keep the last NUM_SYMBOLS of the slot). */
    if (symbol_id < SYMBOL_FLOOR) {
        return JBPF_CODELET_SUCCESS;
    }
#endif

    /* --- Parse Data Section Header --- */
    struct data_section_hdr *data_hdr = (struct data_section_hdr *)next_hdr;
    if ((void *)(data_hdr + 1) >= pkt_end) {
        return JBPF_CODELET_FAILURE;
    }
    next_hdr = (__u8 *)next_hdr + sizeof(struct data_section_hdr);

    /* --- Parse Dynamic Compression Header (2 bytes) ---
     * srsRAN config has enable_ul/dl_static_compr_hdr: false,
     * so the udCompHdr (1 byte) + reserved (1 byte) are present.
     * udCompHdr format: bits [7:4] = iq_width, bits [3:0] = comp_method */
    struct data_section_compression_hdr *dsc_hdr = (struct data_section_compression_hdr *)next_hdr;
    if ((void *)(dsc_hdr + 1) >= pkt_end) {
        return JBPF_CODELET_FAILURE;
    }
    next_hdr = (__u8 *)next_hdr + sizeof(struct data_section_compression_hdr);

    /* --- Parse metadata needed for PRB filtering --- */

    /* data_section_hdr is 32 bits in network byte order:
    *   [sect_id:12][rb:1][sym_inc:1][start_prbu:10][num_prbu:8]
    * Must byte-swap first. */
    uint32_t sec_bits = jbpf_ntohl(data_hdr->fields.all_bits);
    uint16_t num_prbu = sec_bits & 0xFF;
    if (num_prbu == 0)
        num_prbu = 273;

    /* --- PRB filter logic --- */
    /* Check for new config from E3Controller via control input channel */
    struct prb_filter_config new_cfg;
    if (jbpf_control_input_receive(&prb_config_map, (void *)&new_cfg, sizeof(new_cfg)) == 1) {
        struct prb_filter_config *state = (struct prb_filter_config *)jbpf_map_lookup_elem(&prb_filter_state, &zero_index);
        if (state) {
            state->expected_num_prbu = new_cfg.expected_num_prbu;
        }
    }

    /* Apply PRB filter: if configured, skip packets that don't match expected PRB count.
     * When expected_num_prbu == 0 (not yet configured), pass all packets through. */
    struct prb_filter_config *filter = (struct prb_filter_config *)jbpf_map_lookup_elem(&prb_filter_state, &zero_index);
    if (filter && filter->expected_num_prbu > 0 && num_prbu != filter->expected_num_prbu) {
        return JBPF_CODELET_SUCCESS;
    }

    /* --- Sampling logic --- */
    uint32_t *period = (uint32_t *)jbpf_map_lookup_elem(&sampling_config, &zero_index);
    if (!period) {
        return JBPF_CODELET_FAILURE;
    }

    uint32_t *counter = (uint32_t *)jbpf_map_lookup_elem(&symbol_counter, &zero_index);
    if (!counter) {
        return JBPF_CODELET_FAILURE;
    }

    uint32_t sampling_period = *period;

    if (sampling_period > 0) {
        uint32_t cnt = *counter;
        cnt++;
        *counter = cnt;

        /* Only output when counter reaches sampling_period, then reset */
        if (cnt < sampling_period) {
            return JBPF_CODELET_SUCCESS;
        }
        /* Reset counter */
        *counter = 0;
    }

    /* --- Get output buffer from temp map --- */
    struct iq_sample_data *out = (struct iq_sample_data *)jbpf_map_lookup_elem(&output_tmp_map, &zero_index);
    if (!out) {
        return JBPF_CODELET_FAILURE;
    }

    /* RAN-side hand-off timestamp piggybacked on ctx->meta_data by the
     * hook_capture_xran_packet call site (ofh_message_receiver_impl).
     * Read into a dedicated output field so the controller-side ABI is
     * self-documenting. Anchor for gnb_to_codelet_us; paired with
     * codelet_ts_ns below (stamped just before jbpf_ringbuf_output).
     * Held in meta_data because the SDK verifier's OFH ctx descriptor is
     * a fixed 27-byte layout — see jbpf_ran_ofh_ctx comment. */
    out->gnb_ts_ns = ctx->meta_data;
    out->timestamp = entry_ts_us;
    out->direction = ctx->direction;
    out->frame_id = app_hdr->frame_id;  /* single byte, no endianness issue */

    /* sf_slot_sym already parsed above for the symbol-percentage filter. */
    out->subframe_id = (sf_slot_sym >> 12) & 0xF;
    out->slot_id     = (sf_slot_sym >> 6)  & 0x3F;
    out->symbol_id   = symbol_id;

    out->section_id = (sec_bits >> 20) & 0xFFF;
    out->start_prbu = (sec_bits >> 8)  & 0x3FF;
    out->num_prbu = num_prbu;

    /* Read compression parameters from the parsed header */
    out->comp_method = dsc_hdr->ud_comp_hdr.ud_comp_meth;
    out->iq_width = dsc_hdr->ud_comp_hdr.ud_iq_width;

    /* --- Copy I/Q payload --- */
    /* next_hdr points to the start of BFP I/Q data (after 2-byte
     * dynamic compression header).
     * Each PRB: 1-byte exponent + packed 9-bit I/Q samples.
     * We copy the raw compressed bytes as-is. */
    void *iq_start = next_hdr;
    uint64_t avail = (uint64_t)pkt_end - (uint64_t)iq_start;

    uint16_t copy_size = (uint16_t)avail;
    if (copy_size > MAX_IQ_PAYLOAD_BYTES) {
        copy_size = MAX_IQ_PAYLOAD_BYTES;
    }
    out->payload_size = copy_size;

    /* Bounded copy loop for the verifier */
    __u8 *src = (__u8 *)iq_start;
    for (uint16_t i = 0; i < COPY_LOOP_MAX; i++) {
        if (i >= copy_size) {
            break;
        }
        /* Bounds check each byte against packet end */
        if ((void *)(src + i + 1) > pkt_end) {
            break;
        }
        out->iq_payload[i] = src[i];
    }

    /* Codelet-side timestamp captured just before jbpf_ringbuf_output so
     * (codelet_ts_ns - gnb_ts_ns) attributes hook -> codelet-dispatch cost,
     * matching the uplink_slot_samples pipeline's stage schema.
     * Same clock domain as ctx->gnb_ts_ns (CLOCK_REALTIME). */
    out->codelet_ts_ns = jbpf_time_get_ns();

    /* --- Output --- */
    int ret = jbpf_ringbuf_output(&output_map, (void *)out, sizeof(struct iq_sample_data));
    jbpf_map_clear(&output_tmp_map);

    if (ret < 0) {
        return JBPF_CODELET_FAILURE;
    }

    return JBPF_CODELET_SUCCESS;
}