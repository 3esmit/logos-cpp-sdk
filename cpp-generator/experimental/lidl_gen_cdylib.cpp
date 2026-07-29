#include "lidl_gen_cdylib.h"
#include "lidl_emit_common.h"

#include <QTextStream>

#include <set>
#include <string>

QString lidlToPascalCase(const QString& name);
QString lidlTypeToQt(const TypeExpr& te);
bool lidlIsStdConvertible(const TypeExpr& te);

namespace {

// The cdylib-supported subset: std-convertible LIDL types only — the same
// Qt-free set the std apiStyle handled, so any universal module that built
// under std also builds as a header-first cdylib.
// The records a contract DECLARES. A `Named` type is a record only if it is in
// here: `void` is not a LIDL builtin, so `-> void` arrives as Named("void") and
// treating every Named as a record is how the Rust generator once emitted
// `-> Void`. Same trap, same guard.
std::set<std::string> recordNames(const ModuleDecl& module)
{
    std::set<std::string> out;
    for (const TypeDecl& t : module.types) out.insert(t.name);
    return out;
}

bool isRecord(const TypeExpr& te, const std::set<std::string>& recs)
{
    return te.kind == TypeExpr::Named && recs.count(te.name) > 0;
}

bool typeSupported(const TypeExpr& te, bool isReturn, const std::set<std::string>& recs)
{
    if (te.kind == TypeExpr::Primitive) {
        if (te.name == "tstr" || te.name == "bstr" || te.name == "int"
            || te.name == "uint" || te.name == "float64" || te.name == "bool")
            return true;
        // any (LogosMap/LogosList/json) routes through nlohmann in either
        // direction; result (StdLogosResult) and void only make sense as a
        // return. All Qt-free.
        if (te.name == "any")
            return true;
        if (isReturn && (te.name == "result" || te.name == "void"))
            return true;
        return false;
    }
    // A declared record is a generated struct with a generated codec.
    if (isRecord(te, recs))
        return true;
    // Recurse rather than whitelisting element names: that admits [bstr],
    // [[int]], [Record] and [{tstr: T}] in one rule, and keeps the gate and
    // the spelling function agreeing about what is expressible.
    if (te.kind == TypeExpr::Array && te.elements.size() == 1)
        return typeSupported(te.elements[0], false, recs);
    // Only tstr keys: the generated codec spells a map as
    // std::map<std::string, T>, so a non-tstr key has no C++ spelling. This
    // used to `return true` for ANY map, which admitted `{int: tstr}` and then
    // silently produced a LogosMap that lost the key type.
    if (te.kind == TypeExpr::Map) {
        if (te.elements.size() != 2) return false;
        const TypeExpr& k = te.elements[0];
        if (!(k.kind == TypeExpr::Primitive && k.name == "tstr")) return false;
        return typeSupported(te.elements[1], false, recs);
    }
    return false;
}

// Qt-free spelling of a LIDL type (defined below). Forward-declared so the
// method-param decoder can spell composite `any` containers as their nlohmann
// aliases instead of Qt containers in this Qt-free TU.
QString lidlTypeToStdCdylib(const TypeExpr& te, const std::set<std::string>& recs);

// json arg expression -> std-typed C++ expression
// A method argument, decoded into the author's C++ type.
//
// EVERY typed value goes through the generated codec, which recurses — so a bstr
// keeps its canonical tag at ANY depth, a record decodes field by field with a
// path in the error, and a scalar is checked against its declared type.
//
// The scalars used to keep their nlohmann accessor verbatim, and that was the
// last hole in the type contract on this backend: `.get<uint64_t>()` on -1 wraps
// to 18446744073709551615 with no exception, so `echoUint(-1)` answered
// 18446744073709551615 here and `dispatch_failed` on the Rust provider — a
// silent sign flip on a nominal type, in a contract both providers share.
// `.get<int64_t>()` on 3.7 likewise truncated to 3 instead of rejecting.
//
// The comment that used to sit here justified the leniency by pointing at the
// conformance matrix cells that pinned it. That was circular: those cells exist
// to DOCUMENT the divergence, and their own `why` text says the strict behaviour
// is the correct one. The expectations moved with this change.
//
// `any` still passes through untouched — it is the one LIDL type that declares
// nothing, so there is nothing to check it against.
QString jsonArgToStd(const TypeExpr& te, const QString& expr, const QString& path,
                     const std::set<std::string>& recs)
{
    if (te.kind == TypeExpr::Primitive) {
        if (te.name == "bstr")
            return "logos::bytesFromJsonLenient(" + expr + ", \"" + path + "\")";
        if (te.name == "any")  return expr;
    }
    const QString cpp = lidlTypeToStdCdylib(te, recs);
    if (cpp == "LogosMap" || cpp == "LogosList")
        return expr;  // untyped JSON passes through, as it always has
    return "logos::fromJson<" + cpp + ">(" + expr + ", \"" + path + "\")";
}

// std-typed return variable -> json expression
QString stdReturnToJson(const MethodDecl& md, const QString& var,
                        const std::set<std::string>& recs)
{
    const TypeExpr& te = md.returnType;
    if (md.resultReturn) {
        // StdLogosResult -> the canonical {success, value, error} object
        // (same shape logos_json_convert emits for Qt LogosResult).
        return "lidlResultToJson(" + var + ")";
    }
    // `jsonReturn` is set by the front end for any map/list return, but that no
    // longer implies the C++ type IS nlohmann::json: a TYPED map now spells
    // std::map<std::string, T>. Checking the flag before the spelling emitted
    // `result.dump()` on a std::map. The spelling decides.
    const QString cppRet = lidlTypeToStdCdylib(te, recs);
    if (md.jsonReturn && (cppRet == "LogosMap" || cppRet == "LogosList")) {
        return var;  // LogosMap / LogosList are nlohmann::json already
    }
    if (te.kind == TypeExpr::Primitive) {
        if (te.name == "bstr") return "lidlBytesToJson(" + var + ")";
        if (te.name == "any")  return var;
        return "nlohmann::json(" + var + ")";
    }
    if (cppRet == "LogosMap" || cppRet == "LogosList")
        return var;
    // `nlohmann::json(v)` would serialize a vector<uint8_t> as a plain number
    // array and a record not at all; the codec keeps bytes tagged at depth.
    return "logos::toJson<" + cppRet + ">(" + var + ")";
}

// Qt-free spelling of a LIDL type. lidlTypeToStd() falls back to Qt containers
// (QVariant / QVariantMap / QVariantList) for the composite types, but a cdylib
// TU is Qt-free by definition and typeSupported() admits `any` and maps — so
// spell those as their nlohmann aliases (LogosMap / LogosList) instead. Without
// this the events sidecar emits a bare `QVariant` parameter and does not
// compile.
QString lidlTypeToStdCdylib(const TypeExpr& te, const std::set<std::string>& recs)
{
    if (te.kind == TypeExpr::Primitive && te.name == "any")
        return "LogosMap";
    // `{tstr: any}` and `[any]` keep their nlohmann aliases: every existing
    // universal module spells them that way, and narrowing them would be a
    // source break for no gain (they ARE untyped JSON).
    if (te.kind == TypeExpr::Map && te.elements.size() == 2
        && te.elements[1].kind == TypeExpr::Primitive && te.elements[1].name == "any")
        return "LogosMap";
    if (te.kind == TypeExpr::Array && te.elements.size() == 1
        && te.elements[0].kind == TypeExpr::Primitive
        && te.elements[0].name == "any")
        return "LogosList";

    // A declared record is its generated struct.
    if (isRecord(te, recs))
        return qs(te.name);
    // Recurse, so [bstr] is std::vector<std::vector<uint8_t>> and {tstr: Blob}
    // is std::map<std::string, Blob>. lidlTypeToStd() would answer QVariantList
    // / QVariantMap here — a Qt name in a Qt-FREE translation unit, which only
    // failed to appear because the gate used to reject these types. Widening
    // the gate makes that fallback a live leak, so composites must never reach
    // it.
    if (te.kind == TypeExpr::Array && te.elements.size() == 1)
        return "std::vector<" + lidlTypeToStdCdylib(te.elements[0], recs) + ">";
    if (te.kind == TypeExpr::Map && te.elements.size() == 2)
        return "std::map<std::string, " + lidlTypeToStdCdylib(te.elements[1], recs) + ">";

    return lidlTypeToStd(te);
}

// True when the module declares at least one `bstr` event parameter — the only
// reason the events sidecar needs the bytes encoder. Emitting it unconditionally
// leaves an unused static function (a -Wunused-function warning) in every module
// whose events carry no binary data.
// ── The generated codec ─────────────────────────────────────────────────────
//
// Emitted into the module's types header so the author's impl class and the
// generated dispatch share one definition of how a value crosses the wire.
//
// This is deliberately the same SHAPE as logos-protocol's logos_codec.h — and
// it exists as generated code only because that header cannot currently be
// included here: logos_json.h (which every universal module pulls in for
// LogosMap) and logos_codec.h both define logos::b64UrlEncode /
// b64UrlDecode / bytesToJson as inline, so including both in one translation
// unit is a redefinition error. Unify when that is resolved; the emitted
// specializations would then be the only generated part.
//
// The primary template is intentionally left UNDEFINED: an unsupported T is a
// compile error naming the type, never a silent default-constructed value.
// Emits ONE specialization per record the module declares — and nothing else.
//
// The generic half (scalars, bstr, the vector/map composition, the error paths)
// used to be emitted here too, ~186 lines of C++-emitting-C++ that mirrored
// logos-protocol's logos_codec.h by hand. It no longer is: logos_json.h stopped
// defining byte helpers that collided with that header, so a module TU can now
// include the canonical codec directly.
//
// That duplication was not free. The two copies had drifted (the emitted integer
// decode gated on is_number() where the canonical one checked
// is_number_integer() || is_number_unsigned()), they disagreed on padded base64,
// and every codec fix had to be written twice or it silently only half-applied.
//
// What remains is irreducible: a LIDL `type` is a per-contract struct whose field
// names and member types exist only in this module's header, and C++17 has no
// field reflection. Nesting composes for free — Codec<std::vector<Blob>> and
// deeper come from the shared generic half once Codec<::Blob> exists.
void emitRecordCodecs(QTextStream& s, const ModuleDecl& module,
                      const std::set<std::string>& recs)
{
    if (module.types.empty()) return;
    // Reopened so the specializations land beside the primary template they
    // specialize. `::Name` because the author's record types are at global
    // scope, while this is namespace logos::detail — without the qualifier the
    // name would resolve inside logos::.
    s << "namespace logos { namespace detail {\n\n";
    // One specialization per declared record. Field order follows the contract.
    for (const TypeDecl& t : module.types) {
        const QString name = qs(t.name);
        s << "template <> struct Codec<::" << name << ", void> {\n";
        s << "    static nlohmann::json to(const " << name << "& v) {\n";
        s << "        nlohmann::json out = nlohmann::json::object();\n";
        for (const FieldDecl& f : t.fields) {
            const QString ft = lidlTypeToStdCdylib(f.type, recs);
            s << "        out[\"" << qs(f.name) << "\"] = Codec<" << ft << ">::to(v."
              << qs(f.name) << ");\n";
        }
        s << "        return out;\n    }\n";
        s << "    static " << name << " from(const nlohmann::json& j, const std::string& path) {\n";
        s << "        if (!j.is_object()) detail::typeError(path, \"object\", j);\n";
        s << "        " << name << " out;\n";
        for (const FieldDecl& f : t.fields) {
            const QString ft = lidlTypeToStdCdylib(f.type, recs);
            const QString fn = qs(f.name);
            // A missing field is reported at its own path rather than
            // default-constructed: a record that silently loses a field is the
            // failure mode this whole layer exists to prevent.
            s << "        out." << fn << " = Codec<" << ft << ">::from(\n";
            s << "            j.contains(\"" << fn << "\") ? j.at(\"" << fn
              << "\") : nlohmann::json(),\n";
            s << "            path + \"." << fn << "\");\n";
        }
        s << "        return out;\n    }\n};\n\n";
    }
    s << "}}  // namespace logos::detail\n\n";
}

bool hasBytesEventParam(const ModuleDecl& module)
{
    for (const EventDecl& ed : module.events)
        for (const ParamDecl& pd : ed.params)
            if (pd.type.kind == TypeExpr::Primitive && pd.type.name == "bstr")
                return true;
    return false;
}

// The Qt spelling of what actually crosses the Qt boundary.
//
// NOT lidlTypeToQt: that answers the CONSUMER's question ("what type does the
// caller hold?") and since records became real structs it answers `Blob` /
// `QList<Blob>`. Those names are correct in a generated consumer wrapper, where
// the struct exists — but this JSON is the module's getMethods(), read by the
// host to marshal a QVariant across the plugin boundary, and there is no
// metatype called `Blob`. Emitting it made the host SIGSEGV on the first call
// to any record method.
//
// A record IS a variant map at that boundary; the struct only exists inside the
// cdylib.
QString lidlTypeToQtWire(const TypeExpr& te, const std::set<std::string>& recs)
{
    if (isRecord(te, recs))
        return "QVariantMap";
    if (te.kind == TypeExpr::Array && te.elements.size() == 1
        && isRecord(te.elements[0], recs))
        return "QVariantList";
    if (te.kind == TypeExpr::Map && te.elements.size() == 2
        && isRecord(te.elements[1], recs))
        return "QVariantMap";
    return lidlTypeToQt(te);
}

// True when any event parameter is spelled LogosMap / LogosList, so the sidecar
// needs <logos_json.h> for those aliases.
bool hasJsonEventParam(const ModuleDecl& module)
{
    const std::set<std::string> recs = recordNames(module);
    for (const EventDecl& ed : module.events)
        for (const ParamDecl& pd : ed.params) {
            const QString t = lidlTypeToStdCdylib(pd.type, recs);
            if (t == "LogosMap" || t == "LogosList")
                return true;
        }
    return false;
}

// The SCALAR tagged-bytes helpers. A `[bstr]` (and bytes at any deeper
// nesting) rides logos::Codec instead: its full specialization for
// std::vector<uint8_t> beats the generic vector rule, so one mechanism covers
// [bstr], [[bstr]] and {tstr: [bstr]} alike. #111 emitted a dedicated depth-1
// list codec here; the generic one subsumes it, and keeping both left an
// unused static in every module that mentioned [bstr].
void emitBytesEncodeHelpers(QTextStream& s)
{
    s << "// Canonical tagged bytes form {\"_bytes\": base64url} (see logos_protocol.h)\n";
    s << "std::string lidlB64UrlEncode(const std::vector<uint8_t>& bytes)\n{\n";
    s << "    static const char* alpha = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_\";\n";
    s << "    std::string out;\n";
    s << "    size_t i = 0;\n";
    s << "    while (i + 3 <= bytes.size()) {\n";
    s << "        uint32_t n = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i+1]) << 8) | uint32_t(bytes[i+2]);\n";
    s << "        out += alpha[(n >> 18) & 0x3f]; out += alpha[(n >> 12) & 0x3f];\n";
    s << "        out += alpha[(n >> 6) & 0x3f]; out += alpha[n & 0x3f];\n";
    s << "        i += 3;\n    }\n";
    s << "    if (i < bytes.size()) {\n";
    s << "        uint32_t n = uint32_t(bytes[i]) << 16;\n";
    s << "        if (i + 1 < bytes.size()) n |= uint32_t(bytes[i+1]) << 8;\n";
    s << "        out += alpha[(n >> 18) & 0x3f]; out += alpha[(n >> 12) & 0x3f];\n";
    s << "        if (i + 1 < bytes.size()) out += alpha[(n >> 6) & 0x3f];\n";
    s << "    }\n    return out;\n}\n\n";

    s << "nlohmann::json lidlBytesToJson(const std::vector<uint8_t>& bytes)\n{\n";
    s << "    return nlohmann::json{{\"_bytes\", lidlB64UrlEncode(bytes)}};\n}\n\n";

}

void emitInterfaceJson(QTextStream& s, const ModuleDecl& module)
{
    const std::set<std::string> recs = recordNames(module);
    s << "static nlohmann::json lidlInterfaceJson()\n{\n";
    s << "    nlohmann::json methods = nlohmann::json::array();\n";
    for (const MethodDecl& md : module.methods) {
        s << "    {\n        nlohmann::json obj;\n";
        s << "        obj[\"name\"] = \"" << md.name << "\";\n";
        if (!md.description.empty()) {
            QString esc = qs(md.description);
            esc.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n");
            s << "        obj[\"description\"] = \"" << esc << "\";\n";
        }
        QString sig = qs(md.name) + "(";
        for (int i = 0; i < md.params.size(); ++i) {
            sig += lidlTypeToQtWire(md.params[i].type, recs);
            if (i + 1 < md.params.size()) sig += ",";
        }
        sig += ")";
        s << "        obj[\"signature\"] = \"" << sig << "\";\n";
        s << "        obj[\"returnType\"] = \"" << lidlTypeToQtWire(md.returnType, recs) << "\";\n";
        s << "        obj[\"isInvokable\"] = true;\n";
        if (!md.params.empty()) {
            s << "        nlohmann::json params = nlohmann::json::array();\n";
            for (const ParamDecl& pd : md.params) {
                s << "        params.push_back({{\"type\", \"" << lidlTypeToQtWire(pd.type, recs)
                  << "\"}, {\"name\", \"" << pd.name << "\"}});\n";
            }
            s << "        obj[\"parameters\"] = params;\n";
        }
        s << "        methods.push_back(obj);\n    }\n";
    }
    for (const EventDecl& ed : module.events) {
        s << "    {\n        nlohmann::json obj;\n";
        s << "        obj[\"type\"] = \"event\";\n";
        s << "        obj[\"name\"] = \"" << ed.name << "\";\n";
        if (!ed.description.empty()) {
            QString esc = qs(ed.description);
            esc.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n");
            s << "        obj[\"description\"] = \"" << esc << "\";\n";
        }
        QString sig = qs(ed.name) + "(";
        for (int i = 0; i < ed.params.size(); ++i) {
            sig += lidlTypeToQtWire(ed.params[i].type, recs);
            if (i + 1 < ed.params.size()) sig += ",";
        }
        sig += ")";
        s << "        obj[\"signature\"] = \"" << sig << "\";\n";
        if (!ed.params.empty()) {
            s << "        nlohmann::json params = nlohmann::json::array();\n";
            for (const ParamDecl& pd : ed.params) {
                s << "        params.push_back({{\"type\", \"" << lidlTypeToQtWire(pd.type, recs)
                  << "\"}, {\"name\", \"" << pd.name << "\"}});\n";
            }
            s << "        obj[\"parameters\"] = params;\n";
        }
        s << "        methods.push_back(obj);\n    }\n";
    }
    s << "    return methods;\n}\n\n";
}

} // namespace

