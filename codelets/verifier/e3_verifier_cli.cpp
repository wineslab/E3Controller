/*
 * e3_verifier_cli — PREVAIL verifier for E3Controller's codelets.
 *
 * Replaces the SDK image's `srsran_verifier_cli`, which only models
 * `jbpf_ran_ofh_ctx` (27 B) and therefore cannot verify the per-slot codelet
 * at all ("Upper bound must be at most 27").
 *
 * WHY THIS MATTERS: the gNB never verifies. jbpf's load path is
 * ubpf_load_elf_ex + ubpf_compile (external/jbpf/src/core/jbpf.c:968,976),
 * PREVAIL is not on it, and ubpf's JIT emits no memory bounds checks at all
 * (only the interpreter has them, 3p/ubpf/vm/ubpf_vm.c:1724). So this binary,
 * run at build time, is the ONLY memory-safety gate that will ever execute on
 * these codelets. codelets/Makefile.common treats its exit status as fatal.
 *
 * Registers, on top of jbpf's built-in program/helper/map tables:
 *   - program type jbpf_ran_ofh  (ecpri_iq_samples)
 *   - program type jbpf_ran_slot (uplink_slot_samples)
 *   - helper prototypes for the two E3 helpers in jbpf_e3_ids.h
 *
 * Usage: e3_verifier_cli <codelet.o> [section] [--asm <out.asm>]
 * Exit:  0 = verified, 1 = failed (matching jbpf_verifier_cli's `return !res`).
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jbpf_verifier.hpp"
#include "spec_type_descriptors.hpp"

extern "C" {
#include "jbpf_defs.h"
#include "jbpf_helper_api_defs_ext.h"
#include "jbpf_srsran_contexts.h" /* imported from ocudu — see Makefile.defs */
}

#include "jbpf_e3_ids.h"

/* ------------------------------------------------------------------ *
 *  Context descriptors, derived from the ocudu contract header
 * ------------------------------------------------------------------ */

/*
 * CAUTION: PREVAIL's context `size` is the UNPADDED extent — the offset of the
 * last field plus its size — NOT sizeof(). Getting this wrong silently changes
 * what the verifier considers an in-bounds ctx read.
 *
 * Evidence, both directions:
 *   - jbpf's own extension test uses 31 for a struct whose sizeof() is 32
 *     (jbpf_tests/verifier/jbpf_verifier_extension_test.cpp).
 *   - the error we currently get for jbpf_ran_ofh_ctx says "at most 27",
 *     while sizeof(struct jbpf_ran_ofh_ctx) is 32.
 *
 * So: offsetof(last field) + sizeof(last field). Deriving it instead of
 * hard-coding it means a field added on the RAN side breaks this build rather
 * than silently verifying the codelet against a stale layout.
 */
#define CTX_EXTENT(type, last_field) \
    (static_cast<int>(offsetof(type, last_field) + sizeof(static_cast<type*>(nullptr)->last_field)))

/*
 * The `meta` slot is deliberately -1 ("no metadata region") for BOTH contexts,
 * even though each struct has a field literally named `meta_data`.
 *
 * PREVAIL's `meta` means an XDP-style packet-metadata POINTER: it models reads
 * at that offset as a pointer type, and -1 is its documented absent sentinel
 * (`crab/ebpf_domain.cpp:1968` guards on `meta >= 0`;
 * `ebpf_yaml.cpp:217` uses `{64, 0, 8, -1}`).
 *
 * Neither RAN hook supplies packet metadata. Worse, on the eCPRI path
 * `meta_data` is explicitly REPURPOSED to carry the gNB's CLOCK_REALTIME
 * hand-off timestamp — see the comment on the field in ocudu's
 * jbpf_srsran_contexts.h. Declaring it as `meta` makes the verifier type that
 * read as a pointer, so the codelet's perfectly legitimate
 * `out->timestamp = ctx->meta_data` is rejected with:
 *
 *   Only numbers can be stored to externally-visible regions
 *   (r0.type != stack -> r2.type == number)
 *
 * Observed exactly that. With -1 the read is a plain scalar, which is what the
 * field actually holds.
 *
 * Note also what that field comment says: meta_data was reused *because*
 * "extending the struct would require rebuilding srsran_verifier_cli from
 * source". This tool removes that constraint — so adding a proper `gnb_ts_ns`
 * field to jbpf_ran_ofh_ctx (as jbpf_ran_slot_ctx already has) is now possible
 * and would be cleaner than the reuse. Left as a follow-up: it is a RAN-side
 * contract change and invalidates every compiled codelet.
 */
static constexpr int kNoMetaRegion = -1;

static constexpr ebpf_context_descriptor_t g_ran_ofh_descr = {
    CTX_EXTENT(struct jbpf_ran_ofh_ctx, direction),
    static_cast<int>(offsetof(struct jbpf_ran_ofh_ctx, data)),
    static_cast<int>(offsetof(struct jbpf_ran_ofh_ctx, data_end)),
    kNoMetaRegion,
};

