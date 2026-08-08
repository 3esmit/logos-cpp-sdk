#include "lidl_gen_client.h"
#include "lidl_emit_common.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------



static bool isRefType(const QString& qt)
{
    if (qt == "QString" || qt == "QStringList" || qt == "QJsonArray"
        || qt == "QVariantList" || qt == "QVariantMap" || qt == "QByteArray")
        return true;
    // A record is a struct: pass it by const& too. Anything that is not a known
    // Qt scalar/handle spelling is a generated record type.
    return !(qt == "bool" || qt == "int" || qt == "double" || qt == "float"
             || qt == "void" || qt == "qlonglong" || qt == "qulonglong"
             || qt == "QVariant" || qt == "LogosResult");
}

static void emitParam(QTextStream& s, const QString& qtType, const std::string& name)
{
    if (isRefType(qtType))
        s << "const " << qtType << "& " << name;
    else
        s << qtType << " " << name;
}

static bool lidlIsRecord(const TypeExpr& te);
static QString qtToVariantExpr(const TypeExpr& te, const QString& expr);
static QString qtFromVariantExpr(const TypeExpr& te, const QString& expr);
static QString returnConversionFor(const TypeExpr& te, const QString& qt);

static QString returnConversion(const QString& qt)
{
    if (qt == "bool")        return "return _result.toBool();";
    // 64-bit, matching lidlTypeToQt: toInt() truncated a LIDL int/uint, and for
    // uint it also read the value as signed.
    if (qt == "qlonglong")   return "return _result.toLongLong();";
    if (qt == "qulonglong")  return "return _result.toULongLong();";
    if (qt == "double")      return "return _result.toDouble();";
    if (qt == "float")       return "return _result.toFloat();";
    if (qt == "QString")     return "return _result.toString();";
    if (qt == "QStringList") return "return _result.toStringList();";
    if (qt == "QJsonArray")  return "return qvariant_cast<QJsonArray>(_result);";
    if (qt == "QVariantList") return "return _result.toList();";
    if (qt == "QVariantMap") return "return _result.toMap();";
    if (qt == "LogosResult") return "return _result.value<LogosResult>();";
    return "return _result;";
}

// Records (and containers holding them) decode through the generated
// conversions; everything else keeps the historical QVariant accessor.
static QString returnConversionFor(const TypeExpr& te, const QString& qt)
{
    const bool holdsRecord =
        lidlIsRecord(te)
        || (te.kind == TypeExpr::Array && te.elements.size() == 1 && lidlIsRecord(te.elements[0]))
        || (te.kind == TypeExpr::Map && te.elements.size() == 2 && lidlIsRecord(te.elements[1]));
    if (holdsRecord)
        return "return " + qtFromVariantExpr(te, "_result") + ";";
    return returnConversion(qt);
}

// The async twin of returnConversionFor: `v` is the wire QVariant.
//
// A record-bearing return MUST decode field by field here too. The wire carries
// a QVariantMap and no Q_DECLARE_METATYPE is emitted for the struct, so
// `qvariant_cast<Status>(v)` does not fail — it silently returns a
// DEFAULT-CONSTRUCTED Status, and the caller sees empty fields with no
// diagnostic. That is the worst failure mode available: the sync path is
// correct, so the same call is right or wrong depending only on which overload
// the caller reached for.
static QString asyncReturnConversionFor(const TypeExpr& te, const QString& qt)
{
    const bool holdsRecord =
        lidlIsRecord(te)
        || (te.kind == TypeExpr::Array && te.elements.size() == 1 && lidlIsRecord(te.elements[0]))
        || (te.kind == TypeExpr::Map && te.elements.size() == 2 && lidlIsRecord(te.elements[1]));
    if (holdsRecord)
        return qtFromVariantExpr(te, "v");
    return "qvariant_cast<" + qt + ">(v)";
}

static QString asyncDefaultVal(const QString& qt)
{
    if (qt == "bool")        return "false";
    if (qt == "int" || qt == "double" || qt == "float") return "0";
    if (qt == "QString")     return "QString()";
    if (qt == "QStringList") return "QStringList()";
    if (qt == "QJsonArray")  return "QJsonArray()";
    if (qt == "QVariantList") return "QVariantList()";
    if (qt == "QVariantMap") return "QVariantMap()";
    return qt + "{}";
}


