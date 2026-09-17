// The module-side reader for the caller-of-a-dispatch document.
//
// logos-protocol/cpp/logos_module_impl.h holds the normative definition of the
// document; this suite is the C++ reader's conformance to it, rule by rule. The
// Rust reader owes the identical table.
//
// WHY EVERY MALFORMED CASE IS SPELLED OUT rather than covered by one "bad input
// is Unknown" test: the value feeds predicates that sit next to authorization
// decisions, and the failure that matters is not a crash — it is isModule("x")
// answering TRUE for something that is not x. Each case below is a distinct way
// to get there.
//
// HOW EACH TEST WAS SHOWN TO BE A DETECTOR. Throwaway local mutations of
// cpp/logos_caller.h, made and reverted (the same discipline as
// logos-protocol/tests/protocol/test_inbound_token_store.cpp); the report on
// the branch records which test caught which mutation.

#include <gtest/gtest.h>

#include <thread>

#include "logos_caller.h"

using logos::CallerKind;
using logos::LogosCaller;
using logos::parseCaller;

namespace {

// Restores the ambient stack between tests. currentCaller() reads a
// thread_local that outlives any single TEST body, so a test that pushed and
// did not pop would leak its caller into whatever ran next on this thread —
// and the leak would look like a PASS in the test that received it.
class CallerScope : public ::testing::Test {
protected:
    void TearDown() override
    {
        while (!logos::currentCaller().isUnknown())
            logos::detail::setCallCaller(nullptr);
    }
};

} // namespace

// ── Rule 2: an arm this build has never heard of ────────────────────────────
//
// THE test for forward compatibility, and the reason parseCaller cannot be a
// switch over a closed set that asserts. A 0.7 host talking to a module built
// at 0.6 is the ordinary case the moment a new arm is specified, and the module
// must degrade rather than die.
TEST_F(CallerScope, AnUnrecognisedArmFromANewerProtocolIsUnknown)
{
    // The document CARRIES a name, and that is the whole point of the case.
    // An unrecognised arm with no name is caught by any implementation; the
    // failure this guards against is the tempting one — "I do not know this
    // kind, but there is a name here, so treat it as a module" — which turns
    // isModule("chat_module") TRUE for something that is not chat_module.
    // Written first without the name field, this test passed against exactly
    // that fallback.
    const LogosCaller c =
        parseCaller(R"({"kind":"fleet","name":"chat_module","fleet":"eu-west-1"})");

    EXPECT_EQ(c.kind, CallerKind::Unknown);
    EXPECT_TRUE(c.isUnknown());
    EXPECT_FALSE(c.isModule());
    EXPECT_FALSE(c.isModule("chat_module")) << "an unknown arm was salvaged into a module";
    EXPECT_TRUE(c.name.empty());
}

// The same salvage, on the arms that have a REQUIRED name of their own. A
// reader that fell through to "operator" or "derived" on an unknown kind would
// be caught here and nowhere else.
TEST_F(CallerScope, AnUnrecognisedArmIsNotSalvagedIntoAnyKnownArm)
{
    struct Case { const char* label; const char* json; };
    const Case cases[] = {
        {"name-bearing",     R"({"kind":"fleet","name":"ops-readonly"})"},
        {"parent+leaf",      R"({"kind":"scoped","parent":"wallet_module","leaf":"wallet_ui"})"},
        {"every known field",R"({"kind":"v2","name":"chat_module","instance":"a41f",)"
                             R"("parent":"wallet_module","leaf":"wallet_ui"})"},
    };

    for (const Case& c : cases) {
        const LogosCaller parsed = parseCaller(c.json);
        EXPECT_EQ(parsed.kind, CallerKind::Unknown) << c.label;
        EXPECT_FALSE(parsed.isModule()) << c.label;
        EXPECT_FALSE(parsed.isHost()) << c.label;
        EXPECT_FALSE(parsed.isDerived()) << c.label;
        EXPECT_FALSE(parsed.isOperator()) << c.label;
        EXPECT_TRUE(parsed.name.empty()) << c.label;
        EXPECT_TRUE(parsed.parent.empty()) << c.label;
        EXPECT_TRUE(parsed.leaf.empty()) << c.label;
        EXPECT_TRUE(parsed.instance.empty()) << c.label;
    }
}