static constexpr ebpf_context_descriptor_t g_ran_slot_descr = {
    CTX_EXTENT(struct jbpf_ran_slot_ctx, direction),
    static_cast<int>(offsetof(struct jbpf_ran_slot_ctx, data)),
    static_cast<int>(offsetof(struct jbpf_ran_slot_ctx, data_end)),
    kNoMetaRegion,
};

/* Tripwires: if either fires, the RAN-side ctx layout changed. Re-derive the
 * expectation deliberately rather than just bumping the number — a moved
 * data/data_end/meta_data offset also invalidates every compiled codelet. */
static_assert(g_ran_ofh_descr.size == 27, "jbpf_ran_ofh_ctx layout changed (expected 27 B extent)");
static_assert(g_ran_slot_descr.size == 47, "jbpf_ran_slot_ctx layout changed (expected 47 B extent)");
static_assert(g_ran_slot_descr.data == 0 && g_ran_slot_descr.end == 8, "jbpf_ran_slot_ctx data/data_end moved");
static_assert(g_ran_ofh_descr.data == 0 && g_ran_ofh_descr.end == 8, "jbpf_ran_ofh_ctx data/data_end moved");

/* ------------------------------------------------------------------ *
 *  Helper prototypes
 * ------------------------------------------------------------------ */

/*
 * Field order is {name, return_type, argument_type[5], reallocate_packet,
 * context_descriptor} — see 3p/ebpf-verifier/src/helpers.hpp. Positional
 * initialisation (rather than designators) keeps this compiling under a strict
 * -std=c++17, which is what E3Controller sets.
 *
 * context_descriptor is left null so both helpers are callable from any
 * program type; jbpf_verifier_is_helper_usable() only enforces a match when
 * it is non-null.
 */

/* jbpf_e3_attach(cfg, cfg_len) */
static const EbpfHelperPrototype g_e3_attach_proto = {
    "e3_attach",
    EBPF_RETURN_TYPE_INTEGER,
    {
        EBPF_ARGUMENT_TYPE_PTR_TO_READABLE_MEM,
        EBPF_ARGUMENT_TYPE_CONST_SIZE_OR_ZERO,
        EBPF_ARGUMENT_TYPE_DONTCARE,
        EBPF_ARGUMENT_TYPE_DONTCARE,
        EBPF_ARGUMENT_TYPE_DONTCARE,
    },
    false,
    nullptr,
};

/*
 * jbpf_e3_publish_slot(src, src_len, sel, sel_len)
 *
 * Only the SOURCE needs verifier-proven bounds. The destination is not an
 * argument at all — the helper derives the row address from geometry it owns,
 * so a codelet cannot forge it.
 *
 * The (ptr, size) pairing is positional and REQUIRED: PREVAIL only forms a
 * checked pair when the size register immediately follows the pointer register
 * (3p/ebpf-verifier/src/asm_unmarshal.cpp:337-343). This is why the old
 * jbpf_native_memcpy(dst, src, len) signature could never be verified, even
 * with a prototype registered.
 */
static const EbpfHelperPrototype g_e3_publish_slot_proto = {
    "e3_publish_slot",
    EBPF_RETURN_TYPE_INTEGER,
    {
        EBPF_ARGUMENT_TYPE_PTR_TO_READABLE_MEM,
        EBPF_ARGUMENT_TYPE_CONST_SIZE_OR_ZERO,
        EBPF_ARGUMENT_TYPE_PTR_TO_READABLE_MEM,
        EBPF_ARGUMENT_TYPE_CONST_SIZE_OR_ZERO,
        EBPF_ARGUMENT_TYPE_DONTCARE,
    },
    false,
    nullptr,
};

/* ------------------------------------------------------------------ *
 *  Registration
 * ------------------------------------------------------------------ */

/*
 * Poison entry for helper IDs that nothing has registered.
 *
 * WHY THIS IS NEEDED — jbpf bug, worth reporting upstream:
 *
 * jbpf_verifier_register_helper() grows its table with
 * `prototypes.resize(helper_id + 1)` (specs/jbpf_prototypes.cpp:271). Registering
 * anything at CUSTOM_HELPER_START_ID (32) therefore leaves IDs 19..31
 * default-constructed, i.e. zeroed — and a zeroed entry has
 * context_descriptor == nullptr, which is precisely what
 * jbpf_verifier_is_helper_usable() treats as "usable from any program type".
 *
 * So a codelet calling an unregistered helper in that gap gets past the
 * usability check, reaches `res.name = proto.name` in
 * 3p/ebpf-verifier/src/asm_unmarshal.cpp:310 with proto.name == nullptr, and
 * constructs a std::string from a null pointer: SIGSEGV, no diagnostic.
 *
 * Observed exactly that on the current uplink_slot_collect.o, which still calls
 * the old jbpf_native_memcpy stub at ID 19.
 *
 * EBPF_RETURN_TYPE_UNSUPPORTED is handled properly by makeCall(), which throws
 * `Unsupported function: <name>` — so filling the gap turns a crash into a
 * message that names the offending helper.
 */
