#include "lidl_gen_cdylib.h"
#include "lidl_emit_common.h"

#include <QTextStream>

QString lidlToPascalCase(const QString& name);
QString lidlTypeToQt(const TypeExpr& te);
bool lidlIsStdConvertible(const TypeExpr& te);

namespace {

// The cdylib-supported subset: std-convertible LIDL types only — the same
// Qt-free set the std apiStyle handled, so any universal module that built
// under std also builds as a header-first cdylib.
bool typeSupported(const TypeExpr& te, bool isReturn)
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
    if (te.kind == TypeExpr::Array && te.elements.size() == 1) {
        const TypeExpr& e = te.elements[0];
        // `bstr` elements are the one array kind that cannot ride nlohmann's
        // blanket get<>(): each element is the tagged {"_bytes": base64url}
        // OBJECT, not a number array, so it needs the emitted per-element
        // codec (lidlBytesListFromJson / lidlBytesListToJson) below.
        return e.kind == TypeExpr::Primitive
            && (e.name == "tstr" || e.name == "int" || e.name == "uint"
                || e.name == "float64" || e.name == "bool" || e.name == "any"
                || e.name == "bstr");
    }
    // Maps ({k: v}, i.e. LogosMap) round-trip through nlohmann too.
    if (te.kind == TypeExpr::Map)
        return true;
    return false;
}

// `[bstr]` — an array of byte strings (std::vector<std::vector<uint8_t>>).
// Every place scalar `bstr` needs the tagged-bytes codec, this needs the list
// form of it, so the check lives in one place.
bool isBytesArray(const TypeExpr& te)
{
    return te.kind == TypeExpr::Array && te.elements.size() == 1
        && te.elements[0].kind == TypeExpr::Primitive
        && te.elements[0].name == "bstr";
}

// True when the module mentions `[bstr]` anywhere the emitted TU has to encode
// or decode it — method params, method returns, or event payloads. Gates the
// list codec so modules that never use it don't carry an unused static function
// (the same reason hasBytesEventParam() gates the scalar encoder).
bool usesBytesArray(const ModuleDecl& module)
{
    for (const MethodDecl& md : module.methods) {
        if (isBytesArray(md.returnType))
            return true;
        for (const ParamDecl& pd : md.params)
            if (isBytesArray(pd.type))
                return true;
    }
    for (const EventDecl& ed : module.events)
        for (const ParamDecl& pd : ed.params)
            if (isBytesArray(pd.type))
                return true;
    return false;
}

// Qt-free spelling of a LIDL type (defined below). Forward-declared so the
// method-param decoder can spell composite `any` containers as their nlohmann
// aliases instead of Qt containers in this Qt-free TU.
QString lidlTypeToStdCdylib(const TypeExpr& te);

// json arg expression -> std-typed C++ expression
QString jsonArgToStd(const TypeExpr& te, const QString& expr)
{
    if (te.kind == TypeExpr::Primitive) {
        if (te.name == "tstr")    return expr + ".get<std::string>()";
        if (te.name == "bstr")    return "lidlBytesFromJson(" + expr + ")";
        if (te.name == "int")     return expr + ".get<int64_t>()";
        if (te.name == "uint")    return expr + ".get<uint64_t>()";
        if (te.name == "float64") return expr + ".get<double>()";
        if (te.name == "bool")    return expr + ".get<bool>()";
    }
    if (te.kind == TypeExpr::Array && te.elements.size() == 1) {
        // `[bstr]` cannot go through get<std::vector<std::vector<uint8_t>>>():
        // its elements arrive as tagged {"_bytes": …} objects, which nlohmann
        // would refuse (and a raw number-array element would silently bypass
        // the base64 decode). Route it through the per-element codec instead.
        if (isBytesArray(te))
            return "lidlBytesListFromJson(" + expr + ")";
        // Qt-free spelling: `[any]` must decode as LogosList, NOT QVariantList
        // (lidlTypeToStd's Qt fallback), which is undeclared in this TU. Typed
        // scalar arrays ([tstr]/[int]/…) are unaffected — Cdylib defers to the
        // same std spelling for them.
        const QString inner = lidlTypeToStdCdylib(te);
        return expr + ".get<" + inner + ">()";
    }
    return expr;
}

