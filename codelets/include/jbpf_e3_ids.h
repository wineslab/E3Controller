/*
 * jbpf_e3_ids.h — E3Controller's jbpf extension IDs.
 *
 * SINGLE SOURCE OF TRUTH for the helper and program-type IDs that
 * E3Controller's codelets use. This header is included by:
 *
 *   1. the codelets            (to declare the helper stubs),
 *   2. codelets/verifier/e3_verifier_cli.cpp  (to register prototypes),
 *   3. the gNB-side helper implementation in ocudu-e3
 *      (to register the implementations via jbpf_register_helper_function).
 *
 * If (1)/(2) and (3) ever disagree, a codelet that verifies cleanly will
 * still fail to load with "call to nonexistent function <id>". Keeping all
 * three on this header is what prevents that class of skew.
 *
 * IDs are allocated from jbpf's documented extension ranges
 * (external/jbpf/src/common/jbpf_helper_api_defs_ext.h):
 *
 *   CUSTOM_HELPER_START_ID  = 32   (MAX_HELPER_FUNC = 64)
 *   CUSTOM_PROGRAM_START_ID = 8
 *
 * We never edit jbpf's core `enum jbpf_helper_type`. Doing so takes an ID
 * that upstream may later allocate to something else, which would silently
 * bind a codelet call to the wrong native function.
 */

#ifndef JBPF_E3_IDS_H
#define JBPF_E3_IDS_H

#include "jbpf_helper_api_defs_ext.h"

/* ---- Helper IDs ---- */

/*
 * jbpf_e3_attach(const struct e3_shm_cfg *cfg, uint64_t cfg_len)
 *
 * Cold path. Attaches the gNB-side helper to the E3Controller-owned
 * /e3_ran_buffers region and caches its geometry from SharedMemoryHeader.
 */
#define JBPF_E3_ATTACH_ID (CUSTOM_HELPER_START_ID + 0)

/*
 * jbpf_e3_publish_slot(const void *src, uint64_t src_len,
 *                      const struct e3_slot_sel *sel, uint64_t sel_len)
 *
 * Hot path. Converts the cbf16 slot to the dApp's fp16 row format and
 * writes it into the attached region. Returns
 *   ((int64_t)fh_buffer_index << 32) | fh_write_index
 * or a negative value on error.
 *
 * The codelet never holds a pointer into the destination region: it is
 * derived from geometry the helper alone owns. Only `src` needs to be a
 * verifier-proven (ptr, size) pair.
 */
#define JBPF_E3_PUBLISH_SLOT_ID (CUSTOM_HELPER_START_ID + 1)

/* ---- Program type IDs ---- */

/*
 * ocudu's jbpf_srsran_contexts.h already allocates
 *   JBPF_PROG_TYPE_RAN_OFH        = CUSTOM_PROGRAM_START_ID + 0
 *   JBPF_PROG_TYPE_RAN_LAYER2     = CUSTOM_PROGRAM_START_ID + 1
 *   JBPF_PROG_TYPE_RAN_MAC_SCHED  = CUSTOM_PROGRAM_START_ID + 2
 *   JBPF_PROG_TYPE_RAN_GENERIC    = CUSTOM_PROGRAM_START_ID + 3
 *
 * The per-slot upper-PHY hook needs its own program type, because its ctx
 * (struct jbpf_ran_slot_ctx, 47 B) is larger than jbpf_ran_ofh_ctx (27 B).
 * Declaring the slot codelet as SEC("jbpf_ran_ofh") is exactly why
 * verification currently fails with "Upper bound must be at most 27".
 *
 * TODO: this belongs in ocudu's jbpf_srsran_contexts.h next to the other
 * four, since program types are part of the RAN-side contract. Defined here
 * until that upstream change lands; the value must not collide.
 */
#define JBPF_E3_PROG_TYPE_RAN_SLOT (CUSTOM_PROGRAM_START_ID + 4)

/* ELF section name the slot codelet is compiled into, and the prefix the
 * verifier matches to pick the program type above. jbpf loads codelets by
 * *function* name (JBPF_MAIN_FUNCTION_NAME via ubpf_load_elf_ex), not by
 * section, so this name is only meaningful to the verifier. */
#define JBPF_E3_SEC_RAN_SLOT "jbpf_ran_slot"
#define JBPF_E3_SEC_RAN_OFH  "jbpf_ran_ofh"

#endif /* JBPF_E3_IDS_H */