static EbpfHelperPrototype
make_unregistered_proto(int id)
{
    /* Names must outlive registration (EbpfHelperPrototype::name is a raw
     * const char*), and we want the ID in the message so the diagnostic points
     * at the actual call. Deque-like stable storage: a static list that is only
     * ever appended to before use, never reallocated afterwards. */
    static std::vector<std::unique_ptr<std::string>> names;
    names.push_back(std::make_unique<std::string>("<unregistered helper " + std::to_string(id) + ">"));

    EbpfHelperPrototype p{};
    p.name              = names.back()->c_str();
    p.return_type       = EBPF_RETURN_TYPE_UNSUPPORTED;
    p.argument_type[0]  = EBPF_ARGUMENT_TYPE_DONTCARE;
    p.argument_type[1]  = EBPF_ARGUMENT_TYPE_DONTCARE;
    p.argument_type[2]  = EBPF_ARGUMENT_TYPE_DONTCARE;
    p.argument_type[3]  = EBPF_ARGUMENT_TYPE_DONTCARE;
    p.argument_type[4]  = EBPF_ARGUMENT_TYPE_DONTCARE;
    p.reallocate_packet = false;
    p.context_descriptor = nullptr;
    return p;
}

static void
register_e3_extensions()
{
    /* Program types. Lookup is by ELF section prefix, first match wins
     * (jbpf_platform.cpp:45-58). "jbpf_ran_ofh" and "jbpf_ran_slot" are not
     * prefixes of one another, so registration order is not load-bearing. */
    EbpfProgramType ran_ofh;
    ran_ofh.name                   = "jbpf_ran_ofh";
    ran_ofh.context_descriptor     = &g_ran_ofh_descr;
    ran_ofh.platform_specific_data = JBPF_PROG_TYPE_RAN_OFH;
    ran_ofh.section_prefixes       = {JBPF_E3_SEC_RAN_OFH};
    ran_ofh.is_privileged          = false;
    jbpf_verifier_register_program_type(JBPF_PROG_TYPE_RAN_OFH, ran_ofh);

    EbpfProgramType ran_slot;
    ran_slot.name                   = "jbpf_ran_slot";
    ran_slot.context_descriptor     = &g_ran_slot_descr;
    ran_slot.platform_specific_data = JBPF_E3_PROG_TYPE_RAN_SLOT;
    ran_slot.section_prefixes       = {JBPF_E3_SEC_RAN_SLOT};
    ran_slot.is_privileged          = false;
    jbpf_verifier_register_program_type(JBPF_E3_PROG_TYPE_RAN_SLOT, ran_slot);

    /* Close the gap between jbpf's built-ins and our custom range BEFORE
     * registering ours, so no zeroed entry is ever reachable. See the comment
     * on g_unregistered_proto. JBPF_NUM_HELPERS_MAX is one past the last
     * built-in, so it is the correct lower bound whichever jbpf we build
     * against. */
    static_assert(JBPF_NUM_HELPERS_MAX <= JBPF_E3_ATTACH_ID,
                  "jbpf built-in helper range now overlaps the E3 custom range");
    for (int id = JBPF_NUM_HELPERS_MAX; id < JBPF_E3_ATTACH_ID; ++id) {
        jbpf_verifier_register_helper(id, make_unregistered_proto(id));
    }

    /* Helpers. */
    jbpf_verifier_register_helper(JBPF_E3_ATTACH_ID, g_e3_attach_proto);
    jbpf_verifier_register_helper(JBPF_E3_PUBLISH_SLOT_ID, g_e3_publish_slot_proto);
}

/* ------------------------------------------------------------------ */

static void
usage(const char* argv0)
{
    std::fprintf(stderr,
                 "usage: %s <codelet.o> [section] [--asm <out.asm>]\n"
                 "\n"
                 "  Verifies a codelet against the E3 program types and helpers.\n"
                 "  Exit status 0 means verified; 1 means it did not verify.\n",
                 argv0);
}

int
main(int argc, char** argv)
{
    const char* path    = nullptr;
    const char* section = nullptr;
    const char* asmfile = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--asm") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 1;
            }
            asmfile = argv[i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (path == nullptr) {
            path = argv[i];
        } else if (section == nullptr) {
            section = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (path == nullptr) {
        usage(argv[0]);
        return 1;
    }

    register_e3_extensions();

    /* jbpf_verify() lets exceptions from the unmarshaller escape (e.g. an
     * unregistered helper ID, or a malformed object). Catch them so the tool
     * reports a diagnosis and exit status 1 rather than std::terminate. */
    jbpf_verifier_result_t result{};
    try {
        result = jbpf_verify(path, section, asmfile);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAILED verification: %s: %s\n", path, e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "FAILED verification: %s: unknown error\n", path);
        return 1;
    }

    if (!result.verification_pass) {
        std::fprintf(stderr, "FAILED verification: %s: %s\n", path, result.err_msg);
        return 1;
    }

    std::printf(
        "verified: %s (%.3fs, terminates within %lu instructions)\n", path, result.runtime_seconds,
        result.max_instruction_count);
    return 0;
}
