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

// `[bstr]` is in the supported subset: each element carries the canonical
// tagged form, so a module can take or return a list of blobs (e.g. a program
// plus its dependency ELFs) instead of hand-encoding them as hex strings.
//
// #111 reached this with a dedicated depth-1 list codec; the gate now RECURSES
// and the generated Codec's full specialization for std::vector<uint8_t> beats
// its generic vector rule, so the same mechanism covers [bstr], [[bstr]] and
// {tstr: [bstr]}. The assertions moved to that mechanism; what they pin did not.
TEST(LidlGenCdylib, ArrayOfBytesEventParamIsEligibleAndTagsEachElement)
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
    EXPECT_TRUE(source.contains("logos::toJson<std::vector<std::vector<uint8_t>>>(payloads)"))
        << source.toStdString();
    // From #111, still exactly right: Qt-free, and taken by const-ref like the
    // other composite payloads.
    EXPECT_TRUE(source.contains("const std::vector<std::vector<uint8_t>>& payloads"))
        << source.toStdString();
    EXPECT_FALSE(source.contains("QVariant")) << source.toStdString();
}

// Ported from #111. Its assertions named that PR's depth-1 helpers
// (lidlBytesListFromJson / lidlBytesListToJson); the generated Codec subsumes
// them, so the assertions moved to the codec while what they pin — per-element
// tagging, and never nlohmann's blanket container conversion — did not.
TEST(LidlGenCdylib, ArrayOfBytesMethodParamDecodesPerElement)
{
    const ModuleDecl m = moduleWithMethod(method("send", prim("tstr"), {
        param("program_elf",          prim("bstr")),
        param("program_dependencies", TypeExpr{TypeExpr::Array, "", {prim("bstr")}}),
    }));

    QString error;
    ASSERT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();

    const QString source = implSourceFor(m);

    EXPECT_TRUE(source.contains("logos::fromJson<std::vector<std::vector<uint8_t>>>("))
        << source.toStdString();
    // The scalar param decodes leniently too — and now through the SAME
    // function as the nested one. It used to be a separate emitted helper, so a
    // scalar bstr accepted a plain string while a [bstr] element rejected it:
    // echoBytes("hi") worked and echoBytesList(["hi"]) threw, inside one module.
    EXPECT_TRUE(source.contains("logos::bytesFromJsonLenient(")) << source.toStdString();
    // nlohmann's blanket container decode must not be used for this type: it
    // refuses a tagged object and would silently accept a raw number array,
    // skipping the base64 decode entirely.
    EXPECT_FALSE(source.contains(".get<std::vector<std::vector<uint8_t>>>()"))
        << source.toStdString();
}

// Ported from #111: a `[bstr]` RETURN tags each element.
// nlohmann::json(std::vector<std::vector<uint8_t>>) would emit nested number
// arrays, which no consumer decodes as bytes.
TEST(LidlGenCdylib, ArrayOfBytesReturnTagsEachElement)
{
    const ModuleDecl m = moduleWithMethod(
        method("fetchAll", TypeExpr{TypeExpr::Array, "", {prim("bstr")}}, {}));

    QString error;
    ASSERT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();

    const QString source = implSourceFor(m);
    EXPECT_TRUE(source.contains("logos::toJson<std::vector<std::vector<uint8_t>>>("))
        << source.toStdString();
    EXPECT_FALSE(source.contains("nlohmann::json(result)")) << source.toStdString();
}

// #111 gated its list encoder so a module that never carries `[bstr]` did not
// gain an unused static function. The generic codec is a TEMPLATE — it only
// instantiates where used — so that hazard is gone and there is no dedicated
// list encoder to omit. What still needs gating is the SCALAR encoder, and it
// still is; this pins both halves so neither regresses.
TEST(LidlGenCdylib, NoDedicatedListEncoderAndTheScalarOneStaysGated)
{
    const ModuleDecl noBytes = moduleWithEvent("fault", {
        param("code",    prim("int")),
        param("message", prim("tstr")),
    });
    const QString plain = eventsSourceFor(noBytes);
    EXPECT_FALSE(plain.contains("lidlBytesToJson")) << plain.toStdString();
    EXPECT_FALSE(plain.contains("lidlBytesListToJson")) << plain.toStdString();

    const ModuleDecl withList = moduleWithEvent("batchReceived", {
        param("payloads", TypeExpr{TypeExpr::Array, "", {prim("bstr")}}),
    });
    const QString listed = eventsSourceFor(withList);
    // The list rides the codec; no bespoke list encoder is emitted at all.
    EXPECT_FALSE(listed.contains("lidlBytesListToJson")) << listed.toStdString();
    EXPECT_TRUE(listed.contains("logos::toJson<std::vector<std::vector<uint8_t>>>("))
        << listed.toStdString();
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
    // Specialized into logos::detail, beside the primary template it specializes,
    // and spelled ::Blob because the author's struct is at global scope while
    // this is namespace logos::detail.
    EXPECT_TRUE(types.contains("template <> struct Codec<::Blob, void>")) << types.toStdString();
    EXPECT_TRUE(types.contains("namespace logos { namespace detail {")) << types.toStdString();
    // The bstr field goes through the bytes codec, not nlohmann's array-of-numbers.
    EXPECT_TRUE(types.contains("Codec<std::vector<uint8_t>>::to(v.payload)")) << types.toStdString();
    // The generic half is NOT emitted any more — it comes from logos_codec.h.
    EXPECT_TRUE(types.contains("#include <logos_codec.h>")) << types.toStdString();
    EXPECT_FALSE(types.contains("namespace logos_gen")) << types.toStdString();
    EXPECT_FALSE(types.contains("struct Codec<int64_t>")) << types.toStdString();

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