// std-typed return variable -> json expression
QString stdReturnToJson(const MethodDecl& md, const QString& var)
{
    const TypeExpr& te = md.returnType;
    if (md.resultReturn) {
        // StdLogosResult -> the canonical {success, value, error} object
        // (same shape logos_json_convert emits for Qt LogosResult).
        return "lidlResultToJson(" + var + ")";
    }
    if (md.jsonReturn) {
        return var;  // LogosMap / LogosList are nlohmann::json already
    }
    if (te.kind == TypeExpr::Primitive) {
        if (te.name == "bstr") return "lidlBytesToJson(" + var + ")";
        return "nlohmann::json(" + var + ")";
    }
    // `[bstr]`: nlohmann::json(std::vector<std::vector<uint8_t>>) would emit
    // nested number arrays, which no consumer decodes as bytes. Tag each element.
    if (isBytesArray(te))
        return "lidlBytesListToJson(" + var + ")";
    return "nlohmann::json(" + var + ")";
}

// Qt-free spelling of a LIDL type. lidlTypeToStd() falls back to Qt containers
// (QVariant / QVariantMap / QVariantList) for the composite types, but a cdylib
// TU is Qt-free by definition and typeSupported() admits `any` and maps — so
// spell those as their nlohmann aliases (LogosMap / LogosList) instead. Without
// this the events sidecar emits a bare `QVariant` parameter and does not
// compile.
QString lidlTypeToStdCdylib(const TypeExpr& te)
{
    if (te.kind == TypeExpr::Primitive && te.name == "any")
        return "LogosMap";
    if (te.kind == TypeExpr::Map)
        return "LogosMap";
    if (te.kind == TypeExpr::Array && te.elements.size() == 1
        && te.elements[0].kind == TypeExpr::Primitive
        && te.elements[0].name == "any")
        return "LogosList";
    return lidlTypeToStd(te);
}

// True when the module declares at least one `bstr` event parameter — the only
// reason the events sidecar needs the bytes encoder. Emitting it unconditionally
// leaves an unused static function (a -Wunused-function warning) in every module
// whose events carry no binary data.
bool hasBytesEventParam(const ModuleDecl& module)
{
    for (const EventDecl& ed : module.events)
        for (const ParamDecl& pd : ed.params)
            if ((pd.type.kind == TypeExpr::Primitive && pd.type.name == "bstr")
                || isBytesArray(pd.type))
                return true;
    return false;
}

// True when an EVENT carries `[bstr]`, so the sidecar needs the list encoder on
// top of the scalar one. Method params/returns are handled in the impl TU.
bool hasBytesArrayEventParam(const ModuleDecl& module)
{
    for (const EventDecl& ed : module.events)
        for (const ParamDecl& pd : ed.params)
            if (isBytesArray(pd.type))
                return true;
    return false;
}

// True when any event parameter is spelled LogosMap / LogosList, so the sidecar
// needs <logos_json.h> for those aliases.
bool hasJsonEventParam(const ModuleDecl& module)
{
    for (const EventDecl& ed : module.events)
        for (const ParamDecl& pd : ed.params) {
            const QString t = lidlTypeToStdCdylib(pd.type);
            if (t == "LogosMap" || t == "LogosList")
                return true;
        }
    return false;
}

// `withList` additionally emits lidlBytesListToJson for `[bstr]`. Off by
// default so a module that never carries a byte-string array does not gain an
// unused static function.
void emitBytesEncodeHelpers(QTextStream& s, bool withList = false)
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

    if (withList) {
        s << "nlohmann::json lidlBytesListToJson(const std::vector<std::vector<uint8_t>>& list)\n{\n";
        s << "    nlohmann::json out = nlohmann::json::array();\n";
        s << "    for (const std::vector<uint8_t>& bytes : list)\n";
        s << "        out.push_back(lidlBytesToJson(bytes));\n";
        s << "    return out;\n}\n\n";
    }
}

