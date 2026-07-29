// Value-level tests for the canonical tagged-bytes codec.
//
// Binary payloads cross every module boundary as {"_bytes": "<base64url>"}.
// The generated cdylib event sidecar encodes into that form and the generated
// `lp` consumer wrappers decode out of it, so a wrong alphabet, a stray '=',
// or a botched tail group silently corrupts every binary event in the system.
// The code-generation tests assert on generated *source text* and cannot catch
// that; these assert on actual bytes.

#include <gtest/gtest.h>

// logos_codec.h owns the canonical encode; logos_lp_client.h owns the lenient
// `lp`-path decode. They used to be second copies in logos_json.h, which is now
// aliases-only — see the note at the top of that header.
#include <logos_codec.h>
#include <logos_lp_client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> bytesOf(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

// RFC 4648 §10 test vectors, in the URL-safe alphabet without padding — the
// form logos-protocol emits (Base64UrlEncoding | OmitTrailingEquals).
TEST(LogosJsonBytes, EncodesRfc4648VectorsUnpadded)
{
    EXPECT_EQ(logos::b64UrlEncode(bytesOf("")),       "");
    EXPECT_EQ(logos::b64UrlEncode(bytesOf("f")),      "Zg");
    EXPECT_EQ(logos::b64UrlEncode(bytesOf("fo")),     "Zm8");
    EXPECT_EQ(logos::b64UrlEncode(bytesOf("foo")),    "Zm9v");
    EXPECT_EQ(logos::b64UrlEncode(bytesOf("foob")),   "Zm9vYg");
    EXPECT_EQ(logos::b64UrlEncode(bytesOf("fooba")),  "Zm9vYmE");
    EXPECT_EQ(logos::b64UrlEncode(bytesOf("foobar")), "Zm9vYmFy");
}

// The URL-safe alphabet uses '-' and '_' where standard base64 uses '+' and '/'.
// Getting this wrong still round-trips through our own decoder but corrupts
// every payload crossing to the Qt side, which decodes with Base64UrlEncoding.
TEST(LogosJsonBytes, UsesTheUrlSafeAlphabet)
{
    const std::vector<uint8_t> bytes = {0xfb, 0xef, 0xbe};  // four sextets of 62
    const std::string enc = logos::b64UrlEncode(bytes);     // "++++" in standard b64
    EXPECT_EQ(enc, "----");
    EXPECT_EQ(enc.find('+'), std::string::npos);
    EXPECT_EQ(enc.find('/'), std::string::npos);
    EXPECT_EQ(enc.find('='), std::string::npos);
}

TEST(LogosJsonBytes, RoundTripsEveryTailLength)
{
    // Every length 0..64 covers all three len%3 tail groups repeatedly.
    for (size_t n = 0; n <= 64; ++n) {
        std::vector<uint8_t> in(n);
        for (size_t i = 0; i < n; ++i)
            in[i] = static_cast<uint8_t>((i * 7 + 11) & 0xff);

        const nlohmann::json tagged = logos::bytesToJson(in);
        ASSERT_TRUE(tagged.is_object());
        ASSERT_TRUE(tagged.contains("_bytes"));
        EXPECT_EQ(logos::jsonToBytes(tagged), in) << "length " << n;
    }
}

// The reason the tagged form exists at all: a plain JSON string would truncate
// at the first NUL and mangle anything >= 0x80.
TEST(LogosJsonBytes, SurvivesEmbeddedNulsAndHighBytes)
{
    const std::vector<uint8_t> in = {0x00, 0xff, 0x00, 0x80, 0x7f, 0x00, 0xfe, 0xc3, 0x28};
    EXPECT_EQ(logos::jsonToBytes(logos::bytesToJson(in)), in);
}

// A large payload — the shape of the proof blobs that surfaced this bug
// (logos-cpp-sdk#99 reported a 109,447-byte payload arriving empty).
TEST(LogosJsonBytes, RoundTripsALargePayload)
{
    std::vector<uint8_t> in(109447);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);

    const std::vector<uint8_t> out = logos::jsonToBytes(logos::bytesToJson(in));
    ASSERT_EQ(out.size(), in.size());
    EXPECT_EQ(out, in);
}

// The empty payload must round-trip as empty — and, critically, must be
// distinguishable from the bug it masked: an event that dropped its bytes used
// to arrive as exactly this value.
TEST(LogosJsonBytes, EmptyPayloadRoundTrips)
{
    const nlohmann::json tagged = logos::bytesToJson({});
    EXPECT_EQ(tagged, nlohmann::json({{"_bytes", ""}}));
    EXPECT_TRUE(logos::jsonToBytes(tagged).empty());
}

// Decoding is lenient in the same way the rest of the `lp` decode path is:
// a malformed value yields empty rather than throwing across the C ABI.
TEST(LogosJsonBytes, DecodeIsLenientOnMalformedInput)
{
    EXPECT_TRUE(logos::jsonToBytes(nlohmann::json()).empty());
    EXPECT_TRUE(logos::jsonToBytes(nlohmann::json("plain string")).empty());
    EXPECT_TRUE(logos::jsonToBytes(nlohmann::json::array({1, 2, 3})).empty());
    EXPECT_TRUE(logos::jsonToBytes(nlohmann::json{{"other", "key"}}).empty());
    EXPECT_TRUE(logos::jsonToBytes(nlohmann::json{{"_bytes", 42}}).empty());
}

// Padded input is not what we emit, but a peer that pads must still decode:
// '=' is skipped rather than treated as data.
TEST(LogosJsonBytes, DecodeAcceptsPaddedInput)
{
    EXPECT_EQ(logos::jsonToBytes(nlohmann::json{{"_bytes", "Zm9vYg=="}}), bytesOf("foob"));
}
