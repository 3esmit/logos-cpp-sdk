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
    EXPECT_TRUE(source.contains("args.push_back(lidlBytesListToJson(payloads));"));
    EXPECT_TRUE(source.contains("nlohmann::json lidlBytesListToJson"));
    EXPECT_TRUE(source.contains("out.push_back(lidlBytesToJson(bytes));"));

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

    EXPECT_TRUE(source.contains("lidlBytesListFromJson("));
    EXPECT_TRUE(source.contains("std::vector<std::vector<uint8_t>> lidlBytesListFromJson"));
    EXPECT_TRUE(source.contains("out.push_back(lidlBytesFromJson(e));"));
    // The scalar param still uses the scalar decoder.
    EXPECT_TRUE(source.contains("lidlBytesFromJson("));
    // The blanket container decode must not be used for this type.
    EXPECT_FALSE(source.contains(".get<std::vector<std::vector<uint8_t>>>()"));
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
    EXPECT_TRUE(source.contains("lidlBytesListToJson("));
    EXPECT_TRUE(source.contains("nlohmann::json lidlBytesListToJson"));
}

// The list encoder is gated the same way the scalar one is: a module whose
// events carry only a single blob must not gain an unused static function.
TEST(LidlGenCdylib, BytesListEncoderOmittedWhenNoEventCarriesAnArray)
{
    const ModuleDecl m = moduleWithEvent("messageReceived", {
        param("payload", prim("bstr")),
    });

    const QString source = eventsSourceFor(m);

    EXPECT_TRUE(source.contains("nlohmann::json lidlBytesToJson"));
    EXPECT_FALSE(source.contains("lidlBytesListToJson"));
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
