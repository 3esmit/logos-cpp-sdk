// One declaration, two spellings, one binding.
//
// A LIDL record field may be marked optional two ways — the field flag
// (`? name: T`) and the type kind (`name: ?T`) — and logos-lidl's docs/spec.md
// binds them to ONE meaning: "They are identical in meaning and MUST produce
// byte-identical generated code."
//
// The legacy consumer path is the one every real C++ module builds through
// (`--dep <name>=<lidl>`), and it did not honour that. It flattened each
// TypeExpr into a single Qt type NAME before the emitter saw it, which answers
// the optionality question by reading the verbatim spelling: the flag form kept
// T (so `? maybe: tstr` became a bare `QString` that has no empty inhabitant at
// all and silently defaults to "") while the type form collapsed to QVariant.
// One contract, two bindings — and the flag form is the one production
// contracts use.
//
// These tests run the whole path, LIDL text -> JSON surface -> emitted code,
// because the invariant is a property of the composition: neither half can be
// asserted alone. They are written to fail loudly if the two spellings are ever
// reconciled by making BOTH of them wrong, which a pure equality assertion
// would happily accept — see OptionalityActuallySurvives*.

#include <gtest/gtest.h>

#include "generator_lib.h"
#include "lidl_to_json.h"

namespace {

// The same contract twice: flag spelling, then type-kind spelling. Every
// round-trippable shape, plus a record, a list of records, and the untyped
// carriers (`any`, `{tstr: any}`) that must NOT gain a second empty state.
const char* kFlagSpelling = R"LIDL(
module probe {
  version "0.1.0"
  type Inner {
    x: int
  }
  type Rec {
    always: tstr
    ? maybe: tstr
    ? count: uint
    ? blob: bstr
    ? nested: Inner
    ? items: [tstr]
    ? recs: [Inner]
    ? amap: {tstr: any}
    ? anyv: any
  }
  method get() -> Rec
  event changed(r: Rec)
}
)LIDL";

const char* kTypeSpelling = R"LIDL(
module probe {
  version "0.1.0"
  type Inner {
    x: int
  }
  type Rec {
    always: tstr
    maybe: ?tstr
    count: ?uint
    blob: ?bstr
    nested: ?Inner
    items: ?[tstr]
    recs: ?[Inner]
    amap: ?{tstr: any}
    anyv: ?any
  }
  method get() -> Rec
  event changed(r: Rec)
}
)LIDL";

// Same contract with nothing optional — the control that the change is inert
// for every contract that does not use the feature.
const char* kNoOptional = R"LIDL(
module probe {
  version "0.1.0"
  type Inner {
    x: int
  }
  type Rec {
    always: tstr
    maybe: tstr
  }
  method get() -> Rec
  event changed(r: Rec)
}
)LIDL";

ModuleDecl parseOrDie(const char* src)
{
    LidlParseResult pr = lidlParse(QString::fromUtf8(src));
    EXPECT_FALSE(pr.hasError()) << pr.error << " (line " << pr.errorLine << ")";
    return pr.module;
}

struct Emitted { QString header; QString source; };

Emitted emitFor(const char* lidl, ApiStyle style)
{
    const ModuleDecl mod = parseOrDie(lidl);
    const QJsonArray methods = moduleMethodsToJson(mod);
    const QJsonArray events  = moduleEventsToJson(mod);
    const QJsonArray records = moduleRecordsToJson(mod);
    Emitted e;
    e.header = makeHeader("probe", "Probe", methods, style, events, BindMode::Static, records);
    e.source = makeSource("probe", "Probe", "probe_api.h", methods, style, events,
                          BindMode::Static, records);
    return e;
}

} // namespace

// The invariant, stated where it is actually decided: the two spellings reach
// the emitter as the SAME object. Everything downstream follows from this, and
// a failure here localises the bug to the boundary rather than the emitter.
TEST(OptionalSpellings, BoundaryJsonIsIdentical)
{
    const QJsonArray flag = moduleRecordsToJson(parseOrDie(kFlagSpelling));
    const QJsonArray type = moduleRecordsToJson(parseOrDie(kTypeSpelling));
    EXPECT_EQ(flag, type);
}