TEST_F(CallerScope, AnUnrecognisedArmDoesNotThrow)
{
    // Stated separately from the value assertion above because it is a
    // different failure: a handler must not be able to be killed by the
    // document it was handed, and `parseCaller` is called on the dispatch
    // thread inside the generated export, where there is nothing to catch it.
    EXPECT_NO_THROW({
        (void)parseCaller(R"({"kind":"fleet"})");
        (void)parseCaller(R"({"kind":"module_v2","name":"chat_module"})");
    });
}

// ── Rule 1: "kind" is mandatory; malformed input is Unknown ─────────────────

TEST_F(CallerScope, MalformedInputDegradesToUnknownWithoutThrowing)
{
    struct Case { const char* label; const char* json; };
    const Case cases[] = {
        {"empty string",        ""},
        {"whitespace",          "   "},
        {"truncated object",    R"({"kind":"module")"},
        {"not json at all",     "chat_module"},
        {"json null",           "null"},
        {"array, not object",   R"(["kind","host"])"},
        {"string, not object",  R"("host")"},
        {"number, not object",  "42"},
        {"object without kind", R"({"name":"chat_module"})"},
        {"kind is not a string",R"({"kind":7,"name":"chat_module"})"},
        {"kind is null",        R"({"kind":null})"},
        {"nul bytes",           "\x01\x02\x03"},
    };

    for (const Case& c : cases) {
        LogosCaller parsed;
        EXPECT_NO_THROW({ parsed = parseCaller(c.json); }) << c.label;
        EXPECT_EQ(parsed.kind, CallerKind::Unknown) << c.label;
        EXPECT_FALSE(parsed.isModule()) << c.label;
        EXPECT_TRUE(parsed.name.empty()) << c.label;
    }
}

TEST_F(CallerScope, TheExplicitUnknownArmIsUnknown)
{
    // A producer today, per the protocol note. It must not be mistaken for a
    // parse failure and must not be mistaken for an unrecognised arm — all
    // three land on the same value, which is what makes the value safe.
    EXPECT_EQ(parseCaller(R"({"kind":"unknown"})").kind, CallerKind::Unknown);
}

// ── The recognised arms ─────────────────────────────────────────────────────

TEST_F(CallerScope, TheHostArmParses)
{
    const LogosCaller c = parseCaller(R"({"kind":"host"})");

    EXPECT_EQ(c.kind, CallerKind::Host);
    EXPECT_TRUE(c.isHost());
    EXPECT_FALSE(c.isModule());
}

// Rule 5. "core" and "capability_module" hold the same token VALUE under two
// keys by construction, so a name on this arm would be a coin flip presented as
// a fact. If a host ever emits one, it is ignored — rule 3 — rather than
// promoted into a field that call sites would then branch on.
TEST_F(CallerScope, TheHostArmNeverCarriesAName)
{
    const LogosCaller c = parseCaller(R"({"kind":"host","name":"capability_module"})");

    EXPECT_EQ(c.kind, CallerKind::Host);
    EXPECT_TRUE(c.name.empty()) << "host must not gain a name: " << c.name;
}

TEST_F(CallerScope, TheModuleArmParsesAndMatchesByName)
{
    const LogosCaller c = parseCaller(R"({"kind":"module","name":"chat_module"})");

    EXPECT_EQ(c.kind, CallerKind::Module);
    EXPECT_TRUE(c.isModule());
    EXPECT_EQ(c.name, "chat_module");
    EXPECT_TRUE(c.isModule("chat_module"));
    EXPECT_FALSE(c.isModule("wallet_module"));
    EXPECT_TRUE(c.instance.empty());
}

// Rule 6, and the reason `instance` is in the type from day one rather than
// added when instance addressing arrives: isModule(name) must keep its answer
// on the day a producer starts emitting the field. If the predicate compared
// the whole identity, every existing call site would silently start returning
// false the first time a host addressed an instance.
TEST_F(CallerScope, IsModuleIgnoresTheInstance)
{
    const LogosCaller c =
        parseCaller(R"({"kind":"module","name":"chat_module","instance":"a41f"})");

    EXPECT_EQ(c.kind, CallerKind::Module);
    EXPECT_EQ(c.instance, "a41f");
    EXPECT_TRUE(c.isModule("chat_module")) << "instance addressing changed the answer";
}