bool lidlCdylibSupported(const ModuleDecl& module, QString* error)
{
    const std::set<std::string> recs = recordNames(module);
    for (const MethodDecl& md : module.methods) {
        for (const ParamDecl& pd : md.params) {
            if (!typeSupported(pd.type, /*isReturn=*/false, recs)) {
                if (error)
                    *error = QString("method '%1': parameter '%2' has a type outside the "
                                     "cdylib-supported (Qt-free) subset")
                                 .arg(qs(md.name), qs(pd.name));
                return false;
            }
        }
        // `void` is not a lidlBuiltinType, so the .lidl parser yields it as a
        // Named type "void" (the impl-header parser writes "-> void"); an empty
        // name is the in-memory void from the header path. Treat both as void.
        const bool voidReturn =
            md.returnType.name == "void"
            || (md.returnType.kind == TypeExpr::Primitive && md.returnType.name.empty());
        if (!voidReturn && !md.jsonReturn && !md.resultReturn
            && !typeSupported(md.returnType, /*isReturn=*/true, recs)) {
            if (error)
                *error = QString("method '%1': return type outside the cdylib-supported "
                                 "(Qt-free) subset").arg(qs(md.name));
            return false;
        }
    }
    for (const EventDecl& ed : module.events) {
        for (const ParamDecl& pd : ed.params) {
            if (!typeSupported(pd.type, /*isReturn=*/false, recs)) {
                if (error)
                    *error = QString("event '%1': parameter '%2' has a type outside the "
                                     "cdylib-supported (Qt-free) subset")
                                 .arg(qs(ed.name), qs(pd.name));
                return false;
            }
        }
    }
    return true;
}

