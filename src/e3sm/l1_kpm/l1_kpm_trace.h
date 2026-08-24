/*
 * Stage stamps for the L1-KPM slot path.
 *
 * This header owns exactly one thing: how this repository's per-slot path maps
 * onto libe3's shared stage catalog (libe3/latrec.h). It is the whole of the
 * controller's instrumentation - E3SMLayer1::on_sample calls these and nothing
 * else, and the recording bench links this same header, so what CI measures is
 * the code that runs on the radio rather than a copy of it.
 *
 * The catalog names *operations*, not components: the same identifier is used
 * wherever that operation is performed, and which component performed it is
 * read off the ring that recorded it. So there is no E3Controller-specific
 * stage block to use, and inventing one would break every offline tool.
 *
 * Boxes covered (libe3 docs/path-a-e3-loop.md numbering), all on the forward
 * leg:
 *
 *   A1  E3 data recording   RECORD_BEGIN -> PROCESS_BEGIN
 *   A2  Processing          PROCESS_BEGIN -> ENCODE_E3SM_BEGIN
 *   A3  Encode E3SM         ENCODE_E3SM_BEGIN -> ENCODE_E3SM_DONE
 *   -   emit tail           ENCODE_E3SM_DONE -> WAIT_ENTER
 *
 * A1 and A2 are recorded HERE, from the timestamps the slot carries, rather
 * than by the gNB stamping its own ring. The gNB needs no instrumentation for
 * this: gnb_ts_ns marks A1's entry and codelet_ts_ns marks its exit, so both
 * boundaries are already on the wire by the time the slot reaches us.
 *
 *   A1 = gnb_ts_ns -> codelet_ts_ns   the data recording itself: jbpf
 *                                     dispatch, then (writer: gnb) the
 *                                     cbf16 -> fp16 convert of the whole grid
 *                                     and the row write into /e3_ran_buffers.
 *   A2 = codelet_ts_ns -> encode      getting it to the Service Model: the
 *                                     jbpf ring transit, the dispatcher poll
 *                                     and the queue wait.
 *
 * A4 onwards (E3AP encode, queuing, delivery) belong to libe3 and are stamped
 * inside the library. E3SM is this repository's box; E3AP is the library's. We
 * do not re-time the library's stages, we join to them - see bind_libe3().
 *
 * Everything here compiles to nothing when libe3 was built without
 * -DLIBE3_ENABLE_LATREC: latrec.h supplies inline no-op stubs, and because
 * LIBE3_ENABLE_LATREC is a PUBLIC compile definition on libe3::libe3 there is
 * no way for this translation unit to disagree with the library it links.
 */
#ifndef E3_SM_L1KPM_TRACE_H
#define E3_SM_L1KPM_TRACE_H

#include <libe3/latrec.h>
#include <libe3/types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace e3sm_l1kpm_trace {

/* Ring role for the thread that runs the slot path.
 *
 * The role is the component's identity: libe3's tools/latrec2csv.py maps the
 * role's first segment through its RING_COMPONENTS table to decide which
 * per-component CSV the records land in, and `e3controller` is the prefix
 * reserved for us (it files under ocudu.csv, alongside the jbpf/gNB capture
 * side of the same deployment). Do NOT shorten this to "l1_kpm": that prefix
 * is claimed by OAI and the records would be filed as OAI's. A role the table
 * does not recognise lands in other.csv.
 */
inline constexpr const char* kRingRole = "e3controller.l1_kpm";

/* Open the calling thread's ring. Idempotent, first-open-wins, and it faults
 * in the whole mapping - so call it once at the top of the thread's loop, off
 * the slot path, and never from on_sample.
 *
 * Ordering matters: libe3's queue_outbound opens a ring on demand for whatever
 * thread emits, as libe3.outbound. Since the slot path emits from this same
 * thread, if libe3 got there first the ring would carry libe3's name and every
 * record we stamp would be filed as the library's. Opening here, before the
 * first emit, is what keeps the component attribution correct.
 */
inline void open_ring()
{
    latrec_tls_open_as(kRingRole);
}

#ifdef LIBE3_ENABLE_LATREC

/* Monotone floor for this thread's ring, and how often we had to enforce it.
 *
 * A1's boundaries happened before we were called, so RECORD_BEGIN and
 * PROCESS_BEGIN are stamped with times that precede the moment we stamp them.
 * That is safe here in a way it would not have been before: the gNB hook, the
 * codelet and the dispatcher poll all read CLOCK_MONOTONIC now, which is
 * latrec's own clock, so there is no domain conversion and no rate skew to
 * absorb.
 *
 * What still has to hold is the ring's own invariant. A ring is a single-writer
 * log whose t_ns must ascend, and libe3's reader treats the ONE permitted
 * descent as the wrap point and silently rotates there - so a descent would not
 * produce an error, it would produce a plausible-looking capture cut at the
 * wrong offset. Ordering across slots is a program-order property and normally
 * has a wide margin: the jbpf hook is a synchronous inline call, so slot N's
 * codelet has returned before slot N+1 is stamped, and A1 is tens of
 * microseconds against a slot spacing of at least 500 us at 30 kHz SCS.
 *
 * Two things can still break it, so we enforce rather than assume:
 *   - a pipeline stall longer than the slot spacing, and
 *   - several RU receive threads (one per sector) feeding one worker, whose
 *     slots interleave on this one ring with no ordering between them.
 *
 * Enforcing costs a compare and a store. A non-zero clamped() means A1 is
 * understated for that many slots, which is why the count is surfaced rather
 * than left to be inferred from the data.
 */
