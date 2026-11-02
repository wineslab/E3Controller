/*
 * Tests of the Spectrum SM codecs (src/e3sm/sm_spectrum/e3sm_spect_wrapper.*).
 *
 * Build with -DE3C_BUILD_TESTS=ON and run with ctest, or directly:
 *   out/bin/test_spectrum_sm
 *
 * The expected bytes come from outside this code, so a test cannot pass by
 * agreeing with itself:
 *   - IQ indication and RAN function data: encoded by the controller built from
 *     main, i.e. with the previous grammar. The two types did not change, and
 *     neither may their bytes.
 *   - dApp control envelope: derived by hand from X.691 (aligned PER), see the
 *     comment at each vector.
 *   - JSON control: the string the adaptive_cpu dApp builds
 *     (BuildPrbBlockControlJson in subcarrier_power_app.cpp).
 */

#include "e3sm/sm_spectrum/e3sm_spect_wrapper.h"

extern "C" {
#include "Spectrum-DAppControlData.h"
#include "aper_encoder.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace e3sm_spectrum;

static int g_failures = 0;

static void check(bool condition, const std::string& what)
{
    if (condition) {
        std::printf("  ok    %s\n", what.c_str());
    } else {
        std::printf("  FAIL  %s\n", what.c_str());
        g_failures++;
    }
}

static std::string hex(const std::vector<uint8_t>& bytes)
{
    std::string text;
    char        octet[4];
    for (uint8_t b : bytes) {
        std::snprintf(octet, sizeof(octet), "%02x ", b);
        text += octet;
    }
    return text;
}

static void check_bytes(const std::vector<uint8_t>& actual,
                        const std::vector<uint8_t>& expected,
                        const std::string&          what)
{
    check(actual == expected, what);
    if (actual != expected) {
        std::printf("        expected %s\n        actual   %s\n", hex(expected).c_str(), hex(actual).c_str());
    }
}

static std::vector<uint8_t> bytes_of(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

// ---------------------------------------------------------------------------
// Unchanged types: the bytes of the previous grammar
// ---------------------------------------------------------------------------

static void test_iq_indication_bytes_unchanged()
{
    std::printf("IQ indication (legacy eCPRI path)\n");

    SpectrumIQIndication indication;
    indication.iq_data      = {1, 2, 3, 4};
    indication.sample_count = 2;
    indication.timestamp    = 7;

    std::vector<uint8_t> encoded;
    check(encode_spectrum_iq_indication(indication, encoded), "encodes");
    check_bytes(encoded, {0x80, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 0x00, 0x02, 0x00, 0x07},
                "same bytes as with the previous grammar");
}

static void test_ran_function_data()
{
    std::printf("RAN function data\n");

    // Encoded by the controller built from main.
    const std::vector<uint8_t> expected_aper = {
        0x50, 0x73, 0x70, 0x65, 0x63, 0x74, 0x72, 0x75, 0x6d, 0x5f, 0x73, 0x6d, 0x00, 0x01, 0x82, 0x53,
        0x70, 0x65, 0x63, 0x74, 0x72, 0x75, 0x6d, 0x20, 0x73, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x20,
        0x6d, 0x6f, 0x64, 0x65, 0x6c, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x49, 0x51, 0x20, 0x69, 0x6e, 0x64,
        0x69, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x50, 0x52, 0x42, 0x20,
        0x62, 0x6c, 0x61, 0x63, 0x6b, 0x6c, 0x69, 0x73, 0x74, 0x20, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x6f,
        0x6c};
    std::vector<uint8_t> aper;
    check(encode_spectrum_ran_function_data(aper), "APER encodes");
    check_bytes(aper, expected_aper, "APER: same bytes as with the previous grammar");

    std::vector<uint8_t> json;
    check(encode_spectrum_ran_function_data_json(json), "JSON encodes");
    check_bytes(json,
                bytes_of("{\"name\":\"spectrum_sm\",\"version\":1,\"description\":"
                         "\"Spectrum service model for IQ indication and PRB blacklist control\"}"),
                "JSON: the grammar's fields, in its order");
}

// ---------------------------------------------------------------------------
// dApp control, APER
// ---------------------------------------------------------------------------

static void test_control_aper_vectors()
{
    std::printf("dApp control, APER vectors derived from X.691\n");

    // {controlPayload prbBlacklistControl {blacklistedPRBs {5, 6}}}
    //   CHOICE with an extension marker: 1 bit, 0 = root alternative. The root has
    //     one alternative, so its index takes 0 bits.
    //   SEQUENCE preamble: 2 bits, both optional fields absent.
    //   Then 5 bits of padding to the octet:                        00
    //   SIZE (0..273) count 2: range 274 > 256, two aligned octets:  00 02
    //   INTEGER (0..272) each: range 273 > 256, two aligned octets:  00 05  00 06
    const std::vector<uint8_t> two_prbs = {0x00, 0x00, 0x02, 0x00, 0x05, 0x00, 0x06};

    // {controlPayload prbBlacklistControl {blacklistedPRBs {}, samplingThreshold 10,
    //  validityPeriod 60}}
    //   bits 0 (root), 1 1 (both optionals present), 5 bits of padding:  60
    //   count 0, two aligned octets:                                     00 00
    //   INTEGER (0..100) = 10: range 101, a 7-bit field, 0001010,
    //     then 1 bit of padding before the next aligned field:           14
    //   INTEGER (1..3600) = 60, as 60 - 1 = 59: two aligned octets:      00 3b
    const std::vector<uint8_t> clear_with_options = {0x60, 0x00, 0x00, 0x14, 0x00, 0x3b};

    SpectrumPRBBlacklistControl control;
    control.blacklisted_prbs = {5, 6};
    std::vector<uint8_t> encoded;
    check(encode_spectrum_dapp_control_aper(control, encoded), "encodes {5, 6}");
    check_bytes(encoded, two_prbs, "{5, 6} matches the hand-derived bytes");

    SpectrumPRBBlacklistControl clear;
    clear.sampling_threshold = 10;
    clear.validity_period    = 60;
    encoded.clear();
    check(encode_spectrum_dapp_control_aper(clear, encoded), "encodes the clear with options");
    check_bytes(encoded, clear_with_options, "clear with options matches the hand-derived bytes");

    SpectrumPRBBlacklistControl decoded;
    std::string                 why;
    check(decode_spectrum_dapp_control_aper(two_prbs, decoded, why) == DecodeResult::ok, "decodes {5, 6}");
    check(decoded.blacklisted_prbs == std::vector<int>{5, 6}, "  PRBs 5, 6");
    check(!decoded.sampling_threshold.has_value() && !decoded.validity_period.has_value(),
          "  no optional fields");

    check(decode_spectrum_dapp_control_aper(clear_with_options, decoded, why) == DecodeResult::ok,
          "decodes the clear with options");
    check(decoded.blacklisted_prbs.empty(), "  empty PRB list");
    check(decoded.sampling_threshold == 10 && decoded.validity_period == 60, "  samplingThreshold 10, validityPeriod 60");
}

static void test_control_aper_rejections()
{
    std::printf("dApp control, APER rejections\n");

    SpectrumPRBBlacklistControl decoded;
    std::string                 why;

    // Not a DAppControlData: an empty buffer.
    check(decode_spectrum_dapp_control_aper({}, decoded, why) == DecodeResult::malformed, "empty input is malformed");

    // A valid envelope holding the other alternative, a sensing policy. Building
    // and encoding it goes through the BOOLEAN runtime (its "deactivate" field).
    Spectrum_DAppControlData_t envelope;
    std::memset(&envelope, 0, sizeof(envelope));
    envelope.controlPayload.present = Spectrum_DAppControlPayload_PR_sensingPolicyControl;
    Spectrum_SensingPolicyControl_t* policy = &envelope.controlPayload.choice.sensingPolicyControl;
    for (int slot = 0; slot < 20; slot++) {
        long* mask = (long*)calloc(1, sizeof(long));
        *mask      = 0x3C00;
        ASN_SEQUENCE_ADD(&policy->maskPerSlot.list, mask);
    }
    policy->deactivate = 1;  // BOOLEAN DEFAULT FALSE: a plain value, not a pointer

    uint8_t        buffer[512];
    asn_enc_rval_t ret = aper_encode_to_buffer(&asn_DEF_Spectrum_DAppControlData, NULL, &envelope, buffer, sizeof(buffer));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_Spectrum_DAppControlData, &envelope);
    check(ret.encoded > 0, "a sensing policy envelope encodes (BOOLEAN runtime linked)");

    const std::vector<uint8_t> sensing_policy(buffer, buffer + (ret.encoded + 7) / 8);
    check(decode_spectrum_dapp_control_aper(sensing_policy, decoded, why) == DecodeResult::unsupported,
          "a sensing policy is unsupported, not malformed");
}

// ---------------------------------------------------------------------------
// dApp control, JSON
// ---------------------------------------------------------------------------

static DecodeResult decode_json(const std::string& text, SpectrumPRBBlacklistControl& out)
{
    std::string why;
    return decode_spectrum_dapp_control_json(bytes_of(text), out, why);
}

static void test_control_json()
{
    std::printf("dApp control, JSON\n");

    SpectrumPRBBlacklistControl decoded;

    // Exactly what the adaptive_cpu dApp sends.
    const std::string from_dapp =
        "{\"controlType\":\"prbBlacklist\",\"controlPayload\":{\"prbBlacklistControl\":{\"blacklistedPRBs\":[5,6]}}}";
    check(decode_json(from_dapp, decoded) == DecodeResult::ok, "the dApp's control decodes");
    check(decoded.blacklisted_prbs == std::vector<int>{5, 6}, "  PRBs 5, 6");

    check(decode_json("{\"controlPayload\":{\"prbBlacklistControl\":{\"blacklistedPRBs\":[272]}}}", decoded) ==
              DecodeResult::ok,
          "controlType may be absent");
    check(decoded.blacklisted_prbs == std::vector<int>{272}, "  PRB 272");

    check(decode_json("{\"controlType\":\"prbBlacklist\",\"controlPayload\":{\"prbBlacklistControl\":"
                      "{\"blacklistedPRBs\":[],\"samplingThreshold\":10,\"validityPeriod\":60}}}",
                      decoded) == DecodeResult::ok,
          "an empty list with both options decodes");
    check(decoded.blacklisted_prbs.empty() && decoded.sampling_threshold == 10 && decoded.validity_period == 60,
          "  empty list, 10, 60");

    check(decode_json("{\"controlType\":\"sensingPolicy\",\"controlPayload\":{\"sensingPolicyControl\":"
                      "{\"maskPerSlot\":[0]}}}",
                      decoded) == DecodeResult::unsupported,
          "a sensing policy is unsupported");
    check(decode_json("{\"controlPayload\":{\"sensingPolicyControl\":{\"maskPerSlot\":[0]}}}", decoded) ==
              DecodeResult::unsupported,
          "a sensing policy without controlType is unsupported");

    check(decode_json("not json", decoded) == DecodeResult::malformed, "not JSON is malformed");
    check(decode_json("{\"controlType\":\"prbBlacklist\"}", decoded) == DecodeResult::malformed,
          "no controlPayload is malformed");
    check(decode_json("{\"controlType\":\"other\",\"controlPayload\":{\"prbBlacklistControl\":"
                      "{\"blacklistedPRBs\":[1]}}}",
                      decoded) == DecodeResult::malformed,
          "an unknown controlType is malformed");
    check(decode_json("{\"controlPayload\":{\"prbBlacklistControl\":{}}}", decoded) == DecodeResult::malformed,
          "no blacklistedPRBs is malformed, not a clear");
    check(decode_json("{\"controlPayload\":{\"prbBlacklistControl\":{\"blacklistedPRBs\":[273]}}}", decoded) ==
              DecodeResult::malformed,
          "PRB 273 is malformed");
    check(decode_json("{\"controlPayload\":{\"prbBlacklistControl\":{\"blacklistedPRBs\":[1.5]}}}", decoded) ==
              DecodeResult::malformed,
          "a non-integer PRB is malformed");
    check(decode_json("{\"controlPayload\":{\"prbBlacklistControl\":{\"blacklistedPRBs\":[1],"
                      "\"samplingThreshold\":101}}}",
                      decoded) == DecodeResult::malformed,
          "samplingThreshold 101 is malformed");
}

int main()
{
    test_iq_indication_bytes_unchanged();
    test_ran_function_data();
    test_control_aper_vectors();
    test_control_aper_rejections();
    test_control_json();

    if (g_failures > 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
