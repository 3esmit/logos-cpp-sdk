// Code-generation tests for the cdylib backend's events sidecar.
//
// The sidecar is a Qt-FREE translation unit, so two classes of defect live
// here: dropping a payload (logos-cpp-sdk#99 — every `bstr` event argument was
// serialized as an empty tagged value), and emitting a Qt type into a TU that
// cannot compile one.
//
// These assert on generated source text. The bytes the emitted encoder actually
// produces are covered by value in tests/sdk/test_logos_json_bytes.cpp.

#include <gtest/gtest.h>

#include "lidl_gen_cdylib.h"

namespace {

TypeExpr prim(const char* name)
{
    return {TypeExpr::Primitive, name, {}};
}

ParamDecl param(const char* name, const TypeExpr& type)
{
    ParamDecl p;
    p.name = name;
    p.type = type;
    return p;
}

ModuleDecl moduleWithEvent(const char* eventName, const std::vector<ParamDecl>& params)
{
    ModuleDecl m;
    m.name = "delivery_module";

    EventDecl e;
    e.name = eventName;
    e.params = params;
    m.events.push_back(e);
    return m;
}

QString eventsSourceFor(const ModuleDecl& m)
{
    return lidlMakeEventsSourceCdylib(m, "DeliveryModuleImpl", "delivery_module_plugin.h");
}

} // namespace

// logos-cpp-sdk#99: `payload` was replaced by an empty tagged value, so a module
// could emit real bytes and every consumer still received zero of them.
TEST(LidlGenCdylib, BinaryEventPayloadUsesCanonicalBytesEncoding)
{
    const ModuleDecl m = moduleWithEvent("messageReceived", {
        param("messageHash",  prim("tstr")),
        param("contentTopic", prim("tstr")),
        param("payload",      prim("bstr")),
        param("timestamp",    prim("int")),
    });

    const QString source = eventsSourceFor(m);

    // The real argument is serialized, through the canonical encoder...
    EXPECT_TRUE(source.contains("args.push_back(lidlBytesToJson(payload));"));
    EXPECT_TRUE(source.contains("std::string lidlB64UrlEncode"));
    EXPECT_TRUE(source.contains("nlohmann::json lidlBytesToJson"));

    // ...and the empty tagged value is gone.
    EXPECT_FALSE(source.contains("nlohmann::json{{\"_bytes\", \"\"}}"));

    // The other parameters are still passed straight through.
    EXPECT_TRUE(source.contains("args.push_back(messageHash);"));
    EXPECT_TRUE(source.contains("args.push_back(timestamp);"));

    // Bytes are taken by const-ref, matching the author's logos_events: block.
    EXPECT_TRUE(source.contains("const std::vector<uint8_t>& payload"));
}

// The encoder is only needed by modules that actually emit binary payloads.
// Emitted unconditionally it is an unused static function in every other
// module's sidecar (-Wunused-function).
TEST(LidlGenCdylib, BytesEncoderOmittedWhenNoEventCarriesBytes)
{
    const ModuleDecl m = moduleWithEvent("fault", {
        param("code",    prim("int")),
        param("message", prim("tstr")),
        param("fatal",   prim("bool")),
    });

    const QString source = eventsSourceFor(m);

    EXPECT_FALSE(source.contains("lidlB64UrlEncode"));
    EXPECT_FALSE(source.contains("lidlBytesToJson"));
    EXPECT_TRUE(source.contains("args.push_back(code);"));
}

// The sidecar is compiled into the module's Qt-free cdylib, so a JSON payload
// has to be spelled as its nlohmann alias. Emitted as QVariantMap it does not
// compile at all.
TEST(LidlGenCdylib, JsonEventPayloadIsQtFree)
{
    ModuleDecl m;
    m.name = "state_module";

    EventDecl e;
    e.name = "stateChanged";
    e.params.push_back(param("key", prim("tstr")));
    e.params.push_back(param("state",
        TypeExpr{TypeExpr::Map, "", {prim("tstr"), prim("any")}}));
    m.events.push_back(e);

    const QString source =
        lidlMakeEventsSourceCdylib(m, "StateModuleImpl", "state_module_plugin.h");

    EXPECT_TRUE(source.contains("const LogosMap& state"));
    EXPECT_TRUE(source.contains("#include <logos_json.h>"));

    // No Qt type may appear anywhere in a Qt-free TU.
    EXPECT_FALSE(source.contains("QVariant"));
}