TEST_F(CallerScope, TheDerivedArmParses)
{
    const LogosCaller c =
        parseCaller(R"({"kind":"derived","parent":"wallet_module","leaf":"wallet_ui"})");

    EXPECT_EQ(c.kind, CallerKind::Derived);
    EXPECT_TRUE(c.isDerived());
    EXPECT_EQ(c.parent, "wallet_module");
    EXPECT_EQ(c.leaf, "wallet_ui");
    // A derived identity is NOT its parent module. Answering true here would
    // hand a plugin the parent's authority.
    EXPECT_FALSE(c.isModule("wallet_module"));
}

TEST_F(CallerScope, TheOperatorArmParses)
{
    const LogosCaller c = parseCaller(R"({"kind":"operator","name":"ops-readonly"})");

    EXPECT_EQ(c.kind, CallerKind::Operator);
    EXPECT_TRUE(c.isOperator());
    EXPECT_EQ(c.name, "ops-readonly");
    // Shares the `name` field with the module arm and must not be confused for
    // one: an operator named "chat_module" is not chat_module.
    EXPECT_FALSE(c.isModule("ops-readonly"));
}

// The two arms nothing emits yet must nonetheless PARSE. They are specified, so
// a 0.6 module can receive one from a later host; a reader that treated them as
// unrecognised would be within rule 2 but would lose real information for no
// reason, and the gap would only surface once a producer shipped.
TEST_F(CallerScope, TheUnproducedArmsAreStillParsedNotTreatedAsUnrecognised)
{
    EXPECT_EQ(parseCaller(R"({"kind":"derived","parent":"p","leaf":"l"})").kind,
              CallerKind::Derived);
    EXPECT_EQ(parseCaller(R"({"kind":"operator","name":"o"})").kind,
              CallerKind::Operator);
}

// ── Rule 4: a known arm missing a required field is Unknown, not partial ────
//
// The dangerous shape. A reader that kept the arm and left the field empty
// would make isModule("") true for a nameless module document.
TEST_F(CallerScope, AKnownArmMissingARequiredFieldIsUnknownNotPartial)
{
    struct Case { const char* label; const char* json; };
    const Case cases[] = {
        {"module without name",   R"({"kind":"module"})"},
        {"module, name not a string", R"({"kind":"module","name":42})"},
        {"module, empty name",    R"({"kind":"module","name":""})"},
        {"derived without leaf",  R"({"kind":"derived","parent":"wallet_module"})"},
        {"derived without parent",R"({"kind":"derived","leaf":"wallet_ui"})"},
        {"derived, empty leaf",   R"({"kind":"derived","parent":"p","leaf":""})"},
        {"operator without name", R"({"kind":"operator"})"},
        {"operator, empty name",  R"({"kind":"operator","name":""})"},
    };

    for (const Case& c : cases) {
        const LogosCaller parsed = parseCaller(c.json);
        EXPECT_EQ(parsed.kind, CallerKind::Unknown) << c.label;
        EXPECT_FALSE(parsed.isModule("")) << c.label << ": matched the empty name";
        EXPECT_TRUE(parsed.name.empty()) << c.label;
        EXPECT_TRUE(parsed.parent.empty()) << c.label;
        EXPECT_TRUE(parsed.leaf.empty()) << c.label;
    }
}

// ── Rule 3: unrecognised fields inside a known arm are ignored ──────────────

TEST_F(CallerScope, AKnownArmToleratesFieldsItDoesNotKnow)
{
    // This is what lets an arm gain a field without a MINOR bump. A reader that
    // rejected the document would make every such addition a breaking change.
    const LogosCaller c = parseCaller(
        R"({"kind":"module","name":"chat_module","instance":"a41f","tier":"gold","hops":3})");

    EXPECT_EQ(c.kind, CallerKind::Module);
    EXPECT_EQ(c.name, "chat_module");
    EXPECT_EQ(c.instance, "a41f");
}

TEST_F(CallerScope, AnOptionalFieldOfTheWrongTypeIsDroppedNotFatal)
{
    // `instance` is optional and isModule() ignores it, so a malformed one must
    // not cost us a perfectly good name. Contrast the required-field cases
    // above, which must degrade the whole identity.
    const LogosCaller c =
        parseCaller(R"({"kind":"module","name":"chat_module","instance":[1,2]})");

    EXPECT_EQ(c.kind, CallerKind::Module);
    EXPECT_EQ(c.name, "chat_module");
    EXPECT_TRUE(c.instance.empty());
    EXPECT_TRUE(c.isModule("chat_module"));
}