void emitInterfaceJson(QTextStream& s, const ModuleDecl& module)
{
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
            sig += lidlTypeToQt(md.params[i].type);
            if (i + 1 < md.params.size()) sig += ",";
        }
        sig += ")";
        s << "        obj[\"signature\"] = \"" << sig << "\";\n";
        s << "        obj[\"returnType\"] = \"" << lidlTypeToQt(md.returnType) << "\";\n";
        s << "        obj[\"isInvokable\"] = true;\n";
        if (!md.params.empty()) {
            s << "        nlohmann::json params = nlohmann::json::array();\n";
            for (const ParamDecl& pd : md.params) {
                s << "        params.push_back({{\"type\", \"" << lidlTypeToQt(pd.type)
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
            sig += lidlTypeToQt(ed.params[i].type);
            if (i + 1 < ed.params.size()) sig += ",";
        }
        sig += ")";
        s << "        obj[\"signature\"] = \"" << sig << "\";\n";
        if (!ed.params.empty()) {
            s << "        nlohmann::json params = nlohmann::json::array();\n";
            for (const ParamDecl& pd : ed.params) {
                s << "        params.push_back({{\"type\", \"" << lidlTypeToQt(pd.type)
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
    for (const MethodDecl& md : module.methods) {
        for (const ParamDecl& pd : md.params) {
            if (!typeSupported(pd.type, /*isReturn=*/false)) {
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
            && !typeSupported(md.returnType, /*isReturn=*/true)) {
            if (error)
                *error = QString("method '%1': return type outside the cdylib-supported "
                                 "(Qt-free) subset").arg(qs(md.name));
            return false;
        }
    }
    for (const EventDecl& ed : module.events) {
        for (const ParamDecl& pd : ed.params) {
            if (!typeSupported(pd.type, /*isReturn=*/false)) {
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

QString lidlMakeModuleImplExports(const ModuleDecl& module,
                                  const QString& implClass,
                                  const QString& implHeader)
{
    QString c;
    QTextStream s(&c);

    s << "// AUTO-GENERATED by logos-cpp-generator --cdylib -- do not edit\n";
    s << "//\n";
    s << "// The common module-impl C ABI exports (logos_module_impl.h) around the\n";
    s << "// universal impl class `" << implClass << "`. Qt-FREE: compiled into the\n";
    s << "// module's cdylib; the uniform Qt-plugin glue (or a future no-Qt host)\n";
    s << "// drives it exclusively through these symbols.\n";
    s << "#include \"" << implHeader << "\"\n";
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

    const bool bytesList = usesBytesArray(module);
    emitBytesEncodeHelpers(s, bytesList);

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

    if (bytesList) {
        s << "std::vector<std::vector<uint8_t>> lidlBytesListFromJson(const nlohmann::json& j)\n{\n";
        s << "    std::vector<std::vector<uint8_t>> out;\n";
        s << "    // Each element runs through the lenient scalar decode above, so a\n";
        s << "    // caller may send tagged {\"_bytes\": base64url} objects, plain\n";
        s << "    // strings or number arrays — element by element. A non-array arg\n";
        s << "    // yields an empty list rather than throwing, matching the scalar\n";
        s << "    // decoder's behaviour on an unexpected shape.\n";
        s << "    if (!j.is_array()) return out;\n";
        s << "    out.reserve(j.size());\n";
        s << "    for (const auto& e : j)\n";
        s << "        out.push_back(lidlBytesFromJson(e));\n";
        s << "    return out;\n}\n\n";
    }

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
                                 QString("args.at(%1)").arg(i));
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
            s << "            return lidlStrdup(" << stdReturnToJson(md, "result") << ".dump());\n";
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
    s << "#include \"" << implHeader << "\"\n";
    s << "#include <nlohmann/json.hpp>\n\n";
    s << "#include <cstdint>\n";
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
        emitBytesEncodeHelpers(s, hasBytesArrayEventParam(module));
        s << "} // namespace\n\n";
    }

    for (const EventDecl& ed : module.events) {
        s << "void " << implClass << "::" << ed.name << "(";
        for (int i = 0; i < ed.params.size(); ++i) {
            const QString stdType = lidlTypeToStdCdylib(ed.params[i].type);
            // Must match the author's declaration in the `logos_events:` block:
            // the non-scalar types are conventionally taken by const-ref there.
            if (stdType == "std::string" || stdType.startsWith("std::vector")
                || stdType == "LogosMap" || stdType == "LogosList")
                s << "const " << stdType << "& " << ed.params[i].name;
            else
                s << stdType << " " << ed.params[i].name;
            if (i + 1 < ed.params.size()) s << ", ";
        }
        s << ")\n{\n";
        s << "    nlohmann::json args = nlohmann::json::array();\n";
        for (const ParamDecl& pd : ed.params) {
            if (pd.type.kind == TypeExpr::Primitive && pd.type.name == "bstr")
                s << "    args.push_back(lidlBytesToJson(" << pd.name << "));\n";
            else if (isBytesArray(pd.type))
                s << "    args.push_back(lidlBytesListToJson(" << pd.name << "));\n";
            else
                s << "    args.push_back(" << pd.name << ");\n";
        }
        s << "    emitEventImpl_(\"" << ed.name << "\", &args);\n";
        s << "}\n\n";
    }
    return c;
}
