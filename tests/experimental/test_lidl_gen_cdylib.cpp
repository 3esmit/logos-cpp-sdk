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

    // The real argument is serialized, through THE canonical encoder — the one
    // in logos-protocol's logos_codec.h, reached via "<module>_types.h". The
    // sidecar used to emit its own base64 encoder beside this call.
    EXPECT_TRUE(source.contains("args.push_back(logos::bytesToJson(payload));"));
    EXPECT_FALSE(source.contains("lidlB64UrlEncode"));
    EXPECT_FALSE(source.contains("lidlBytesToJson"));

    // ...and the empty tagged value is gone.
    EXPECT_FALSE(source.contains("nlohmann::json{{\"_bytes\", \"\"}}"));

    // The other parameters are still passed straight through.
    EXPECT_TRUE(source.contains("args.push_back(messageHash);"));
    EXPECT_TRUE(source.contains("args.push_back(timestamp);"));

    // Bytes are taken by const-ref, matching the author's logos_events: block.
    EXPECT_TRUE(source.contains("const std::vector<uint8_t>& payload"));
}

// No module carries a local base64 codec any more — not the ones with binary
// events and not the ones without. The gate that used to decide which got one
// is gone with it.
TEST(LidlGenCdylib, NoModuleEmitsItsOwnBase64Codec)
{
    const ModuleDecl bytes = moduleWithEvent("messageReceived", {
        param("payload", prim("bstr")),
    });
    const ModuleDecl plain = moduleWithEvent("fault", {
        param("code",    prim("int")),
        param("message", prim("tstr")),
        param("fatal",   prim("bool")),
    });

    for (const QString& source : {eventsSourceFor(bytes), eventsSourceFor(plain),
                                  implSourceFor(bytes), implSourceFor(plain)}) {
        EXPECT_FALSE(source.contains("lidlB64UrlEncode")) << source.toStdString();
        EXPECT_FALSE(source.contains("lidlB64Idx")) << source.toStdString();
        EXPECT_FALSE(source.contains("lidlBytesToJson")) << source.toStdString();
        // The decoder had no call site at all after #117 — emitted into every
        // module and never once called.
        EXPECT_FALSE(source.contains("lidlBytesFromJson")) << source.toStdString();
        EXPECT_FALSE(source.contains(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"))
            << source.toStdString();
    }
    EXPECT_TRUE(eventsSourceFor(plain).contains("args.push_back(code);"));
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

// ---------------------------------------------------------------------------
// Optionality — `?T` on the Qt-free cdylib surface.
//
// Two-state (a value of T, or empty), std::optional<T>, and the wire rule that
// depends on the SLOT: empty omits the key where the slot is named (a record
// field) and is spelled null where it is positional (an argument, a return, an
// event parameter — those have no key to omit and their arity must not change).
// ---------------------------------------------------------------------------

TypeExpr opt(const TypeExpr& inner)
{
    return {TypeExpr::Optional, "", {inner}};
}

TypeExpr arr(const TypeExpr& elem)
{
    return {TypeExpr::Array, "", {elem}};
}

TypeExpr map(const TypeExpr& key, const TypeExpr& value)
{
    return {TypeExpr::Map, "", {key, value}};
}

FieldDecl field(const char* name, const TypeExpr& type)
{
    FieldDecl f;
    f.name = name;
    f.type = type;
    return f;
}

// `?T` used to be a HARD REJECT — "module not cdylib-eligible" — so nothing
// downstream could even be reached. The gate opens exactly as far as the value
// type allows.
TEST(LidlGenCdylib, OptionalIsEligibleWhenItsValueTypeIs)
{
    ModuleDecl m;
    m.name = "o_module";
    m.methods.push_back(method("echoOptional", opt(prim("tstr")),
                               {param("v", opt(prim("tstr")))}));
    m.methods.push_back(method("nested", opt(TypeExpr{TypeExpr::Array, "", {prim("bstr")}}),
                               {param("v", TypeExpr{TypeExpr::Array, "", {opt(prim("int"))}})}));

    QString error;
    EXPECT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();
}

// `result` and `void` are return-only spellings, and neither can be optional:
// void is the absence of a value, and result already carries its own
// success/error discriminant. The value type is checked as a non-return
// position, which is what makes both fall out.
TEST(LidlGenCdylib, OptionalResultAndVoidAreRejected)
{
    for (const char* n : {"result", "void"}) {
        ModuleDecl m;
        m.name = "o_module";
        m.methods.push_back(method("bad", opt(prim(n)), {}));
        QString error;
        EXPECT_FALSE(lidlCdylibSupported(m, &error)) << n;
        EXPECT_TRUE(error.contains("bad")) << error.toStdString();
    }
}

// R3. `? name: T` (the field flag) and `name: ?T` (the type kind) are the same
// declaration and MUST emit byte-identical code. Nothing enforced that before —
// a backend reading only one of the two would have silently disagreed with the
// next one to try.
TEST(LidlGenCdylib, BothOptionalSpellingsEmitIdenticalCode)
{
    auto moduleWithField = [](const FieldDecl& f) {
        ModuleDecl m;
        m.name = "o_module";
        TypeDecl t;
        t.name = "Opt";
        t.fields = {f};
        m.types.push_back(t);
        return m;
    };

    FieldDecl flagged = field("maybe", prim("tstr"));
    flagged.optional = true;                       // `? maybe: tstr`
    const FieldDecl typed = field("maybe", opt(prim("tstr")));  // `maybe: ?tstr`

    const QString a = lidlMakeTypesHeaderCdylib(moduleWithField(flagged));
    const QString b = lidlMakeTypesHeaderCdylib(moduleWithField(typed));
    EXPECT_EQ(a, b) << a.toStdString() << "\n---\n" << b.toStdString();
    EXPECT_TRUE(a.contains("std::optional<std::string>")) << a.toStdString();
}

// R2, named slot: empty OMITS the key. Writing null instead would be the
// positional spelling in a slot that has a name — and Codec<std::optional<T>>
// cannot do this itself, because a codec only ever sees a value, never the slot.
TEST(LidlGenCdylib, OptionalRecordFieldOmitsTheKeyWhenEmpty)
{
    ModuleDecl m;
    m.name = "o_module";
    TypeDecl t;
    t.name = "Opt";
    t.fields = {field("required", prim("tstr")), field("maybe", opt(prim("tstr")))};
    m.types.push_back(t);

    const QString types = lidlMakeTypesHeaderCdylib(m);
    EXPECT_TRUE(types.contains("if (v.maybe.has_value())")) << types.toStdString();
    EXPECT_TRUE(types.contains("out[\"maybe\"] = Codec<std::string>::to(*v.maybe);"))
        << types.toStdString();
    // Decode needs no optional branch: an absent key is ALREADY materialised as
    // null right there, so absent and explicit null reach the codec
    // indistinguishable — nullopt in an optional field, still an error in a
    // required one.
    EXPECT_TRUE(types.contains("out.maybe = Codec<std::optional<std::string>>::from("))
        << types.toStdString();
    EXPECT_TRUE(types.contains("j.contains(\"maybe\") ? j.at(\"maybe\") : nlohmann::json()"))
        << types.toStdString();
    // The required field is untouched by any of this.
    EXPECT_TRUE(types.contains("out[\"required\"] = Codec<std::string>::to(v.required);"))
        << types.toStdString();
    EXPECT_TRUE(types.contains("#include <optional>")) << types.toStdString();
}

// A contract with no optional keeps its generated output byte-for-byte, down to
// the include list — every cpp-sdk change rebuilds the whole module graph, so a
// gratuitous diff here is a rebuild of everything.
TEST(LidlGenCdylib, NoOptionalMeansNoOptionalInclude)
{
    ModuleDecl m;
    m.name = "o_module";
    TypeDecl t;
    t.name = "Plain";
    t.fields = {field("id", prim("tstr"))};
    m.types.push_back(t);

    EXPECT_FALSE(lidlMakeTypesHeaderCdylib(m).contains("#include <optional>"));
}

// R2, positional slot: arity never changes on the way OUT, but absent and null
// are the same state coming IN — so the gate admits a missing trailing optional
// and materialises it as null, exactly the way a missing record field already is.
TEST(LidlGenCdylib, OptionalArgumentMayBeAbsentOrNull)
{
    ModuleDecl m;
    m.name = "o_module";
    m.methods.push_back(method("f", prim("tstr"),
                               {param("required", prim("tstr")),
                                param("maybe", opt(prim("tstr")))}));

    const QString src = lidlMakeModuleImplExports(m, "OImpl", "o_impl.h");
    // The gate counts REQUIRED parameters — the same rule the Rust generator
    // applies, so the two report the same `expected` for the same contract.
    EXPECT_TRUE(src.contains("if (args.size() < 1) {")) << src.toStdString();
    EXPECT_TRUE(src.contains("\"expected 1 arguments, got \"")) << src.toStdString();
    EXPECT_TRUE(src.contains("(args.size() > 1 ? args.at(1) : nlohmann::json())"))
        << src.toStdString();
    EXPECT_TRUE(src.contains("logos::fromJson<std::optional<std::string>>"))
        << src.toStdString();
    // The REQUIRED argument keeps the hard gate and the plain accessor.
    EXPECT_TRUE(src.contains("logos::fromJson<std::string>(args.at(0), \"arg0\")"))
        << src.toStdString();
}

// A wrong argument COUNT is reported, in the shape logos-rust-sdk's
// args::invalid_args() emits — same three keys, same message text, same origin.
// It used to `return nullptr`, which the Qt glue turns into an empty QVariant:
// "you passed 1 of 2 arguments" was indistinguishable from a successful empty
// answer.
TEST(LidlGenCdylib, WrongArgumentCountReportsInvalidArgs)
{
    ModuleDecl m;
    m.name = "o_module";
    m.methods.push_back(method("f", prim("tstr"),
                               {param("a", prim("tstr")), param("b", prim("tstr"))}));

    const QString src = lidlMakeModuleImplExports(m, "OImpl", "o_impl.h");
    EXPECT_TRUE(src.contains("if (args.size() < 2) {")) << src.toStdString();
    EXPECT_TRUE(src.contains("{\"code\", \"invalid_args\"}")) << src.toStdString();
    EXPECT_TRUE(src.contains(
        "{\"message\", \"expected 2 arguments, got \" + std::to_string(args.size())}"))
        << src.toStdString();
    EXPECT_TRUE(src.contains("{\"origin\", \"o_module\"}")) << src.toStdString();
    EXPECT_TRUE(src.contains("return lidlStrdup(err.dump());")) << src.toStdString();
    // The silent reply is gone from the arity path.
    EXPECT_FALSE(src.contains("if (args.size() < 2) return nullptr;")) << src.toStdString();
    EXPECT_FALSE(src.contains("args.size() > ")) << src.toStdString();
}

// `args.size()` is unsigned, so `< 0` never fires: a zero-argument method
// carried a dead branch. The Rust generator has always skipped it; now both do.
TEST(LidlGenCdylib, ZeroArgumentMethodEmitsNoArityGate)
{
    ModuleDecl m;
    m.name = "o_module";
    m.methods.push_back(method("ping", prim("tstr"), {}));

    const QString src = lidlMakeModuleImplExports(m, "OImpl", "o_impl.h");
    EXPECT_FALSE(src.contains("args.size() < 0")) << src.toStdString();
    EXPECT_FALSE(src.contains("invalid_args")) << src.toStdString();
    EXPECT_TRUE(src.contains("lidlImpl().ping()")) << src.toStdString();
}

// R4. Optional widens the accepted domain by exactly ONE inhabitant (empty); a
// present value is still decoded as T. For `bstr` that has to be the LENIENT
// decode a bare `bstr` argument gets, or the identical value would be accepted
// in a required slot and rejected in an optional one.
TEST(LidlGenCdylib, OptionalBytesArgumentKeepsTheLenientDecode)
{
    ModuleDecl m;
    m.name = "o_module";
    m.methods.push_back(method("f", prim("bool"), {param("v", opt(prim("bstr")))}));

    const QString src = lidlMakeModuleImplExports(m, "OImpl", "o_impl.h");
    EXPECT_TRUE(src.contains("logos::bytesFromJsonLenient")) << src.toStdString();
    EXPECT_TRUE(src.contains(".is_null() ? std::optional<std::vector<uint8_t>>()"))
        << src.toStdString();
}

// `?any` collapses onto `any`. nlohmann::json already HAS null among its
// inhabitants, so std::optional<LogosMap> would give the slot two spellings of
// empty — three-state, which is the one thing `?T` may never be.
TEST(LidlGenCdylib, OptionalAnyCollapsesOntoAny)
{
    ModuleDecl m;
    m.name = "o_module";
    TypeDecl t;
    t.name = "Loose";
    t.fields = {field("blob", opt(prim("any")))};
    m.types.push_back(t);
    m.methods.push_back(method("f", prim("bool"), {param("v", opt(prim("any")))}));

    QString error;
    ASSERT_TRUE(lidlCdylibSupported(m, &error)) << error.toStdString();

    const QString types = lidlMakeTypesHeaderCdylib(m);
    EXPECT_TRUE(types.contains("Codec<LogosMap>::to(v.blob)")) << types.toStdString();
    EXPECT_FALSE(types.contains("std::optional<LogosMap>")) << types.toStdString();

    const QString src = lidlMakeModuleImplExports(m, "OImpl", "o_impl.h");
    EXPECT_FALSE(src.contains("std::optional<LogosMap>")) << src.toStdString();
}

// An event parameter is a POSITIONAL slot: empty is null, and the argument list
// keeps its length. It is also taken by const reference, like every other
// non-scalar, so the generated definition matches the author's declaration in
// the `logos_events:` block.
TEST(LidlGenCdylib, OptionalEventParamIsConstRefAndNullWhenEmpty)
{
    const ModuleDecl m = moduleWithEvent("changed", {
        param("name",     prim("tstr")),
        param("instance", opt(prim("tstr"))),
    });

    const QString src = eventsSourceFor(m);
    EXPECT_TRUE(src.contains("const std::optional<std::string>& instance"))
        << src.toStdString();
    EXPECT_TRUE(src.contains("args.push_back(logos::toJson<std::optional<std::string>>(instance));"))
        << src.toStdString();
    EXPECT_TRUE(src.contains("#include <optional>")) << src.toStdString();
}

// `{tstr: T}` is the one LIDL type with two C++ spellings (std::map and
// std::unordered_map), and logos_codec.h specializes Codec for both. Naming one
// of them in the generated dispatch made the other a compile error in code the
// author never wrote, so the map slots hand the compiler a proxy / deduce
// instead and let the author's declaration pick.
TEST(LidlGenCdylib, TypedMapBindsTheAuthorsOwnContainer)
{
    ModuleDecl m;
    m.name = "o_module";
    m.methods.push_back(method("echoIntMap", map(prim("tstr"), prim("int")),
                               {param("v", map(prim("tstr"), prim("int")))}));

    const QString src = lidlMakeModuleImplExports(m, "OImpl", "o_impl.h");
    EXPECT_TRUE(src.contains("logos::JsonArg(args.at(0), \"arg0\")")) << src.toStdString();
    EXPECT_TRUE(src.contains("logos::toJson(result)")) << src.toStdString();
    EXPECT_FALSE(src.contains("logos::fromJson<std::map<std::string, int64_t>>"))
        << src.toStdString();
    EXPECT_FALSE(src.contains("logos::toJson<std::map<std::string, int64_t>>"))
        << src.toStdString();
}

// ...and only maps. Every other type has one C++ spelling here, and JsonArg
// documents one target it cannot serve — std::optional<X>, whose converting
// constructor out-ranks the proxy's conversion operator, so an empty optional
// would decode as a wrong-typed X and throw.
TEST(LidlGenCdylib, NonMapSlotsStillNameTheirType)
{
    ModuleDecl m;
    m.name = "o_module";
    m.methods.push_back(method("f", prim("bool"),
                               {param("a", arr(prim("int"))),
                                param("b", opt(map(prim("tstr"), prim("tstr"))))}));

    const QString src = lidlMakeModuleImplExports(m, "OImpl", "o_impl.h");
    EXPECT_TRUE(src.contains("logos::fromJson<std::vector<int64_t>>")) << src.toStdString();
    EXPECT_TRUE(src.contains("logos::fromJson<std::optional<std::map<std::string, std::string>>>"))
        << src.toStdString();
    EXPECT_FALSE(src.contains("logos::JsonArg")) << src.toStdString();
}
