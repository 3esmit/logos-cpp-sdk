#include <gtest/gtest.h>

#include "logos_host_services.h"

#include <string>

// Coverage for the Qt-free host-services veneer.
//
// The GATE itself (ungranted callers get LP_ERR_UNSUPPORTED / a null
// lp_token_keys) is tested in logos-protocol, which is where the gate lives and
// where the library is linked — see tests/protocol/test_host_services_grant.cpp.
// What is tested here is what this header ADDS: constantTimeEquals, plus the
// fact that the header parses standalone in a Qt-free, protocol-unlinked TU.
//
// Note this file deliberately does NOT call tokenKeys()/informModuleTokenTo():
// they are `inline` and never ODR-used here, so no lp_* symbol is referenced
// and sdk_tests keeps linking against logos_headers alone. That is also the
// property being asserted by this file existing at all — the veneer must not
// drag the protocol library into a header-only consumer.

using logos::host::constantTimeEquals;

TEST(HostServicesConstantTimeEquals, EqualStringsMatch)
{
    EXPECT_TRUE(constantTimeEquals("", ""));
    EXPECT_TRUE(constantTimeEquals("a", "a"));
    EXPECT_TRUE(constantTimeEquals("10d794ec-1234-5678-9abc-def012345678",
                                   "10d794ec-1234-5678-9abc-def012345678"));
}

TEST(HostServicesConstantTimeEquals, DifferentLengthsDoNotMatch)
{
    EXPECT_FALSE(constantTimeEquals("", "a"));
    EXPECT_FALSE(constantTimeEquals("a", ""));
    EXPECT_FALSE(constantTimeEquals("token", "token "));
    EXPECT_FALSE(constantTimeEquals("token", "toke"));
}

TEST(HostServicesConstantTimeEquals, DifferenceInAnyPositionIsCaught)
{
    const std::string ref = "abcdefghijklmnop";
    // A comparison that early-exits would still get these right; what would
    // NOT be caught by a weaker test is a loop that stops at the first
    // mismatch and reports equality for the rest. Walk every index so a
    // truncated loop bound fails here rather than in production.
    for (std::size_t i = 0; i < ref.size(); ++i) {
        std::string other = ref;
        other[i] = static_cast<char>(other[i] ^ 0x01);
        EXPECT_FALSE(constantTimeEquals(ref, other))
            << "difference at index " << i << " was not detected";
    }
}

TEST(HostServicesConstantTimeEquals, EmbeddedNulsAreCompared)
{
    // std::string is not NUL-terminated-by-convention here; a memcmp/strcmp
    // regression would stop at the NUL and call these equal.
    const std::string a("tok\0AAA", 7);
    const std::string b("tok\0BBB", 7);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_FALSE(constantTimeEquals(a, b));
    EXPECT_TRUE(constantTimeEquals(a, std::string("tok\0AAA", 7)));
}

TEST(HostServicesConstantTimeEquals, HighBitBytesAreCompared)
{
    // Signed char: 0x80 sign-extends. Without the unsigned casts in the
    // implementation the XOR still works, but a naive `int` accumulator that
    // dropped the cast could mask a difference — pin the behaviour.
    const std::string a("\x80\x01", 2);
    const std::string b("\x80\x81", 2);
    EXPECT_FALSE(constantTimeEquals(a, b));
    EXPECT_TRUE(constantTimeEquals(a, std::string("\x80\x01", 2)));
}

TEST(HostServicesStatus, UngrantedIsDistinguishableFromOtherFailures)
{
    logos::host::Status ungranted{false, LP_ERR_UNSUPPORTED};
    EXPECT_TRUE(ungranted.ungranted());
    EXPECT_FALSE(static_cast<bool>(ungranted));

    logos::host::Status otherFailure{false, LP_ERR_INVALID_ARG};
    EXPECT_FALSE(otherFailure.ungranted())
        << "a non-gate failure must not be reported as 'not permitted'";

    logos::host::Status ok = logos::host::Status::fromCode(LP_OK);
    EXPECT_TRUE(static_cast<bool>(ok));
    EXPECT_FALSE(ok.ungranted());
}