TEST(OptionalSpellings, QtOutputIsByteIdentical)
{
    const Emitted flag = emitFor(kFlagSpelling, ApiStyle::Qt);
    const Emitted type = emitFor(kTypeSpelling, ApiStyle::Qt);
    EXPECT_EQ(flag.header, type.header);
    EXPECT_EQ(flag.source, type.source);
}

TEST(OptionalSpellings, LpOutputIsByteIdentical)
{
    const Emitted flag = emitFor(kFlagSpelling, ApiStyle::Lp);
    const Emitted type = emitFor(kTypeSpelling, ApiStyle::Lp);
    EXPECT_EQ(flag.header, type.header);
    EXPECT_EQ(flag.source, type.source);
}

// Equality alone is satisfied by two identically WRONG outputs — e.g. by
// dropping optionality from both spellings. These pin what the agreed answer
// has to be.
//
// Qt: an invalid QVariant is the surface's single empty inhabitant, so `?T` is
// QVariant — two-state, untyped. Same answer the client-stub backend gives for
// the same surface (lidl_gen_client.cpp), deliberately.
TEST(OptionalSpellings, OptionalityActuallySurvivesOnQt)
{
    for (const char* lidl : {kFlagSpelling, kTypeSpelling}) {
        const Emitted e = emitFor(lidl, ApiStyle::Qt);
        // The declared type is not the value type — a bare QString could not be
        // empty at all.
        EXPECT_TRUE(e.header.contains("QVariant maybe{};")) << e.header.toStdString();
        EXPECT_TRUE(e.header.contains("QVariant count{};")) << e.header.toStdString();
        EXPECT_FALSE(e.header.contains("QString maybe{};")) << e.header.toStdString();
        // A record field is a NAMED slot: empty omits the key.
        EXPECT_TRUE(e.source.contains("if (v.maybe.isValid()) __m.insert")) << e.source.toStdString();
        // Absent and null are the same state on decode: no conversion, which
        // would have turned empty into "".
        EXPECT_TRUE(e.source.contains(
            "__out.maybe = __m.value(QStringLiteral(\"maybe\"));")) << e.source.toStdString();
        // Required fields keep their typed conversion.
        EXPECT_TRUE(e.source.contains(
            "__out.always = __m.value(QStringLiteral(\"always\")).toString();")) << e.source.toStdString();
    }
}

// Lp: the std surface HAS an optional, so it keeps the value type. Same answer
// the cdylib backend gives, and the encoder that pairs with it is
// logos-protocol's Codec<std::optional<T>>.
TEST(OptionalSpellings, OptionalityActuallySurvivesOnLp)
{
    for (const char* lidl : {kFlagSpelling, kTypeSpelling}) {
        const Emitted e = emitFor(lidl, ApiStyle::Lp);
        EXPECT_TRUE(e.header.contains("std::optional<std::string> maybe{};")) << e.header.toStdString();
        EXPECT_TRUE(e.header.contains("std::optional<uint64_t> count{};")) << e.header.toStdString();
        EXPECT_TRUE(e.header.contains("std::optional<std::vector<uint8_t>> blob{};")) << e.header.toStdString();
        EXPECT_TRUE(e.header.contains("std::optional<Inner> nested{};")) << e.header.toStdString();
        EXPECT_TRUE(e.header.contains("std::optional<std::vector<std::string>> items{};")) << e.header.toStdString();
        EXPECT_TRUE(e.header.contains("std::optional<std::vector<Inner>> recs{};")) << e.header.toStdString();
        EXPECT_TRUE(e.header.contains("#include <optional>\n")) << e.header.toStdString();
        // Named slot: empty omits the key; the value is encoded exactly as the
        // non-optional field would be (bytes still canonically tagged).
        EXPECT_TRUE(e.source.contains(
            "if (v.blob.has_value()) __j[\"blob\"] = logos::bytesToJson((*v.blob));")) << e.source.toStdString();
        // Absent AND explicit null both leave it nullopt. A `contains`-only
        // guard would decode null through the value conversion and turn empty
        // into 0 / "".
        EXPECT_TRUE(e.source.contains(
            "if (w.contains(\"maybe\") && !w.at(\"maybe\").is_null())")) << e.source.toStdString();
    }
}

