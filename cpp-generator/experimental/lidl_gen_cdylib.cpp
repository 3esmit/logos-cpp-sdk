#include "lidl_gen_cdylib.h"
#include "lidl_emit_common.h"

#include <QSet>
#include <QTextStream>

QString lidlToPascalCase(const QString& name);
QString lidlTypeToQt(const TypeExpr& te);
bool lidlIsStdConvertible(const TypeExpr& te);

namespace {

// The cdylib-supported subset: std-convertible LIDL types only — the same
// Qt-free set the std apiStyle handled, so any universal module that built
// under std also builds as a header-first cdylib.
// Records declared by the module, so a Named type can be resolved. Set for the
// duration of lidlCdylibSupported / the emitters — the alternative is threading
// the module through typeSupported's several recursive call sites.
QSet<QString> g_declaredRecords;

bool typeSupported(const TypeExpr& te, bool isReturn)
{
    // A record the module declares is a first-class type: the generated codec
    // specialisation below encodes it, and because that plugs into
    // logos::detail::Codec, records compose inside [T] and {tstr: T} for free.
    if (te.kind == TypeExpr::Named && g_declaredRecords.contains(qs(te.name)))
        return true;
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
    // Composition is GENERIC and recursive: [T] and {tstr: T} are supported for
    // any supported T, at any depth. logos::toJson/fromJson (logos_codec.h) does
    // the matching recursion at runtime, so [[bstr]], {tstr: [bstr]} and bytes
    // nested inside a map all encode canonically without this function — or the
    // emitter — enumerating the combinations.
    if (te.kind == TypeExpr::Array && te.elements.size() == 1)
        return typeSupported(te.elements[0], /*isReturn=*/false);

    // Only string-keyed maps are representable, matching {tstr: T}.
    if (te.kind == TypeExpr::Map && te.elements.size() == 2) {
        const TypeExpr& k = te.elements[0];
        if (!(k.kind == TypeExpr::Primitive && k.name == "tstr"))
            return false;
        return typeSupported(te.elements[1], /*isReturn=*/false);
    }
    return false;
}

// The C++ spelling the parser could not map, for the rejection message. Empty
// for anything else.
QString unsupportedSpelling(const TypeExpr& te)
{
    if (te.kind == TypeExpr::Named)
        return qs(te.name);
    if (te.kind == TypeExpr::Array && te.elements.size() == 1)
        return unsupportedSpelling(te.elements[0]);
    if (te.kind == TypeExpr::Map && te.elements.size() == 2) {
        const QString v = unsupportedSpelling(te.elements[1]);
        return v.isEmpty() ? unsupportedSpelling(te.elements[0]) : v;
    }
    return QString();
}

// " (std::pair<...>)" when the parser could not map a spelling, else empty.
// The author needs the offending C++ type, not just the parameter name — and for
// a narrow numeric spelling, the fix, since that is the likeliest rejection and
// the answer is always the same: numbers in this contract are 64-bit.
QString spellingNote(const TypeExpr& te)
{
    const QString sp = unsupportedSpelling(te);
    if (sp.isEmpty())
        return QString();

    static const QSet<QString> kNarrowSigned = {
        "int", "signed", "signed int", "short", "short int", "signed short",
        "long", "long int", "long long", "long long int", "ssize_t", "ptrdiff_t",
        "int8_t", "int16_t", "int32_t", "intmax_t", "intptr_t",
    };
    static const QSet<QString> kNarrowUnsigned = {
        "unsigned", "unsigned int", "unsigned short", "unsigned long",
        "unsigned long long", "size_t", "uint8_t", "uint16_t", "uint32_t",
        "uintmax_t", "uintptr_t",
    };
    if (kNarrowSigned.contains(sp))
        return QString(" (%1 — numbers are 64-bit here: use int64_t)").arg(sp);
    if (kNarrowUnsigned.contains(sp))
        return QString(" (%1 — numbers are 64-bit here: use uint64_t; uint8_t is "
                       "only meaningful as std::vector<uint8_t>, i.e. bstr)").arg(sp);
    if (sp == "float" || sp == "long double")
        return QString(" (%1 — the only floating type is double)").arg(sp);
    return QString(" (%1)").arg(sp);
}

// Qt-free spelling of a LIDL type (defined below). Forward-declared so the
// method-param decoder can spell composite `any` containers as their nlohmann
// aliases instead of Qt containers in this Qt-free TU.
QString lidlTypeToStdCdylib(const TypeExpr& te);

// std-typed return variable -> json expression
QString stdReturnToJson(const MethodDecl& md, const QString& var)
{
    const TypeExpr& te = md.returnType;
    if (md.resultReturn) {
        // StdLogosResult -> the canonical {success, value, error} object
        // (same shape logos_json_convert emits for Qt LogosResult).
        return "lidlResultToJson(" + var + ")";
    }
    // No LogosMap/LogosList special case: logos::toJson of an nlohmann::json is
    // the identity, and inferring "already json" from the LIDL kind is wrong now
    // that a plain std::map<std::string, T> is also Map-kind — it produced
    // `result.dump()` on a std::map and failed to compile.
    // Everything else — scalars, bytes, arrays, maps, any nesting — goes through
    // the canonical encoder, which tags bytes wherever they occur.
    (void)te;
    return "logos::toJson(" + var + ")";
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
    g_declaredRecords.clear();
    for (const TypeDecl& t : module.types)
        g_declaredRecords.insert(qs(t.name));
    for (const MethodDecl& md : module.methods) {
        for (const ParamDecl& pd : md.params) {
            if (!typeSupported(pd.type, /*isReturn=*/false)) {
                if (error)
                    *error = QString("method '%1': parameter '%2' has a type outside the "
                                     "cdylib-supported (Qt-free) subset%3")
                                 .arg(qs(md.name), qs(pd.name), spellingNote(pd.type));
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
                                 "(Qt-free) subset%2")
                             .arg(qs(md.name), spellingNote(md.returnType));
            return false;
        }
    }
    for (const EventDecl& ed : module.events) {
        for (const ParamDecl& pd : ed.params) {
            if (!typeSupported(pd.type, /*isReturn=*/false)) {
                if (error)
                    *error = QString("event '%1': parameter '%2' has a type outside the "
                                     "cdylib-supported (Qt-free) subset%3")
                                 .arg(qs(ed.name), qs(pd.name), spellingNote(pd.type));
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
    // The canonical codec — one implementation of the LIDL <-> JSON mapping,
    // replacing the base64/tagged-bytes copy this file used to emit per module.
    s << "#include <logos_codec.h>\n";
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


    s << "nlohmann::json lidlResultToJson(const StdLogosResult& r)\n{\n";
    s << "    nlohmann::json obj;\n";
    s << "    obj[\"success\"] = r.success;\n";
    s << "    obj[\"value\"] = r.value;\n";
    s << "    obj[\"error\"] = r.error.empty() ? nlohmann::json() : nlohmann::json(r.error);\n";
    s << "    return obj;\n}\n\n";

    // One Codec specialisation per declared record. Fields are addressed through
    // decltype, so no C++ type name has to be spelled — the same trick JsonArg
    // uses — and because these plug into logos::detail::Codec, a record nested in
    // [T] or {tstr: T} is handled by the existing recursion with nothing further
    // emitted. A missing field decodes as null, which the leaf codec then rejects
    // with the field's path.
    if (!module.types.empty()) {
        s << "} // namespace\n\n";
        s << "namespace logos { namespace detail {\n\n";
        for (const TypeDecl& t : module.types) {
            const QString n = qs(t.name);
            s << "template <> struct Codec<" << n << ", void> {\n";
            s << "    static nlohmann::json to(const " << n << "& v)\n    {\n";
            s << "        nlohmann::json o = nlohmann::json::object();\n";
            for (const FieldDecl& f : t.fields) {
                const QString fn = qs(f.name);
                s << "        o[\"" << fn << "\"] = Codec<std::decay_t<decltype(v." << fn
                  << ")>>::to(v." << fn << ");\n";
            }
            s << "        return o;\n    }\n";
            s << "    static " << n << " from(const nlohmann::json& j, const std::string& path)\n    {\n";
            s << "        if (!j.is_object()) typeError(path, \"object\", j);\n";
            s << "        " << n << " out;\n";
            for (const FieldDecl& f : t.fields) {
                const QString fn = qs(f.name);
                s << "        out." << fn << " = Codec<std::decay_t<decltype(out." << fn
                  << ")>>::from(j.contains(\"" << fn << "\") ? j.at(\"" << fn
                  << "\") : nlohmann::json(), joinPath(path, \"." << fn << "\"));\n";
            }
            s << "        return out;\n    }\n};\n\n";
        }
        s << "}} // namespace logos::detail\n\n";
        s << "namespace {\n\n";
    }

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
        // A malformed call reports WHY. This used to return nullptr, which the Qt
        // glue turns into an empty QVariant — indistinguishable from a method
        // that legitimately returned nothing, so "you passed 2 of 4 arguments"
        // looked like a successful empty answer. dispatch_failed objects already
        // travel this path, so a structured reply here is nothing new for hosts.
        // Rust's generated dispatch emits the same code and message.
        s << "            if (args.size() < " << md.params.size() << ") {\n";
        s << "                nlohmann::json err{{\"code\", \"invalid_args\"},\n";
        s << "                    {\"message\", \"expected " << md.params.size()
          << " arguments, got \" + std::to_string(args.size())},\n";
        s << "                    {\"origin\", \"" << module.name << "\"}};\n";
        s << "                return lidlStrdup(err.dump());\n";
        s << "            }\n";
        QString call = "lidlImpl()." + qs(md.name) + "(";
        for (int i = 0; i < md.params.size(); ++i) {
            // logos::JsonArg converts itself into whatever the impl's parameter
            // type is, so the author's own spelling (uint32_t, a nested map, …)
            // is what gets decoded — no type name is emitted, and there is no
            // mapping table to keep in sync. logos_codec.h does the recursion.
            call += QString("logos::JsonArg{args.at(%1), \"arg%1\"}").arg(i);
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
    s << "#include <nlohmann/json.hpp>\n";
    s << "#include <logos_codec.h>\n\n";
    s << "#include <cstdint>\n";
    s << "#include <string>\n";
    s << "#include <vector>\n";
    // LogosMap / LogosList (nlohmann aliases) appear in the emitted signatures
    // whenever an event carries a map or an `any` payload.
    if (hasJsonEventParam(module))
        s << "#include <logos_json.h>\n";
    s << "\n";

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
            s << "    args.push_back(logos::toJson(" << pd.name << "));\n";
        }
        s << "    emitEventImpl_(\"" << ed.name << "\", &args);\n";
        s << "}\n\n";
    }
    return c;
}