QString lidlMakeTypesHeaderCdylib(const ModuleDecl& module)
{
    const std::set<std::string> recs = recordNames(module);
    QString c;
    QTextStream s(&c);
    s << "// AUTO-GENERATED by logos-cpp-generator --backend cdylib -- do not edit\n";
    s << "//\n";
    s << "// The record types `" << module.name << "` declares, plus the codec that moves\n";
    s << "// them across the wire. Qt-FREE. The author's impl header includes this and\n";
    s << "// writes the structs directly:\n";
    s << "//\n";
    s << "//     Blob echoBlob(const Blob& v);\n";
    s << "//\n";
    s << "// rather than picking fields out of a LogosMap.\n";
    s << "#pragma once\n";
    s << "#include <logos_json.h>\n";   // LogosMap / LogosList aliases
    s << "#include <logos_codec.h>\n";  // logos::Codec — the ONE definition
    s << "#include <cstdint>\n";
    s << "#include <map>\n";
    s << "#include <string>\n";
    s << "#include <vector>\n\n";

    // The structs themselves are the AUTHOR's: this file is included after the
    // impl header, and the contract was derived from those very declarations,
    // so emitting them again is a redefinition error. Only forward
    // declarations, so the codec below can name them in any order.
    if (!module.types.empty()) {
        for (const TypeDecl& t : module.types)
            s << "struct " << qs(t.name) << ";\n";
        s << "\n";
    }

    emitRecordCodecs(s, module, recs);
    return c;
}