// `[bstr]` used to be rejected by name: the gate whitelisted element types and
// the emitter had no way to keep a nested byte string tagged. Both are fixed —
// the gate recurses and the generated codec's full specialization for
// std::vector<uint8_t> wins over its generic vector rule at every depth — so
// the type is now SUPPORTED. Inverted deliberately rather than deleted: the
// cell it used to pin still matters, only its answer changed.
TEST(LidlGenCdylib, ArrayOfBytesEventParamIsSupported)
{
    const ModuleDecl m = moduleWithEvent("batchReceived", {
        param("payloads", TypeExpr{TypeExpr::Array, "", {prim("bstr")}}),
    });

    QString error;
    EXPECT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();

    const QString source = eventsSourceFor(m);
    // Spelled Qt-free and encoded through the codec, so each element keeps its
    // canonical tag instead of becoming a plain array of numbers.
    EXPECT_TRUE(source.contains("std::vector<std::vector<uint8_t>>")) << source.toStdString();
    EXPECT_TRUE(source.contains("logos_gen::Codec<std::vector<std::vector<uint8_t>>>::to(payloads)"))
        << source.toStdString();
}

// The gate recurses, so what it refuses is now a property of the leaf. A map
// with a non-tstr key has no C++ spelling (the codec spells a map as
// std::map<std::string, T>) and must still be refused BY NAME — it used to be
// admitted by a blanket `return true` for any map and then silently flattened
// to an untyped LogosMap, losing the key type.
TEST(LidlGenCdylib, NonStringMapKeyIsRejected)
{
    ModuleDecl m;
    m.name = "k_module";
    MethodDecl md;
    md.name = "takeOddMap";
    md.returnType = prim("tstr");
    ParamDecl p;
    p.name = "m";
    p.type = TypeExpr{TypeExpr::Map, "", {prim("int"), prim("tstr")}};
    md.params.push_back(p);
    m.methods.push_back(md);

    QString error;
    EXPECT_FALSE(lidlCdylibSupported(m, &error));
    EXPECT_TRUE(error.contains("takeOddMap")) << error.toStdString();
}

// A record the contract declares is admitted and spelled as its struct; an
// UNDECLARED Named type is not. `void` is the reason that distinction has to
// exist — it is not a LIDL builtin, so `-> void` arrives as Named("void").
TEST(LidlGenCdylib, OnlyDeclaredRecordsAreRecords)
{
    ModuleDecl m;
    m.name = "r_module";

    TypeDecl rec;
    rec.name = "Blob";
    FieldDecl f;
    f.name = "payload";
    f.type = prim("bstr");
    rec.fields = {f};
    m.types.push_back(rec);

    MethodDecl good;
    good.name = "echoBlob";
    good.returnType = TypeExpr{TypeExpr::Named, "Blob", {}};
    ParamDecl gp; gp.name = "v"; gp.type = TypeExpr{TypeExpr::Named, "Blob", {}};
    good.params.push_back(gp);
    m.methods.push_back(good);

    QString error;
    EXPECT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();

    // The struct and its codec specialization are emitted.
    const QString types = lidlMakeTypesHeaderCdylib(m);
    // Forward-declared, not defined: the struct is the author's (the contract
    // was derived from that very declaration), so emitting it again would be a
    // redefinition.
    EXPECT_TRUE(types.contains("struct Blob;")) << types.toStdString();
    EXPECT_FALSE(types.contains("struct Blob {")) << types.toStdString();
    EXPECT_TRUE(types.contains("template <> struct Codec<Blob>")) << types.toStdString();
    // The bstr field goes through the bytes codec, not nlohmann's array-of-numbers.
    EXPECT_TRUE(types.contains("Codec<std::vector<uint8_t>>::to(v.payload)")) << types.toStdString();

    // An undeclared Named type is NOT a record and stays refused.
    MethodDecl bad;
    bad.name = "takeGhost";
    bad.returnType = prim("tstr");
    ParamDecl bp; bp.name = "g"; bp.type = TypeExpr{TypeExpr::Named, "Ghost", {}};
    bad.params.push_back(bp);
    m.methods.push_back(bad);
    EXPECT_FALSE(lidlCdylibSupported(m, &error));
    EXPECT_TRUE(error.contains("takeGhost")) << error.toStdString();
}

// The supported scalar / bytes payloads stay eligible.
TEST(LidlGenCdylib, SupportedEventParamsRemainEligible)
{
    const ModuleDecl m = moduleWithEvent("messageReceived", {
        param("messageHash", prim("tstr")),
        param("payload",     prim("bstr")),
        param("timestamp",   prim("int")),
    });

    QString error;
    EXPECT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();
}