// ---------------------------------------------------------------------------
// Records
//
// A `type Foo { … }` in the contract becomes a real C++ struct plus two inline
// conversions, so a Qt consumer says `Status s = client.makeStatus();` instead
// of digging fields out of a QVariantMap. One LIDL type, one type per language.
//
// bstr fields are QByteArray on purpose: logos-protocol's QVariant<->JSON
// conversion already materialises the canonical {"_bytes": base64url} form as a
// QByteArray and back (logos_json_convert.cpp), so the record conversions stay
// pure field mapping and binary survives at any depth for free.
// ---------------------------------------------------------------------------

static bool lidlIsRecord(const TypeExpr& te)
{
    return te.kind == TypeExpr::Named && !te.name.empty();
}

// value expression of the Qt type -> QVariant
static QString qtToVariantExpr(const TypeExpr& te, const QString& expr)
{
    if (lidlIsRecord(te))
        return qs(te.name) + "ToVariant(" + expr + ")";
    if (te.kind == TypeExpr::Array && te.elements.size() == 1
        && (lidlIsRecord(te.elements[0]) || te.elements[0].kind != TypeExpr::Primitive)) {
        return "[&]{ QVariantList __l; for (const auto& __e : " + expr + ") __l.append("
             + qtToVariantExpr(te.elements[0], "__e") + "); return QVariant(__l); }()";
    }
    if (te.kind == TypeExpr::Map && te.elements.size() == 2
        && (lidlIsRecord(te.elements[1]) || te.elements[1].kind != TypeExpr::Primitive)) {
        return "[&]{ QVariantMap __m; for (auto __it = " + expr + ".begin(); __it != " + expr
             + ".end(); ++__it) __m.insert(__it.key(), "
             + qtToVariantExpr(te.elements[1], "__it.value()") + "); return QVariant(__m); }()";
    }
    return "QVariant::fromValue(" + expr + ")";
}

// QVariant expression -> value of the Qt type
static QString qtFromVariantExpr(const TypeExpr& te, const QString& expr)
{
    if (lidlIsRecord(te))
        return qs(te.name) + "FromVariant(" + expr + ")";
    if (te.kind == TypeExpr::Primitive) {
        const QString n = qs(te.name);
        if (n == "tstr")    return expr + ".toString()";
        if (n == "bstr")    return expr + ".toByteArray()";
        if (n == "int")     return expr + ".toLongLong()";
        if (n == "uint")    return expr + ".toULongLong()";
        if (n == "float64") return expr + ".toDouble()";
        if (n == "bool")    return expr + ".toBool()";
    }
    if (te.kind == TypeExpr::Array && te.elements.size() == 1) {
        const TypeExpr& e = te.elements[0];
        return "[&]{ " + lidlTypeToQt(te) + " __acc; for (const QVariant& __e : " + expr
             + ".toList()) __acc.append(" + qtFromVariantExpr(e, "__e") + "); return __acc; }()";
    }
    if (te.kind == TypeExpr::Map && te.elements.size() == 2) {
        const TypeExpr& v = te.elements[1];
        return "[&]{ " + lidlTypeToQt(te) + " __acc; const QVariantMap __mm = " + expr
             + ".toMap(); for (auto __it = __mm.begin(); __it != __mm.end(); ++__it) __acc.insert("
             + "__it.key(), " + qtFromVariantExpr(v, "__it.value()") + "); return __acc; }()";
    }
    return expr;
}

// A method argument as passed to packVariantList: records convert, everything
// else goes through unchanged (packVariantList wraps with QVariant::fromValue).
static QString qtArgExpr(const TypeExpr& te, const QString& name)
{
    const bool holdsRecord =
        lidlIsRecord(te)
        || (te.kind == TypeExpr::Array && te.elements.size() == 1 && lidlIsRecord(te.elements[0]))
        || (te.kind == TypeExpr::Map && te.elements.size() == 2 && lidlIsRecord(te.elements[1]));
    return holdsRecord ? qtToVariantExpr(te, name) : name;
}