QString lidlMakeModuleImplExports(const ModuleDecl& module,
                                  const QString& implClass,
                                  const QString& implHeader)
{
    const std::set<std::string> recs = recordNames(module);
    QString c;
    QTextStream s(&c);

    s << "// AUTO-GENERATED by logos-cpp-generator --cdylib -- do not edit\n";
    s << "//\n";
    s << "// The common module-impl C ABI exports (logos_module_impl.h) around the\n";
    s << "// universal impl class `" << implClass << "`. Qt-FREE: compiled into the\n";
    s << "// module's cdylib; the uniform Qt-plugin glue (or a future no-Qt host)\n";
    s << "// drives it exclusively through these symbols.\n";
    s << "#include \"" << implHeader << "\"\n";
    s << "#include \"" << module.name << "_types.h\"\n";
    s << "#include \"logos_module_impl.h\"\n";
    s << "#include \"logos_protocol.h\"\n";
    s << "#include \"logos_module_context.h\"\n";
    s << "#include \"logos_result.h\"\n";
    s << "#include <nlohmann/json.hpp>\n";
    s << "#include <cstdlib>\n";
    s << "#include <cstring>\n";
    s << "#include <atomic>\n";
    s << "#include <map>\n";
    s << "#include <mutex>\n";
    s << "#include <string>\n";
    s << "#include <vector>\n";
    // The Qt-free typed dependency surface: LogosModules (behind modules())
    // built from this module's dependencies (metadata.json#dependencies),
    // calling the lp_* C ABI — no Qt in the cdylib. The umbrella codegen
    // emits logos_sdk.h for every cdylib module (empty when there are no
    // dependencies), so this include is always available.
    s << "#include \"logos_sdk.h\"\n";
    s << "\n";

    // -- shared statics ------------------------------------------------------
    s << "namespace {\n\n";
    s << implClass << "& lidlImpl()\n{\n    static " << implClass << " impl;\n    return impl;\n}\n\n";
    s << "logos_module_emit_cb g_emitCb = nullptr;\n";
    s << "void* g_emitUd = nullptr;\n";
    s << "std::mutex g_emitMutex;\n";
    s << "std::mutex g_ctxMutex;\n";
    s << "bool g_ctxStored = false;\n";
    s << "std::string g_ctxPath, g_ctxId, g_ctxPersist;\n";
    s << "std::atomic<bool> g_hookFired{false};\n\n";

    s << "char* lidlStrdup(const std::string& str)\n{\n";
    s << "    char* out = static_cast<char*>(std::malloc(str.size() + 1));\n";
    s << "    if (out) std::memcpy(out, str.data(), str.size() + 1);\n";
    s << "    return out;\n}\n\n";

    emitBytesEncodeHelpers(s);

    s << "int lidlB64Idx(char ch)\n{\n";
    s << "    if (ch >= 'A' && ch <= 'Z') return ch - 'A';\n";
    s << "    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;\n";
    s << "    if (ch >= '0' && ch <= '9') return ch - '0' + 52;\n";
    s << "    if (ch == '-') return 62;\n    if (ch == '_') return 63;\n    return -1;\n}\n\n";

    s << "std::vector<uint8_t> lidlBytesFromJson(const nlohmann::json& j)\n{\n";
    s << "    std::vector<uint8_t> out;\n";
    s << "    // Lenient bytes decode (matches the std path, where a QString or\n";
    s << "    // QByteArray arg both became bytes): a caller may send the tagged\n";
    s << "    // {\"_bytes\": base64url} form, a plain string (raw UTF-8 bytes), or\n";
    s << "    // an array of byte values. Only the tagged form needs base64.\n";
    s << "    if (j.is_string()) {\n";
    s << "        const std::string s = j.get<std::string>();\n";
    s << "        out.assign(s.begin(), s.end());\n";
    s << "        return out;\n";
    s << "    }\n";
    s << "    if (j.is_number()) {\n";
    s << "        // A number arg becomes its decimal text as bytes — matches\n";
    s << "        // Qt's QVariant(int)->QByteArray, so a caller (or the\n";
    s << "        // logoscore CLI's type auto-detection) passing a bare number\n";
    s << "        // to a bytes param behaves the same as the Qt path.\n";
    s << "        const std::string s = j.dump();\n";
    s << "        out.assign(s.begin(), s.end());\n";
    s << "        return out;\n";
    s << "    }\n";
    s << "    if (j.is_array()) {\n";
    s << "        for (const auto& e : j)\n";
    s << "            if (e.is_number_integer() || e.is_number_unsigned())\n";
    s << "                out.push_back(static_cast<uint8_t>(e.get<int64_t>() & 0xff));\n";
    s << "        return out;\n";
    s << "    }\n";
    s << "    if (!j.is_object() || j.size() != 1 || !j.contains(\"_bytes\") || !j[\"_bytes\"].is_string())\n";
    s << "        return out;\n";
    s << "    const std::string s64 = j[\"_bytes\"].get<std::string>();\n";
    s << "    size_t i = 0;\n";
    s << "    while (i + 4 <= s64.size()) {\n";
    s << "        int a = lidlB64Idx(s64[i]), b = lidlB64Idx(s64[i+1]), c2 = lidlB64Idx(s64[i+2]), d = lidlB64Idx(s64[i+3]);\n";
    s << "        if (a < 0 || b < 0 || c2 < 0 || d < 0) return {};\n";
    s << "        uint32_t n = (uint32_t(a) << 18) | (uint32_t(b) << 12) | (uint32_t(c2) << 6) | uint32_t(d);\n";
    s << "        out.push_back((n >> 16) & 0xff); out.push_back((n >> 8) & 0xff); out.push_back(n & 0xff);\n";
    s << "        i += 4;\n    }\n";
    s << "    size_t rem = s64.size() - i;\n";
    s << "    if (rem == 2 || rem == 3) {\n";
    s << "        int a = lidlB64Idx(s64[i]), b = lidlB64Idx(s64[i+1]);\n";
    s << "        if (a < 0 || b < 0) return {};\n";
    s << "        uint32_t n = (uint32_t(a) << 18) | (uint32_t(b) << 12);\n";
    s << "        out.push_back((n >> 16) & 0xff);\n";
    s << "        if (rem == 3) {\n";
    s << "            int c2 = lidlB64Idx(s64[i+2]);\n";
    s << "            if (c2 < 0) return {};\n";
    s << "            n |= uint32_t(c2) << 6;\n";
    s << "            out.push_back((n >> 8) & 0xff);\n";
    s << "        }\n    }\n    return out;\n}\n\n";


    s << "nlohmann::json lidlResultToJson(const StdLogosResult& r)\n{\n";
    s << "    nlohmann::json obj;\n";
    s << "    obj[\"success\"] = r.success;\n";
    s << "    obj[\"value\"] = r.value;\n";
    s << "    obj[\"error\"] = r.error.empty() ? nlohmann::json() : nlohmann::json(r.error);\n";
    s << "    return obj;\n}\n\n";

    emitInterfaceJson(s, module);
    s << "} // namespace\n\n";

    // -- event wiring (install once, lazily) ---------------------------------
    s << "static void lidlEnsureEmitWiring()\n{\n";
    s << "    static std::once_flag once;\n";
    s << "    std::call_once(once, []() {\n";
    s << "        _logos_codegen_::maybeSetEmitEvent(lidlImpl(),\n";
    s << "            [](const std::string& name, void* args) {\n";
    s << "                // cdylib events sidecar marshals into nlohmann::json\n";
    s << "                const nlohmann::json* payload = static_cast<const nlohmann::json*>(args);\n";
    s << "                std::lock_guard<std::mutex> lock(g_emitMutex);\n";
    s << "                if (g_emitCb) {\n";
    s << "                    const std::string dumped = payload ? payload->dump() : \"[]\";\n";
    s << "                    g_emitCb(name.c_str(), dumped.c_str(), g_emitUd);\n";
    s << "                }\n";
    s << "            });\n";
    s << "    });\n}\n\n";

    // -- typed dependency surface (modules().<dep>...) -----------------------
    // Wire modules() INDEPENDENTLY of the persistence context. Each dependency
    // client bakes its target+origin at codegen time and creates its lp client
    // lazily on first call, so modules() needs nothing from the context. A
    // module with deps but no STORED context still must have it wired — gating
    // it on the context latch (as it used to be) left m_logosModulesPtr null and
    // segfaulted the first cross-module call when the daemon never delivered a
    // context. No-op for impls that don't derive LogosModuleContext. Fired once
    // from the FIRST lidlTryFireContext (i.e. the first dispatch / set_context /
    // set_emit_callback), before the context-gated early return below.
    s << "static void lidlEnsureModulesWired()\n{\n";
    s << "    static std::once_flag once;\n";
    s << "    std::call_once(once, []() {\n";
    s << "        _logos_codegen_::maybeSetLogosModules(lidlImpl(), new LogosModules());\n";
    s << "    });\n}\n\n";

    // The context ready-latch: stamp the context + fire onContextReady ONCE,
    // as soon as the module is fully wired (context stored AND the emit
    // callback delivered) — at module load, before publication. Hosts that
    // never wire an emit callback still get the hook before first dispatch
    // (requireEmit = false fallback).
    s << "static void lidlTryFireContext(bool requireEmit)\n{\n";
    s << "    lidlEnsureEmitWiring();\n";
    s << "    lidlEnsureModulesWired();\n";
    s << "    if (g_hookFired.load(std::memory_order_acquire)) return;\n";
    s << "    std::string path, id, persist;\n";
    s << "    {\n";
    s << "        std::lock_guard<std::mutex> lock(g_ctxMutex);\n";
    s << "        if (!g_ctxStored) return;\n";
    s << "        path = g_ctxPath; id = g_ctxId; persist = g_ctxPersist;\n";
    s << "    }\n";
    s << "    if (requireEmit) {\n";
    s << "        std::lock_guard<std::mutex> lock(g_emitMutex);\n";
    s << "        if (!g_emitCb) return;\n";
    s << "    }\n";
    s << "    g_hookFired.store(true, std::memory_order_release);\n";
    // modules() was already wired by lidlEnsureModulesWired() above (before this
    // context-gated early return), so onContextReady can safely call
    // modules().<dep>... / subscribe to dependency events from the hook.
    s << "    _logos_codegen_::maybeSetContext(lidlImpl(), path, id, persist);\n";
    s << "}\n\n";

    // -- exports -------------------------------------------------------------
    s << "extern \"C\" {\n\n";

    s << "char* logos_module_dispatch(const char* method, const char* args_json)\n{\n";
    s << "    if (!method) return nullptr;\n";
    s << "    lidlTryFireContext(false);\n";
    s << "    nlohmann::json args = nlohmann::json::array();\n";
    s << "    if (args_json && *args_json) {\n";
    s << "        args = nlohmann::json::parse(args_json, nullptr, false);\n";
    s << "        if (args.is_discarded() || !args.is_array()) return nullptr;\n";
    s << "    }\n";
    s << "    const std::string m(method);\n";
    s << "    try {\n";

    for (const MethodDecl& md : module.methods) {
        s << "        if (m == \"" << md.name << "\") {\n";
        s << "            if (args.size() < " << md.params.size() << ") return nullptr;\n";
        QString call = "lidlImpl()." + qs(md.name) + "(";
        for (int i = 0; i < md.params.size(); ++i) {
            call += jsonArgToStd(md.params[i].type,
                                 QString("args.at(%1)").arg(i),
                                 QString("arg%1").arg(i), recs);
            if (i + 1 < md.params.size()) call += ", ";
        }
        call += ")";
        // `void` parses as a Named type "void" from a .lidl (it isn't a
        // lidlBuiltinType); empty name is the header path's in-memory void.
        const bool voidReturn =
            md.returnType.name == "void"
            || (md.returnType.kind == TypeExpr::Primitive && md.returnType.name.empty())
            || lidlTypeToQt(md.returnType) == "void";
        if (voidReturn) {
            s << "            " << call << ";\n";
            s << "            return lidlStrdup(\"true\");\n";
        } else {
            s << "            auto result = " << call << ";\n";
            s << "            return lidlStrdup(" << stdReturnToJson(md, "result", recs) << ".dump());\n";
        }
        s << "        }\n";
    }

    s << "    } catch (const std::exception& e) {\n";
    s << "        nlohmann::json err{{\"code\", \"dispatch_failed\"}, {\"message\", e.what()},\n";
    s << "                           {\"origin\", \"" << module.name << "\"}};\n";
    s << "        return lidlStrdup(err.dump());\n";
    s << "    }\n";
    s << "    return nullptr;  // unknown method\n";
    s << "}\n\n";

    s << "char* logos_module_get_methods(void)\n{\n";
    s << "    return lidlStrdup(lidlInterfaceJson().dump());\n}\n\n";

    s << "void logos_module_set_context(const char* module_path,\n";
    s << "                              const char* instance_id,\n";
    s << "                              const char* instance_persistence_path)\n{\n";
    s << "    {\n";
    s << "        std::lock_guard<std::mutex> lock(g_ctxMutex);\n";
    s << "        g_ctxPath = module_path ? module_path : \"\";\n";
    s << "        g_ctxId = instance_id ? instance_id : \"\";\n";
    s << "        g_ctxPersist = instance_persistence_path ? instance_persistence_path : \"\";\n";
    s << "        g_ctxStored = true;\n";
    s << "    }\n";
    s << "    lidlTryFireContext(true);\n";
    s << "}\n\n";

    s << "void logos_module_set_emit_callback(logos_module_emit_cb cb, void* user_data)\n{\n";
    s << "    {\n";
    s << "        std::lock_guard<std::mutex> lock(g_emitMutex);\n";
    s << "        g_emitCb = cb;\n";
    s << "        g_emitUd = user_data;\n";
    s << "    }\n";
    s << "    lidlTryFireContext(true);\n";
    s << "}\n\n";

    s << "int logos_module_accept_token(const char* module_name, const char* token)\n{\n";
    s << "    if (!module_name || !token) return -1;\n";
    s << "    // Seed the protocol's shared TokenManager so this module's OUTBOUND\n";
    s << "    // lp_client (modules().<dep>...) can authenticate calls. In\n";
    s << "    // particular the capability_module bootstrap token the host\n";
    s << "    // delivers at load lets the automatic requestModule flow fetch a\n";
    s << "    // per-target token on the first cross-module call. lp_token_save\n";
    s << "    // writes the same TokenManager::instance() the lp_client reads.\n";
    s << "    return lp_token_save(module_name, token);\n}\n\n";

    s << "const char* logos_module_get_protocol_version(void)\n{\n";
    s << "    return LOGOS_PROTOCOL_VERSION_STRING;\n}\n\n";

    s << "void logos_module_string_free(char* str)\n{\n";
    s << "    std::free(str);\n}\n\n";

    s << "} // extern \"C\"\n";
    return c;
}

