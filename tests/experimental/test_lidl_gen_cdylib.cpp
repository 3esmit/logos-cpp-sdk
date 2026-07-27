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

MethodDecl method(const char* name, const TypeExpr& returnType,
                  const std::vector<ParamDecl>& params)
{
    MethodDecl md;
    md.name = name;
    md.returnType = returnType;
    md.params = params;
    return md;
}

ModuleDecl moduleWithMethod(const MethodDecl& md)
{
    ModuleDecl m;
    m.name = "delivery_module";
    m.methods.push_back(md);
    return m;
}

QString implSourceFor(const ModuleDecl& m)
{
    return lidlMakeModuleImplExports(m, "DeliveryModuleImpl", "delivery_module_plugin.h");
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

    // The real argument is serialized through the canonical encoder, which now
    // lives in logos-protocol instead of being emitted into every module.
    EXPECT_TRUE(source.contains("args.push_back(logos::toJson(payload));"));
    EXPECT_TRUE(source.contains("#include <logos_codec.h>"));
    EXPECT_FALSE(source.contains("lidlB64UrlEncode"));
    EXPECT_FALSE(source.contains("lidlBytesToJson"));

    // ...and the empty tagged value is gone.
    EXPECT_FALSE(source.contains("nlohmann::json{{\"_bytes\", \"\"}}"));

    // Every parameter goes through the same call — no per-type special cases.
    EXPECT_TRUE(source.contains("args.push_back(logos::toJson(messageHash));"));
    EXPECT_TRUE(source.contains("args.push_back(logos::toJson(timestamp));"));

    // Bytes are taken by const-ref, matching the author's logos_events: block.
    EXPECT_TRUE(source.contains("const std::vector<uint8_t>& payload"));
}

// No module carries a codec copy any more: the base64/tagged-bytes
// implementation lives once in logos-protocol (logos_codec.h) and modules call
// it. This also retires the gating that existed only to avoid emitting an
// unused static function.
TEST(LidlGenCdylib, NoCodecIsEmittedIntoTheModule)
{
    const ModuleDecl m = moduleWithEvent("fault", {
        param("code",    prim("int")),
        param("message", prim("tstr")),
        param("fatal",   prim("bool")),
    });

    const QString source = eventsSourceFor(m);

    EXPECT_FALSE(source.contains("lidlB64UrlEncode"));
    EXPECT_FALSE(source.contains("lidlBytesToJson"));
    EXPECT_FALSE(source.contains("lidlB64Idx"));
    EXPECT_TRUE(source.contains("#include <logos_codec.h>"));
    EXPECT_TRUE(source.contains("args.push_back(logos::toJson(code));"));
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

// `[bstr]` is in the supported subset: each element carries the canonical tagged
// form, so a module can take or return a list of blobs (e.g. a program plus its
// dependency ELFs) instead of hand-encoding them as hex strings.
TEST(LidlGenCdylib, ArrayOfBytesEventParamIsEligibleAndTagsEachElement)
{
    const ModuleDecl m = moduleWithEvent("batchReceived", {
        param("payloads", TypeExpr{TypeExpr::Array, "", {prim("bstr")}}),
    });

    QString error;
    EXPECT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();

    const QString source = eventsSourceFor(m);

    // Each element is tagged, not emitted as a nested number array — which is
    // what nlohmann::json(std::vector<std::vector<uint8_t>>) would have produced
    // and no consumer decodes as bytes.
    EXPECT_TRUE(source.contains("args.push_back(logos::toJson(payloads));"));

    // Qt-free, and taken by const-ref like the other composite payloads.
    EXPECT_TRUE(source.contains("const std::vector<std::vector<uint8_t>>& payloads"));
    EXPECT_FALSE(source.contains("QVariant"));
}

// The method path: a `[bstr]` parameter must be DECODED per element, never via
// nlohmann's blanket get<>(). get<std::vector<std::vector<uint8_t>>>() throws on
// the tagged {"_bytes": …} object form, and would silently skip the base64
// decode for a number-array element.
TEST(LidlGenCdylib, ArrayOfBytesMethodParamDecodesPerElement)
{
    const ModuleDecl m = moduleWithMethod(method("send", prim("tstr"), {
        param("program_elf",          prim("bstr")),
        param("program_dependencies", TypeExpr{TypeExpr::Array, "", {prim("bstr")}}),
    }));

    QString error;
    ASSERT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();

    const QString source = implSourceFor(m);

    // Both params decode through the proxy, which converts itself into the
    // author's own parameter type — so no type name is emitted at all and the
    // scalar/array distinction needs no special case.
    EXPECT_TRUE(source.contains("logos::JsonArg{args.at(0), \"arg0\"}"));
    EXPECT_TRUE(source.contains("logos::JsonArg{args.at(1), \"arg1\"}"));
    // The blanket container decode must not be used for any type.
    EXPECT_FALSE(source.contains(".get<std::vector<"));
}

// The return path: nlohmann::json(std::vector<std::vector<uint8_t>>) would emit
// nested number arrays, which no consumer decodes as bytes.
TEST(LidlGenCdylib, ArrayOfBytesReturnTagsEachElement)
{
    const ModuleDecl m = moduleWithMethod(
        method("dependencies", TypeExpr{TypeExpr::Array, "", {prim("bstr")}}, {}));

    QString error;
    ASSERT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();

    const QString source = implSourceFor(m);
    EXPECT_TRUE(source.contains("logos::toJson(result)"));
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

// Numbers are 64-bit only. A narrower spelling is NOT auto-widened: widening
// would make the declared C++ type and the published LIDL contract disagree
// about range, so it is a build error that names the type and the fix.
TEST(LidlGenCdylib, NarrowNumericSpellingsAreRejectedWithTheFix)
{
    struct Case { const char* cpp; const char* hint; };
    const Case cases[] = {
        {"uint32_t", "uint64_t"},
        {"int",      "int64_t"},
        {"size_t",   "uint64_t"},
        {"float",    "double"},
    };

    for (const Case& c : cases) {
        const ModuleDecl m = moduleWithMethod(method("f", prim("tstr"), {
            param("n", TypeExpr{TypeExpr::Named, c.cpp, {}}),
        }));
        QString error;
        EXPECT_FALSE(lidlCdylibSupported(m, &error)) << c.cpp;
        EXPECT_TRUE(error.contains(c.cpp)) << error.toStdString();
        EXPECT_TRUE(error.contains(c.hint)) << error.toStdString();
    }

    // uint8_t names its one legitimate use rather than just the width rule.
    const ModuleDecl m = moduleWithMethod(method("f", prim("tstr"), {
        param("b", TypeExpr{TypeExpr::Named, "uint8_t", {}}),
    }));
    QString error;
    EXPECT_FALSE(lidlCdylibSupported(m, &error));
    EXPECT_TRUE(error.contains("std::vector<uint8_t>")) << error.toStdString();
}

// A narrow spelling nested inside a supported container is rejected too, and the
// message still names it — the recursion must not lose the offender.
TEST(LidlGenCdylib, NarrowNumericInsideAContainerIsRejected)
{
    const ModuleDecl m = moduleWithMethod(method("f", prim("tstr"), {
        param("counters", TypeExpr{TypeExpr::Array, "", {TypeExpr{TypeExpr::Named, "uint32_t", {}}}}),
    }));
    QString error;
    EXPECT_FALSE(lidlCdylibSupported(m, &error));
    EXPECT_TRUE(error.contains("uint32_t")) << error.toStdString();
    EXPECT_TRUE(error.contains("counters")) << error.toStdString();
}