// A record field's Qt type, honouring BOTH optionality spellings.
//
// `?T` is QVariant on the Qt surface — Qt has no optional template, and an
// invalid QVariant is its single empty inhabitant. The point of routing through
// fieldIsOptional() is that `? name: T` and `name: ?T` are the same declaration:
// reading `f.type` alone made the flag spelling emit a bare `T` (which cannot be
// empty at all) while the type spelling emitted QVariant, from one contract.
static QString lidlFieldTypeQt(const FieldDecl& f)
{
    return fieldIsOptional(f) ? QString("QVariant") : lidlTypeToQt(f.type);
}

static void emitRecords(QTextStream& s, const ModuleDecl& module)
{
    if (module.types.empty()) return;
    for (const TypeDecl& t : module.types) {
        const QString n = qs(t.name);
        s << "/// `" << n << "` — a record declared by the `" << qs(module.name) << "` contract.\n";
        s << "struct " << n << " {\n";
        for (const FieldDecl& f : t.fields)
            s << "    " << lidlFieldTypeQt(f) << " " << qs(f.name) << "{};\n";
        s << "};\n\n";
    }
    // Conversions come after ALL structs so records may reference each other.
    for (const TypeDecl& t : module.types) {
        const QString n = qs(t.name);
        s << "inline QVariant " << n << "ToVariant(const " << n << "& v)\n{\n";
        s << "    QVariantMap __m;\n";
        for (const FieldDecl& f : t.fields) {
            if (fieldIsOptional(f)) {
                // A record field is a NAMED slot: empty is spelled by OMITTING
                // the key, not by inserting an invalid QVariant. Same rule the
                // cdylib record codec follows, on the other surface.
                s << "    if (v." << qs(f.name) << ".isValid())\n";
                s << "        __m.insert(\"" << qs(f.name) << "\", v." << qs(f.name) << ");\n";
                continue;
            }
            s << "    __m.insert(\"" << qs(f.name) << "\", "
              << qtToVariantExpr(f.type, "v." + qs(f.name)) << ");\n";
        }
        s << "    return QVariant(__m);\n}\n\n";

        s << "inline " << n << " " << n << "FromVariant(const QVariant& value)\n{\n";
        s << "    const QVariantMap __m = value.toMap();\n";
        s << "    " << n << " __out;\n";
        for (const FieldDecl& f : t.fields) {
            if (fieldIsOptional(f)) {
                // Absent and null both arrive as an invalid QVariant — the same
                // state, as the contract requires. Converting (`.toString()` on
                // a flag-optional `tstr`) would have turned "empty" into "",
                // which is a VALUE.
                s << "    __out." << qs(f.name) << " = __m.value(\"" << qs(f.name) << "\");\n";
                continue;
            }
            s << "    __out." << qs(f.name) << " = "
              << qtFromVariantExpr(f.type, "__m.value(\"" + qs(f.name) + "\")") << ";\n";
        }
        s << "    return __out;\n}\n\n";
    }
}

// ---------------------------------------------------------------------------
// Header generation
// ---------------------------------------------------------------------------