// ── The ambient accessor: push, pop, nesting, threads ───────────────────────

TEST_F(CallerScope, OutsideADispatchTheCallerIsUnknown)
{
    // A worker thread, a timer, a context hook and an event emission all land
    // here. Unknown is the correct answer, not a bug to be papered over.
    EXPECT_TRUE(logos::currentCaller().isUnknown());
}

TEST_F(CallerScope, APushIsVisibleToTheHandlerAndThePopClearsIt)
{
    logos::detail::setCallCaller(R"({"kind":"module","name":"chat_module"})");
    EXPECT_TRUE(logos::currentCaller().isModule("chat_module"));

    logos::detail::setCallCaller(nullptr);
    EXPECT_TRUE(logos::currentCaller().isUnknown());
}

// THE nesting test, and the reason this is a stack rather than a slot. A
// handler that calls out spins a nested event loop; a second inbound call
// arriving on that same thread inside it pushes, and its pop must restore the
// OUTER caller — not clear the slot, which would leave the outer handler
// reading Unknown for the rest of its frame.
TEST_F(CallerScope, ANestedDispatchRestoresTheOuterCallerRatherThanClearingIt)
{
    logos::detail::setCallCaller(R"({"kind":"module","name":"outer_module"})");
    ASSERT_TRUE(logos::currentCaller().isModule("outer_module"));

    logos::detail::setCallCaller(R"({"kind":"module","name":"inner_module"})");
    EXPECT_TRUE(logos::currentCaller().isModule("inner_module"));

    logos::detail::setCallCaller(nullptr);
    EXPECT_TRUE(logos::currentCaller().isModule("outer_module"))
        << "the inner pop erased the outer caller";

    logos::detail::setCallCaller(nullptr);
    EXPECT_TRUE(logos::currentCaller().isUnknown());
}

TEST_F(CallerScope, AnUnbalancedPopIsANoOpRatherThanUndefinedBehaviour)
{
    // The host and the module do not share a lifetime. A clear that arrives
    // without its push — a retried teardown, a module loaded mid-dispatch —
    // must not pop an empty vector.
    EXPECT_NO_THROW({
        logos::detail::setCallCaller(nullptr);
        logos::detail::setCallCaller(nullptr);
    });
    EXPECT_TRUE(logos::currentCaller().isUnknown());
}

// Each thread has its OWN stack. With a shared one, a module whose concurrency
// is "multi" would have handlers on different threads overwriting each other's
// caller — the worst possible failure for this value, because the wrong answer
// would be another real module's name rather than Unknown.
TEST_F(CallerScope, EachThreadHasItsOwnCallerStack)
{
    logos::detail::setCallCaller(R"({"kind":"module","name":"main_thread_caller"})");
    ASSERT_TRUE(logos::currentCaller().isModule("main_thread_caller"));

    bool otherThreadStartedUnknown = false;
    bool otherThreadSawItsOwn = false;

    std::thread worker([&] {
        otherThreadStartedUnknown = logos::currentCaller().isUnknown();
        logos::detail::setCallCaller(R"({"kind":"module","name":"worker_caller"})");
        otherThreadSawItsOwn = logos::currentCaller().isModule("worker_caller");
        logos::detail::setCallCaller(nullptr);
    });
    worker.join();

    EXPECT_TRUE(otherThreadStartedUnknown) << "the worker inherited another thread's caller";
    EXPECT_TRUE(otherThreadSawItsOwn);
    EXPECT_TRUE(logos::currentCaller().isModule("main_thread_caller"))
        << "the worker's push was visible on the main thread";
}

// currentCaller() returns a reference into the stack. A handler that copies it
// must keep a valid value after the dispatch pops — the documented way to use
// the identity beyond one's own frame.
TEST_F(CallerScope, ACopyOutlivesTheDispatchThatProducedIt)
{
    logos::detail::setCallCaller(R"({"kind":"module","name":"chat_module"})");
    const LogosCaller copied = logos::currentCaller();
    logos::detail::setCallCaller(nullptr);

    EXPECT_TRUE(copied.isModule("chat_module"));
    EXPECT_TRUE(logos::currentCaller().isUnknown());
}