QString lidlMakeEventsSourceCdylib(const ModuleDecl& module,
                                   const QString& implClass,
                                   const QString& implHeader)
{
    QString c;
    QTextStream s(&c);
    s << "// AUTO-GENERATED by logos-cpp-generator --cdylib -- do not edit\n";
    s << "// Typed `logos_events:` bodies, cdylib flavor: marshal into\n";
    s << "// nlohmann::json and route through LogosModuleContext::emitEventImpl_\n";
    s << "// (the export wrapper forwards to the host's emit callback).\n";
    const std::set<std::string> recsEv = recordNames(module);
    s << "#include \"" << implHeader << "\"\n";
    s << "#include \"" << module.name << "_types.h\"\n";
    s << "#include <nlohmann/json.hpp>\n\n";
    s << "#include <cstdint>\n";
    s << "#include <map>\n";
    s << "#include <string>\n";
    s << "#include <vector>\n";
    // LogosMap / LogosList (nlohmann aliases) appear in the emitted signatures
    // whenever an event carries a map or an `any` payload.
    if (hasJsonEventParam(module))
        s << "#include <logos_json.h>\n";
    s << "\n";

    // Only the modules that actually emit binary event payloads need the bytes
    // encoder; emitting it everywhere would leave it unused (and warned about).
    if (hasBytesEventParam(module)) {
        s << "namespace {\n\n";
        emitBytesEncodeHelpers(s);
        s << "} // namespace\n\n";
    }

    for (const EventDecl& ed : module.events) {
        s << "void " << implClass << "::" << ed.name << "(";
        for (int i = 0; i < ed.params.size(); ++i) {
            const QString stdType = lidlTypeToStdCdylib(ed.params[i].type, recsEv);
            // Must match the author's declaration in the `logos_events:` block:
            // the non-scalar types are conventionally taken by const-ref there.
            // Records and std::map belong in that set too — they are structs and
            // containers, and emitting them BY VALUE makes the generated
            // definition not match the author's declaration, which is a compile
            // error naming a parameter type mismatch rather than anything
            // helpful.
            if (stdType == "std::string" || stdType.startsWith("std::vector")
                || stdType.startsWith("std::map")
                || isRecord(ed.params[i].type, recsEv)
                || stdType == "LogosMap" || stdType == "LogosList")
                s << "const " << stdType << "& " << ed.params[i].name;
            else
                s << stdType << " " << ed.params[i].name;
            if (i + 1 < ed.params.size()) s << ", ";
        }
        s << ")\n{\n";
        s << "    nlohmann::json args = nlohmann::json::array();\n";
        for (const ParamDecl& pd : ed.params) {
            const QString evStd = lidlTypeToStdCdylib(pd.type, recsEv);
            // A record or a composite carrying bytes rides the generated codec,
            // exactly like a method return — otherwise an event payload would be
            // the one place a bstr silently loses its tag.
            if (evStd != "LogosMap" && evStd != "LogosList"
                && (isRecord(pd.type, recsEv)
                    || pd.type.kind == TypeExpr::Array || pd.type.kind == TypeExpr::Map)) {
                s << "    args.push_back(logos::toJson<" << evStd << ">("
                  << pd.name << "));\n";
                continue;
            }
            if (pd.type.kind == TypeExpr::Primitive && pd.type.name == "bstr")
                s << "    args.push_back(lidlBytesToJson(" << pd.name << "));\n";
            else
                s << "    args.push_back(" << pd.name << ");\n";
        }
        s << "    emitEventImpl_(\"" << ed.name << "\", &args);\n";
        s << "}\n\n";
    }
    return c;
}