QString lidlMakeHeader(const ModuleDecl& module, BindMode bindMode)
{
    QString className = lidlToPascalCase(qs(module.name));
    QString h;
    QTextStream s(&h);

    s << "#pragma once\n";
    s << "#include <QString>\n";
    s << "#include <QVariant>\n";
    s << "#include <QStringList>\n";
    s << "#include <QJsonArray>\n";
    s << "#include <QVariantList>\n";
    s << "#include <QVariantMap>\n";
    s << "#include <functional>\n";
    s << "#include <utility>\n";
    s << "#include \"logos_types.h\"\n";
    s << "#include \"logos_api.h\"\n";
    s << "#include \"logos_api_client.h\"\n";
    s << "#include \"logos_call_error.h\"\n";
    s << "#include \"logos_async_result.h\"\n";
    s << "#include \"logos_object.h\"\n\n";

    emitRecords(s, module);

    s << "class " << className << " {\n";
    s << "public:\n";
    if (bindMode == BindMode::Bound)
        s << "    explicit " << className << "(LogosAPI* api, const QString& moduleName);\n\n";
    else
        s << "    explicit " << className << "(LogosAPI* api);\n\n";

    s << "    using RawEventCallback = std::function<void(const QString&, const QVariantList&)>;\n";
    s << "    using EventCallback = std::function<void(const QVariantList&)>;\n\n";
    s << "    bool on(const QString& eventName, RawEventCallback callback);\n";
    s << "    bool on(const QString& eventName, EventCallback callback);\n";

    for (const MethodDecl& md : module.methods) {
        QString ret = lidlTypeToQt(md.returnType);
        s << "    " << ret << " " << md.name << "(";
        for (int i = 0; i < md.params.size(); ++i) {
            emitParam(s, lidlTypeToQt(md.params[i].type), md.params[i].name);
            if (i + 1 < md.params.size()) s << ", ";
        }
        // Optional error out-channel: pass a logos::CallError* to distinguish
        // a failed remote call from a legitimately default-valued result —
        // followed by an optional Timeout. Both trailing and defaulted, so
        // existing call sites (including ones passing `&err` positionally)
        // compile unchanged. Mirrors the legacy emitter in
        // legacy/generator_lib.cpp; the two must agree, since a consumer can
        // reach either (this one from a published `.lidl`, that one through the
        // module builder) for the same contract.
        if (!md.params.empty()) s << ", ";
        s << "logos::CallError* err = nullptr, Timeout timeout = Timeout());\n";

        auto emitAsyncParams = [&]() {
            for (int i = 0; i < md.params.size(); ++i) {
                emitParam(s, lidlTypeToQt(md.params[i].type), md.params[i].name);
                if (i + 1 < md.params.size()) s << ", ";
            }
            if (!md.params.empty()) s << ", ";
        };

        QString asyncCb = (ret == "void")
            ? QString("std::function<void()>")
            : QString("std::function<void(") + ret + ")>";
        s << "    void " << md.name << "Async(";
        emitAsyncParams();
        s << asyncCb << " callback, Timeout timeout = Timeout());\n";

        // Result-carrying async entry point. Distinct name, not an overload:
        // std::function<void(AsyncResult<T>)> alongside std::function<void(T)>
        // is ambiguous for a generic lambda.
        s << "    void " << md.name << "AsyncResult(";
        emitAsyncParams();
        s << "std::function<void(logos::AsyncResult<" << ret << ">)> callback"
          << ", Timeout timeout = Timeout());\n";
    }

    s << "\nprivate:\n";
    s << "    LogosObject* ensureReplica();\n";
    s << "    template<typename... Args>\n";
    s << "    static QVariantList packVariantList(Args&&... args) {\n";
    s << "        QVariantList list;\n";
    s << "        list.reserve(sizeof...(Args));\n";
    s << "        using Expander = int[];\n";
    s << "        (void)Expander{0, (list.append(QVariant::fromValue(std::forward<Args>(args))), 0)...};\n";
    s << "        return list;\n";
    s << "    }\n";
    s << "    LogosAPI* m_api;\n";
    s << "    LogosAPIClient* m_client;\n";
    s << "    QString m_moduleName;\n";
    s << "    LogosObject* m_eventReplica = nullptr;\n";
    s << "};\n";

    return h;
}

// ---------------------------------------------------------------------------
// Source generation
// ---------------------------------------------------------------------------