inline thread_local uint64_t      tls_floor_ns = 0;
/* Process-wide, so the shutdown summary can read it from the main thread while
 * the worker maintains it. Touched only when a clamp actually happens, which is
 * the rare case - the floor itself is thread-local and costs a compare. */
inline std::atomic<uint64_t> g_clamped{0};

inline uint64_t monotone(uint64_t t_ns)
{
    if (t_ns <= tls_floor_ns) {
        g_clamped.fetch_add(1, std::memory_order_relaxed);
        t_ns = tls_floor_ns + 1;
    }
    tls_floor_ns = t_ns;
    return t_ns;
}

inline void stamp_at(uint64_t seq, uint8_t stage, uint64_t aux, uint64_t aux2, uint64_t t_ns)
{
    latrec_tstamp_at(seq, stage, aux, aux2, monotone(t_ns));
}

inline void stamp_now(uint64_t seq, uint8_t stage, uint64_t aux, uint64_t aux2)
{
    /* Routed through the same floor as the back-dated stamps, so the floor
     * reflects every record in the ring and not just the ones we adjust. */
    stamp_at(seq, stage, aux, aux2, latrec_tnow());
}

/* Stamps whose time had to be clamped to keep the ring ascending. Non-zero
 * means A1 is understated for that many slots. */
inline uint64_t clamped() { return g_clamped.load(std::memory_order_relaxed); }

#else  /* !LIBE3_ENABLE_LATREC */

inline void stamp_at(uint64_t, uint8_t, uint64_t, uint64_t, uint64_t) {}
inline void stamp_now(uint64_t, uint8_t, uint64_t, uint64_t) {}
inline uint64_t clamped() { return 0; }

#endif /* LIBE3_ENABLE_LATREC */

/* A1 entry: the slot's data existed and nothing had moved yet.
 *
 * aux carries the source-defined slot key, per the catalog. aux2 carries the
 * codelet's entry stamp, which splits A1 into the part that is jbpf getting to
 * the codelet and the part that is the codelet moving the data:
 *
 *   jbpf dispatch     RECORD_BEGIN.aux2 - RECORD_BEGIN t_ns
 *   convert + write   PROCESS_BEGIN t_ns - RECORD_BEGIN.aux2
 */
inline void record_begin(uint64_t seq,
                         uint32_t sfn,
                         uint16_t abs_slot,
                         uint64_t gnb_ts_ns,
                         uint64_t codelet_entry_ts_ns)
{
    stamp_at(seq,
             LATREC_RECORD_BEGIN,
             (static_cast<uint64_t>(sfn) << 16) | abs_slot,
             codelet_entry_ts_ns,
             gnb_ts_ns);
}

/* A1 exit / A2 entry: the slot's data was in place and the codelet was about to
 * submit the descriptor.
 *
 * aux carries the dispatcher's poll stamp, splitting A2 into the jbpf ring
 * transit and the queue wait:
 *
 *   ring transit   PROCESS_BEGIN.aux - PROCESS_BEGIN t_ns
 *   queue wait     ENCODE_E3SM_BEGIN t_ns - PROCESS_BEGIN.aux
 */
inline void process_begin(uint64_t seq, uint64_t codelet_ts_ns, uint64_t dispatch_ts_ns, uint32_t bytes)
{
    stamp_at(seq, LATREC_PROCESS_BEGIN, dispatch_ts_ns, bytes, codelet_ts_ns);
}

/* A2 exit / A3 entry. */
inline void encode_begin(uint64_t seq, uint32_t payload_bytes)
{
    stamp_now(seq,
              LATREC_ENCODE_E3SM_BEGIN,
              payload_bytes,
              static_cast<uint64_t>(libe3::PduType::INDICATION_MESSAGE));
}

/* A3 exit. */
inline void encode_done(uint64_t seq, std::size_t encoded_bytes)
{
    stamp_now(seq,
              LATREC_ENCODE_E3SM_DONE,
              static_cast<uint64_t>(encoded_bytes),
              static_cast<uint64_t>(libe3::PduType::INDICATION_MESSAGE));
}

/* A3 failed: the slot produced no indication. Closes the row with a reason
 * instead of leaving it looking like a lost record. */
inline void encode_failed(uint64_t seq)
{
    stamp_now(seq, LATREC_SKIPPED, 0, LATREC_SKIP_ENCODE);
}

/* Publish this slot's key to libe3, immediately before entering the library.
 *
 * This is the whole of the cross-component join: libe3 records whatever was
 * last passed here into EMIT_ENTER's aux, which latrec2csv.py surfaces as the
 * `origin_seq` column on the outbound leg. Without it the library's records
 * are unattributable to the slot that produced them.
 *
 * It is a no-op on a thread with no ring open, which is the other reason
 * open_ring() has to have run first.
 */
inline void bind_libe3(uint64_t seq)
{
    latrec_ctx_set(seq);
}

/* The emit tail: subscriber fan-out bookkeeping after the payload has gone
 * into the library. latrec2csv.py emits it as ENCODE_E3SM_DONE__WAIT_ENTER_us,
 * because (ENCODE_E3SM_DONE -> WAIT_ENTER) is a declared extra hop on the
 * source leg.
 *
 * Stamped once per slot, after the loop, so it covers every subscriber rather
 * than just the first - aux carries how many there were, which is what makes
 * the per-subscriber cost recoverable.
 */
inline void emit_tail(uint64_t seq, std::size_t subscribers)
{
    stamp_now(seq, LATREC_WAIT_ENTER, static_cast<uint64_t>(subscribers), 0);
}

}  // namespace e3sm_l1kpm_trace

#endif /* E3_SM_L1KPM_TRACE_H */