// `?any`, `?{K:V}` and `?[any]` are carried as nlohmann::json, which ALREADY
// has null among its inhabitants. Wrapping those in std::optional would give
// them two distinct empty spellings — three states, which the two-state rule
// forbids. They collapse onto the bare alias instead. (The cdylib backend makes
// exactly this exception; the Qt surface needs none, because `any` is QVariant
// there either way.)
TEST(OptionalSpellings, UntypedJsonAliasesDoNotGainASecondEmptyState)
{
    for (const char* lidl : {kFlagSpelling, kTypeSpelling}) {
        const Emitted e = emitFor(lidl, ApiStyle::Lp);
        EXPECT_TRUE(e.header.contains("LogosMap amap{};")) << e.header.toStdString();
        EXPECT_TRUE(e.header.contains("LogosMap anyv{};")) << e.header.toStdString();
        EXPECT_FALSE(e.header.contains("std::optional<LogosMap>")) << e.header.toStdString();
        EXPECT_FALSE(e.header.contains("std::optional<LogosList>")) << e.header.toStdString();
    }
}

// A contract that declares no optional must be untouched by all of the above —
// including the conditional `#include <optional>`, whose whole point is that it
// is conditional.
TEST(OptionalSpellings, ContractsWithoutOptionalsAreUnaffected)
{
    const Emitted qt = emitFor(kNoOptional, ApiStyle::Qt);
    EXPECT_TRUE(qt.header.contains("QString maybe{};")) << qt.header.toStdString();
    EXPECT_TRUE(qt.source.contains(
        "__out.maybe = __m.value(QStringLiteral(\"maybe\")).toString();")) << qt.source.toStdString();
    EXPECT_FALSE(qt.source.contains("isValid()) __m.insert")) << qt.source.toStdString();

    const Emitted lp = emitFor(kNoOptional, ApiStyle::Lp);
    EXPECT_TRUE(lp.header.contains("std::string maybe{};")) << lp.header.toStdString();
    EXPECT_FALSE(lp.header.contains("std::optional")) << lp.header.toStdString();
    EXPECT_FALSE(lp.header.contains("#include <optional>")) << lp.header.toStdString();
    EXPECT_FALSE(lp.source.contains("has_value()")) << lp.source.toStdString();
}

// A POSITIONAL slot (method parameter, return type, event parameter) has no
// name to hang a flag on, so it has only the type-kind spelling and there is no
// divergence to fix — but it is also still flattened to an untyped carrier,
// which is the part of the gap this change does NOT close. Pinned so the
// remaining gap is a documented fact rather than an assumption, and so that
// closing it later is a deliberate, visible change.
TEST(OptionalSpellings, PositionalSlotsAreStillFlattened)
{
    const char* lidl = R"LIDL(
module probe {
  version "0.1.0"
  method echo(v: ?tstr) -> ?tstr
}
)LIDL";
    const Emitted qt = emitFor(lidl, ApiStyle::Qt);
    EXPECT_TRUE(qt.header.contains("QVariant echo(QVariant v")) << qt.header.toStdString();
    const Emitted lp = emitFor(lidl, ApiStyle::Lp);
    EXPECT_TRUE(lp.header.contains("LogosMap echo(const LogosMap& v")) << lp.header.toStdString();
    EXPECT_FALSE(lp.header.contains("std::optional")) << lp.header.toStdString();
}