QString lidlMakeSource(const ModuleDecl& module, BindMode bindMode)
{
    QString className = lidlToPascalCase(qs(module.name));
    QString headerRel = qs(module.name) + "_api.h";
    QString c;
    QTextStream s(&c);

    s << "#include \"" << headerRel << "\"\n\n";
    s << "#include <QDebug>\n\n";

    // Provider validation failures are returned as a canonical QVariant map,
    // not as a transport CallError. Keep the result-carrying async surface from
    // turning that rejection into a successful default value.
    if (!module.methods.empty()) {
        s << "#include <QJsonObject>\n\n";
        s << "#ifndef LOGOS_GENERATED_LIDL_DISPATCH_REJECTION\n"
             "#define LOGOS_GENERATED_LIDL_DISPATCH_REJECTION\n\n"
             "namespace {\n"
             "bool logosLidlDispatchRejection(const QVariant& value, logos::CallError& error)\n"
             "{\n"
             "    QVariantMap map;\n"
             "    switch (value.userType()) {\n"
             "    case QMetaType::QVariantMap: map = value.toMap(); break;\n"
             "    case QMetaType::QJsonObject: map = value.toJsonObject().toVariantMap(); break;\n"
             "    default: return false;\n"
             "    }\n"
             "    if (map.size() != 3) return false;\n"
             "    const QVariant code = map.value(QStringLiteral(\"code\"));\n"
             "    const QVariant message = map.value(QStringLiteral(\"message\"));\n"
             "    const QVariant origin = map.value(QStringLiteral(\"origin\"));\n"
             "    if (code.userType() != QMetaType::QString\n"
             "        || message.userType() != QMetaType::QString\n"
             "        || origin.userType() != QMetaType::QString) return false;\n"
             "    if (code.toString() != QStringLiteral(\"dispatch_failed\")) return false;\n"
             "    error.code = code.toString().toStdString();\n"
             "    error.message = message.toString().toStdString();\n"
             "    error.origin = origin.toString().toStdString();\n"
             "    return true;\n"
             "}\n"
             "} // namespace\n"
             "#endif  // LOGOS_GENERATED_LIDL_DISPATCH_REJECTION\n\n";
    }

    // Target expression for every remote call: a baked literal in Static
    // mode, the runtime m_moduleName member in Bound (interface) mode.
    const QString targetExpr = (bindMode == BindMode::Bound)
        ? QStringLiteral("m_moduleName")
        : (QStringLiteral("\"") + qs(module.name) + QStringLiteral("\""));
    if (bindMode == BindMode::Bound)
        s << className << "::" << className << "(LogosAPI* api, const QString& moduleName) : m_api(api), m_client(api->getClient(moduleName)), m_moduleName(moduleName) {}\n\n";
    else
        s << className << "::" << className << "(LogosAPI* api) : m_api(api), m_client(api->getClient(\""
          << module.name << "\")), m_moduleName(QStringLiteral(\"" << module.name << "\")) {}\n\n";

    s << "LogosObject* " << className << "::ensureReplica() {\n";
    s << "    if (!m_eventReplica) {\n";
    s << "        LogosObject* replica = m_client->requestObject(m_moduleName);\n";
    s << "        if (!replica) {\n";
    s << "            qWarning() << \"" << className << ": failed to acquire remote object for events on\" << m_moduleName;\n";
    s << "            return nullptr;\n";
    s << "        }\n";
    s << "        m_eventReplica = replica;\n";
    s << "    }\n";
    s << "    return m_eventReplica;\n";
    s << "}\n\n";

    s << "bool " << className << "::on(const QString& eventName, RawEventCallback callback) {\n";
    s << "    if (!callback) { qWarning() << \"" << className << ": ignoring empty event callback for\" << eventName; return false; }\n";
    s << "    LogosObject* origin = ensureReplica();\n";
    s << "    if (!origin) return false;\n";
    s << "    m_client->onEvent(origin, eventName, callback);\n";
    s << "    return true;\n";
    s << "}\n\n";

    s << "bool " << className << "::on(const QString& eventName, EventCallback callback) {\n";
    s << "    if (!callback) { qWarning() << \"" << className << ": ignoring empty event callback for\" << eventName; return false; }\n";
    s << "    return on(eventName, [callback](const QString&, const QVariantList& data) { callback(data); });\n";
    s << "}\n\n";



    for (const MethodDecl& md : module.methods) {
        QString ret = lidlTypeToQt(md.returnType);
        int nParams = md.params.size();

        s << ret << " " << className << "::" << md.name << "(";
        for (int i = 0; i < nParams; ++i) {
            emitParam(s, lidlTypeToQt(md.params[i].type), md.params[i].name);
            if (i + 1 < nParams) s << ", ";
        }
        if (nParams > 0) s << ", ";
        s << "logos::CallError* err, Timeout timeout) {\n";

        // Call through the err-out overload: with a logos::CallError* the
        // caller can distinguish a failed remote call from a legitimately
        // default-valued result; without it the historical default-on-failure
        // behavior is kept, plus a warning in the module log.
        s << "    logos::CallError _err;\n";
        if (ret != "void") s << "    QVariant _result = ";
        else                s << "    ";

        // Pack each argument as ONE element via packVariantList (which wraps
        // with QVariant::fromValue). A braced `QVariantList{v}` or `<< v` would
        // CONCATENATE a QVariantList-typed arg (any `[T]` list) into the args
        // list, sending a 3-element [1,2,3] as three positional args instead of
        // one — the historical "typed arrays empty over the Qt path" bug.
        s << "m_client->invokeRemoteMethod(" << targetExpr << ", \"" << md.name << "\", packVariantList(";
        for (int i = 0; i < nParams; ++i) {
            s << qtArgExpr(md.params[i].type, qs(md.params[i].name));
            if (i + 1 < nParams) s << ", ";
        }
        // The caller's deadline, not a hard-coded default: this is the overload
        // that carries BOTH the deadline and the error out-channel.
        s << "), timeout, &_err);\n";
        s << "    if (err) *err = _err;\n";
        s << "    else if (!_err.ok()) qWarning() << \"" << className << "::" << md.name
          << ": remote call failed:\" << QString::fromStdString(_err.message);\n";

        if (ret != "void")
            s << "    " << returnConversionFor(md.returnType, ret) << "\n";
        s << "}\n\n";

        // Shared between the two async entry points so they cannot drift in how
        // they marshal args or decode the reply.
        auto emitAsyncParams = [&]() {
            for (int i = 0; i < nParams; ++i) {
                emitParam(s, lidlTypeToQt(md.params[i].type), md.params[i].name);
                if (i + 1 < nParams) s << ", ";
            }
            if (nParams > 0) s << ", ";
        };
        // Same one-element-per-arg packing as the sync path (see above): a
        // QVariantList-typed arg must not be spread across the args list.
        auto emitAsyncArgs = [&]() {
            s << "packVariantList(";
            for (int i = 0; i < nParams; ++i) {
                s << qtArgExpr(md.params[i].type, qs(md.params[i].name));
                if (i + 1 < nParams) s << ", ";
            }
            s << ")";
        };
        // The QVariant -> typed-return expression, given the QVariant's name.
        auto asyncDecodeExpr = [&](const QString& var) -> QString {
            if (ret == "void") return QString();
            if (ret == "QVariant") return var;
            return var + ".isValid() ? " + asyncReturnConversionFor(md.returnType, ret)
                 + " : " + asyncDefaultVal(ret);
        };

        s << "void " << className << "::" << md.name << "Async(";
        emitAsyncParams();
        s << "std::function<void(" << (ret == "void" ? "void" : ret) << ")> callback, Timeout timeout) {\n";
        s << "    if (!callback) return;\n";
        s << "    m_client->invokeRemoteMethodAsync(" << targetExpr << ", \"" << md.name << "\", ";
        emitAsyncArgs();
        // ONE-argument lambda -> LogosAPIClient::AsyncResultCallback, i.e. the
        // historical value-only transport overload.
        s << ", [callback](QVariant v) {\n";
        if (ret == "void") s << "        callback();\n";
        else               s << "        callback(" << asyncDecodeExpr("v") << ");\n";
        s << "    }, timeout);\n";
        s << "}\n\n";

        // Result-carrying async: a TWO-argument lambda, so it binds to the
        // transport's CallError-aware AsyncResultErrorCallback overload. The
        // value on failure is exactly what `<name>Async` would have delivered;
        // what changes is that the callback can now tell.
        s << "void " << className << "::" << md.name << "AsyncResult(";
        emitAsyncParams();
        s << "std::function<void(logos::AsyncResult<" << ret << ">)> callback, Timeout timeout) {\n";
        s << "    if (!callback) return;\n";
        s << "    m_client->invokeRemoteMethodAsync(" << targetExpr << ", \"" << md.name << "\", ";
        emitAsyncArgs();
        s << ", [callback](QVariant v, const logos::CallError& _err) {\n";
        s << "        logos::AsyncResult<" << ret << "> _r;\n";
        s << "        _r.error = _err;\n";
        s << "        if (_r.error.ok()) logosLidlDispatchRejection(v, _r.error);\n";
        if (ret == "void") s << "        (void)v;\n";
        else               s << "        _r.value = " << asyncDecodeExpr("v") << ";\n";
        s << "        callback(_r);\n";
        s << "    }, timeout);\n";
        s << "}\n\n";
    }

    return c;
}

// ---------------------------------------------------------------------------
// metadata.json
// ---------------------------------------------------------------------------

QString lidlGenerateMetadataJson(const ModuleDecl& module)
{
    QJsonObject obj;
    obj["name"] = qs(module.name);
    obj["version"] = module.version.empty() ? QStringLiteral("0.0.0") : qs(module.version);
    obj["type"] = "core";
    obj["category"] = module.category.empty() ? QStringLiteral("general") : qs(module.category);
    obj["description"] = qs(module.description);
    obj["main"] = qs(module.name) + "_plugin";
    QJsonArray deps;
    for (const std::string& d : module.depends)
        deps.append(qs(d));
    obj["dependencies"] = deps;
    QJsonDocument doc(obj);
    return doc.toJson(QJsonDocument::Indented);
}

// ---------------------------------------------------------------------------
// Full pipeline (from .lidl file)
// ---------------------------------------------------------------------------

int lidlGenerateClientStubs(const QString& lidlPath, const QString& outputDir,
                            bool moduleOnly, QTextStream& out, QTextStream& err)
{
    QFileInfo fi(lidlPath);
    if (!fi.exists()) { err << "LIDL file does not exist: " << lidlPath << "\n"; return 2; }
    QFile file(fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { err << "Failed to open LIDL file: " << lidlPath << "\n"; return 3; }
    QString source = QString::fromUtf8(file.readAll());
    file.close();

    LidlParseResult pr = lidlParse(source);
    if (pr.hasError()) { err << lidlPath << ":" << pr.errorLine << ":" << pr.errorColumn << ": " << pr.error << "\n"; return 4; }

    LidlValidationResult vr = lidlValidate(pr.module);
    if (vr.hasErrors()) { for (const std::string& e : vr.errors) err << lidlPath << ": " << e << "\n"; return 5; }
    {
        QString recErr;
        if (!lidlCheckRecords(pr.module, &recErr)) { err << lidlPath << ": " << recErr << "\n"; return 5; }
    }

    const ModuleDecl& mod = pr.module;
    QString genDirPath = outputDir.isEmpty() ? QDir::current().filePath("logos-cpp-sdk/cpp/generated") : outputDir;
    QDir().mkpath(genDirPath);

    QString headerAbs = QDir(genDirPath).filePath(qs(mod.name) + "_api.h");
    QString sourceAbs = QDir(genDirPath).filePath(qs(mod.name) + "_api.cpp");

    { QFile f(headerAbs); if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { err << "Failed to write: " << headerAbs << "\n"; return 6; } f.write(lidlMakeHeader(mod).toUtf8()); }
    { QFile f(sourceAbs); if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { err << "Failed to write: " << sourceAbs << "\n"; return 7; } f.write(lidlMakeSource(mod).toUtf8()); }
    { QString metaPath = QDir(genDirPath).filePath("metadata.json"); QFile f(metaPath); if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { err << "Failed to write: " << metaPath << "\n"; return 8; } f.write(lidlGenerateMetadataJson(mod).toUtf8()); }

    out << "Generated: " << headerAbs << " and " << sourceAbs << "\n";

    if (!moduleOnly) {
        QDir genDir(genDirPath);
        QStringList headers = genDir.entryList(QStringList() << "*_api.h", QDir::Files | QDir::Readable);
        { QString content; QTextStream ss(&content);
          ss << "#pragma once\n#include \"logos_api.h\"\n#include \"logos_api_client.h\"\n\n";
          for (const QString& h : headers) ss << "#include \"" << h << "\"\n";
          ss << "\nstruct LogosModules {\n    explicit LogosModules(LogosAPI* api) : api(api)";
          for (const QString& h : headers) { QString base = h; base.chop(6); ss << ", \n        " << base << "(api)"; }
          ss << " {}\n    LogosAPI* api;\n";
          for (const QString& h : headers) { QString base = h; base.chop(6); ss << "    " << lidlToPascalCase(base) << " " << base << ";\n"; }
          ss << "};\n";
          QFile f(genDir.filePath("logos_sdk.h")); if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { err << "Failed to write umbrella\n"; return 9; } f.write(content.toUtf8()); }
        { QStringList sources = genDir.entryList(QStringList() << "*_api.cpp", QDir::Files | QDir::Readable);
          QString content; QTextStream ss(&content); ss << "#include \"logos_sdk.h\"\n\n";
          for (const QString& c : sources) ss << "#include \"" << c << "\"\n"; ss << "\n";
          QFile f(genDir.filePath("logos_sdk.cpp")); if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { err << "Failed to write umbrella\n"; return 10; } f.write(content.toUtf8()); }
        out << "Generated: logos_sdk.h and logos_sdk.cpp\n";
    }
    out << "Generated: metadata.json\n";
    out.flush();
    return 0;
}
