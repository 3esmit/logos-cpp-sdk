#include "generator_lib.h"

#include "metadata_dependencies.h"

#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

QString toPascalCase(const QString& name)
{
    QString out;
    bool cap = true;
    for (QChar c : name) {
        if (!c.isLetterOrNumber()) { cap = true; continue; }
        if (cap) { out.append(c.toUpper()); cap = false; }
        else { out.append(c.toLower()); }
    }
    if (out.isEmpty()) return QString("Module");
    return out;
}

QString normalizeType(QString t)
{
    t = t.trimmed();
    if (t.startsWith("const ")) t = t.mid(6);
    t = t.trimmed();
    // Drop reference and pointer qualifiers
    if (t.endsWith('&') || t.endsWith('*')) t.chop(1);
    t = t.trimmed();
    return t;
}

QString mapParamType(const QString& qtType)
{
    const QString base = normalizeType(qtType);
    static const QSet<QString> known = {
        "void","bool","int","qlonglong","qulonglong","double","float","QString","QStringList","QByteArray","QJsonArray","QVariantList","QVariantMap","QVariant"
    };
    if (known.contains(base)) return base;
    // Fallback to QVariant for unknown types
    return QString("QVariant");
}

QString mapReturnType(const QString& qtType)
{
    const QString base = normalizeType(qtType);
    if (base.isEmpty() || base == "void") return QString("void");
    static const QSet<QString> known = {
        "bool","int","qlonglong","qulonglong","double","float","QString","QStringList","QByteArray","QJsonArray","QVariantList","QVariantMap","QVariant","LogosResult"
    };
    if (known.contains(base)) return base;
    return QString("QVariant");
}

// How an INCOMING provider argument is turned into the type the author
// declared. Not the same job as toQVariantConversion below, which converts a
// value the module already owns.
//
// The Qt conversions coerce: `args.at(0).toULongLong()` turned echoUint(-1)
// into 18446744073709551615 and `.toLongLong()` turned echoInt(3.7) into 4, so
// the author's method body never saw the value the caller actually sent, while
// every non-Qt provider answered {"code":"dispatch_failed"} for the same input.
// logos::qtArgFromVariant<T> routes the value through the canonical codec
// instead — one rule, shared with the QMetaObject dispatch in logos-qt-sdk, and
// deliberately NOT re-derived here (the codec is what knows that a whole-valued
// 3.0 is a legal integer and 3.7 is not).
//
// A type the codec has no rule for keeps the old conversion verbatim: those are
// module-author types the generator already treated as `any`, and routing them
// through the codec would be a compile error rather than a behaviour change.
QString toProviderArgDecode(const QString& type, const QString& argExpr,
                            const QString& path)
{
    static const QSet<QString> codecKnown = {
        "bool","int","qlonglong","qulonglong","double","float",
        "QString","QStringList","QByteArray","QJsonArray","QJsonObject",
        "QVariantList","QVariantMap","QVariant","LogosResult"
    };
    if (!codecKnown.contains(type))
        return toQVariantConversion(type, argExpr);
    return "logos::qtArgFromVariant<" + type + ">(" + argExpr + ", \"" + path + "\")";
}

QString toQVariantConversion(const QString& type, const QString& argExpr)
{
    if (type == "int") return argExpr + ".toInt()";
    // LIDL int/uint are 64-bit; toInt() would truncate and re-sign them.
    if (type == "qlonglong") return argExpr + ".toLongLong()";
    if (type == "qulonglong") return argExpr + ".toULongLong()";
    if (type == "bool") return argExpr + ".toBool()";
    if (type == "double") return argExpr + ".toDouble()";
    if (type == "float") return argExpr + ".toFloat()";
    if (type == "QString") return argExpr + ".toString()";
    if (type == "QStringList") return argExpr + ".toStringList()";
    if (type == "QByteArray") return argExpr + ".toByteArray()";
    if (type == "QJsonArray") return "qvariant_cast<QJsonArray>(" + argExpr + ")";
    if (type == "QVariantList") return argExpr + ".toList()";
    if (type == "QVariantMap") return argExpr + ".toMap()";
    if (type == "QVariant") return argExpr;
    if (type == "LogosResult") return argExpr + ".value<LogosResult>()";
    return argExpr + ".toString()";
}

// ─── std (pure-C++) type-mapping table ───────────────────────────────────
//
// File-local — not exposed in generator_lib.h. This is the type table the
// Qt-free surface (ApiStyle::Lp) exposes: the wrapper's signatures are std
// types so a universal / cdylib module's own translation units never name a
// Qt type. Reached through paramTypeFor / returnTypeFor / byRefFor below (the
// non-Qt arm of each) and directly from the Lp backend's lpPushExpr /
// lpFromJsonExpr.

static QString mapParamTypeStd(const QString& qtType)
{
    const QString base = mapParamType(qtType);
    if (base == "QString")      return "std::string";
    if (base == "QStringList")  return "std::vector<std::string>";
    if (base == "QByteArray")   return "std::vector<uint8_t>";
    if (base == "QJsonArray")   return "LogosList";
    if (base == "QVariantList") return "LogosList";
    if (base == "QVariantMap")  return "LogosMap";
    if (base == "QVariant")     return "LogosMap";
    if (base == "int")          return "int64_t";
    if (base == "qlonglong")    return "int64_t";
    if (base == "qulonglong")   return "uint64_t";
    return base;
}

static QString mapReturnTypeStd(const QString& qtType)
{
    const QString base = mapReturnType(qtType);
    if (base == "void")         return "void";
    if (base == "QString")      return "std::string";
    if (base == "QStringList")  return "std::vector<std::string>";
    if (base == "QByteArray")   return "std::vector<uint8_t>";
    if (base == "QJsonArray")   return "LogosList";
    if (base == "QVariantList") return "LogosList";
    if (base == "QVariantMap")  return "LogosMap";
    if (base == "QVariant")     return "LogosMap";
    if (base == "LogosResult")  return "StdLogosResult";
    if (base == "int")          return "int64_t";
    if (base == "qlonglong")    return "int64_t";
    if (base == "qulonglong")   return "uint64_t";
    return base;
}

// ─── Records ─────────────────────────────────────────────────────────────
//
// A contract's `type Status { port: uint }` is a REAL C++ struct on the
// consumer side, not a QVariant / LogosMap the caller picks apart by string
// key. Without this a `bstr` field is the worst case: the caller receives the
// canonical `{"_bytes": "..."}` envelope and has to know to unwrap it, while
// every other language's consumer hands back plain bytes.
//
// main.cpp passes the declarations alongside the methods:
//   [ { "name": "Status", "fields": [ { "name": "port", "type": "qulonglong" } ] } ]
// spelled with the same Qt type names methods use, so a field can name another
// record ("Status"), a list of them ("QList<Status>") or a map of them
// ("QMap<QString, Status>"). The struct is nested in the wrapper class —
// `InfoModule::Status` — because one module consuming two deps that each
// declare `Status` includes both wrappers into the same translation unit.
//
// A field object carries an optional third key, `"optional"`. It is NOT a type
// name — it cannot be, because neither surface's type name can express `?T` the
// same way: Qt has no optional template and the std one does. `"type"` is
// always the VALUE type (optionality stripped) and `"optional"` says whether the
// slot may be empty, so the two LIDL spellings of one declaration (`? name: T`
// and `name: ?T`) arrive here as the same object and leave as the same code.
// The metaobject-introspection path never sets it; false is the historical
// behaviour.
//
// An empty record set leaves every emission path byte-for-byte as it was, and so
// does a record set in which nothing is optional.

struct RecordField { QString name; QString type; bool optional = false; };
struct RecordDef   { QString name; QVector<RecordField> fields; };
using RecordSet = QVector<RecordDef>;

// Forward declarations: the Lp (Qt-free) conversion helpers live further down
// with the rest of the Lp backend, but the record helpers below dispatch to
// them for non-record field types.
static QString lpPushExpr(const QString& qtType, const QString& argName);
static QString lpFromJsonExpr(const QString& qtType, const QString& jv);

static RecordSet parseRecords(const QJsonArray& records)
{
    RecordSet out;
    for (const QJsonValue& rv : records) {
        const QJsonObject ro = rv.toObject();
        RecordDef def;
        def.name = ro.value("name").toString();
        if (def.name.isEmpty()) continue;
        for (const QJsonValue& fv : ro.value("fields").toArray()) {
            const QJsonObject fo = fv.toObject();
            RecordField f;
            f.name = fo.value("name").toString();
            f.type = fo.value("type").toString();
            f.optional = fo.value("optional").toBool();
            if (f.name.isEmpty()) continue;
            def.fields.append(f);
        }
        out.append(def);
    }
    return out;
}

static bool isRecordName(const RecordSet& rs, const QString& name)
{
    for (const RecordDef& d : rs) if (d.name == name) return true;
    return false;
}

// How a type name mentions a record, if at all.
enum class RecordShape { None, Scalar, List, Map };

static RecordShape recordShape(const RecordSet& rs, const QString& t, QString* elem)
{
    if (rs.isEmpty()) return RecordShape::None;
    if (isRecordName(rs, t)) { if (elem) *elem = t; return RecordShape::Scalar; }
    if (t.startsWith("QList<") && t.endsWith(">")) {
        const QString e = t.mid(6, t.size() - 7).trimmed();
        if (isRecordName(rs, e)) { if (elem) *elem = e; return RecordShape::List; }
    }
    if (t.startsWith("QMap<QString,") && t.endsWith(">")) {
        const QString e = t.mid(13, t.size() - 14).trimmed();
        if (isRecordName(rs, e)) { if (elem) *elem = e; return RecordShape::Map; }
    }
    return RecordShape::None;
}

// The C++ spelling of a record-bearing type, or empty when `t` names none.
// `qual` qualifies the nested struct ("InfoModule::") where class scope does
// not already apply — i.e. a return type written before the `Class::` in a
// definition.
static QString recordCppType(const RecordSet& rs, const QString& t, ApiStyle style, const QString& qual)
{
    QString elem;
    const RecordShape shape = recordShape(rs, t, &elem);
    const QString q = qual + elem;
    switch (shape) {
    case RecordShape::None:   return QString();
    case RecordShape::Scalar: return q;
    case RecordShape::List:
        return style == ApiStyle::Qt ? "QList<" + q + ">" : "std::vector<" + q + ">";
    case RecordShape::Map:
        return style == ApiStyle::Qt ? "QMap<QString, " + q + ">"
                                     : "std::map<std::string, " + q + ">";
    }
    return QString();
}

// File-local conversion helpers emitted into the generated .cpp — never into
// the header, so a std/lp consumer's own translation units stay free of the
// wire type (QVariant / nlohmann::json) the conversion is written in.
static QString recToWireFn(const QString& record)   { return "recToWire_" + record; }
static QString recFromWireFn(const QString& record) { return "recFromWire_" + record; }

// Record value -> wire value, and back. Empty when `t` names no record.
static QString recordToWireExpr(const RecordSet& rs, const QString& t, ApiStyle style, const QString& expr)
{
    QString elem;
    const RecordShape shape = recordShape(rs, t, &elem);
    if (shape == RecordShape::None) return QString();
    const QString conv = recToWireFn(elem);
    if (shape == RecordShape::Scalar) return conv + "(" + expr + ")";
    // Locals are named apart from the record encoder/decoder's own `__m` / `__j`
    // / `__out`: these lambdas are emitted INSIDE those functions when a record
    // has a container-of-record field, and a shadowing local silently reads
    // itself (caught by -Wuninitialized, not by any assertion on the text).
    if (style == ApiStyle::Lp) {
        if (shape == RecordShape::List)
            return "[&]{ nlohmann::json __acc = nlohmann::json::array(); for (const auto& __e : "
                 + expr + ") __acc.push_back(" + conv + "(__e)); return __acc; }()";
        return "[&]{ nlohmann::json __acc = nlohmann::json::object(); for (const auto& __kv : "
             + expr + ") __acc[__kv.first] = " + conv + "(__kv.second); return __acc; }()";
    }
    if (shape == RecordShape::List)
        return "[&]{ QVariantList __acc; for (const auto& __e : " + expr
             + ") __acc.append(" + conv + "(__e)); return __acc; }()";
    // Map, Qt surface: the source container is a QMap, so keys are QString.
    return "[&]{ QVariantMap __acc; for (auto __i = " + expr + ".cbegin(); __i != " + expr
         + ".cend(); ++__i) __acc.insert(__i.key(), " + conv + "(__i.value())); return __acc; }()";
}

static QString recordFromWireExpr(const RecordSet& rs, const QString& t, ApiStyle style,
                                  const QString& wire, const QString& qual)
{
    QString elem;
    const RecordShape shape = recordShape(rs, t, &elem);
    if (shape == RecordShape::None) return QString();
    const QString conv = recFromWireFn(elem);
    const QString cpp = recordCppType(rs, t, style, qual);
    if (shape == RecordShape::Scalar) return conv + "(" + wire + ")";
    if (style == ApiStyle::Lp) {
        if (shape == RecordShape::List)
            return "[&]{ " + cpp + " __acc; const nlohmann::json& __src = " + wire
                 + "; if (__src.is_array()) for (const auto& __e : __src) __acc.push_back(" + conv
                 + "(__e)); return __acc; }()";
        return "[&]{ " + cpp + " __acc; const nlohmann::json& __src = " + wire
             + "; if (__src.is_object()) for (auto __i = __src.begin(); __i != __src.end(); ++__i) "
               "__acc[__i.key()] = " + conv + "(__i.value()); return __acc; }()";
    }
    if (shape == RecordShape::List)
        return "[&]{ " + cpp + " __acc; for (const QVariant& __e : (" + wire
             + ").toList()) __acc.push_back(" + conv + "(__e)); return __acc; }()";
    // Map, Qt surface: the destination container is a QMap, so keys are QString.
    return "[&]{ " + cpp + " __acc; const QVariantMap __src = (" + wire
         + ").toMap(); for (auto __i = __src.cbegin(); __i != __src.cend(); ++__i) "
           "__acc.insert(__i.key(), " + conv + "(__i.value())); return __acc; }()";
}

// Param-type predicate: passed by const-ref?
static bool isStdRefType(const QString& t)
{
    return t == "std::string" || t.startsWith("std::vector")
        || t == "std::map" || t.startsWith("std::map")
        || t == "LogosMap" || t == "LogosList";
}

static bool isQtRefType(const QString& t)
{
    // Matches the pre-refactor Qt-style by-ref set exactly. `QByteArray`
    // and `LogosResult` are intentionally NOT included — the original
    // generator emitted those parameter types by value, and the goal
    // of routing existing Qt-style wrappers through this predicate is
    // to keep the generated signatures bit-for-bit unchanged. Adding
    // them to the set would have broken downstream code that took
    // the address-of, overloaded on the parameter type, or relied on
    // the by-value signature in shipped headers. (Reported by Copilot
    // review on PR #61.)
    return t == "QString" || t == "QStringList"
        || t == "QJsonArray" || t == "QVariantList" || t == "QVariantMap";
}

// ─── Record-aware type / conversion dispatch ─────────────────────────────
//
// The one entry point every emission site goes through. A record-bearing type
// takes the record path; everything else falls through to the pre-existing
// mapping tables unchanged, so an empty record set is a no-op.

static QString paramTypeFor(const QString& qtType, ApiStyle style, const RecordSet& rs,
                            const QString& qual = QString())
{
    const QString rec = recordCppType(rs, qtType, style, qual);
    if (!rec.isEmpty()) return rec;
    return (style == ApiStyle::Qt) ? mapParamType(qtType) : mapParamTypeStd(qtType);
}

static QString returnTypeFor(const QString& qtType, ApiStyle style, const RecordSet& rs,
                             const QString& qual = QString())
{
    const QString rec = recordCppType(rs, qtType, style, qual);
    if (!rec.isEmpty()) return rec;
    return (style == ApiStyle::Qt) ? mapReturnType(qtType) : mapReturnTypeStd(qtType);
}

// Records are structs — always by const-ref, never copied into a call.
static bool byRefFor(const QString& qtType, const QString& cppType, ApiStyle style, const RecordSet& rs)
{
    if (recordShape(rs, qtType, nullptr) != RecordShape::None) return true;
    return (style == ApiStyle::Qt) ? isQtRefType(cppType) : isStdRefType(cppType);
}

// Typed value -> wire value (QVariant for Qt, nlohmann::json for Lp).
static QString toWireFor(const QString& qtType, ApiStyle style, const RecordSet& rs, const QString& expr)
{
    const QString rec = recordToWireExpr(rs, qtType, style, expr);
    if (!rec.isEmpty()) return rec;
    if (style == ApiStyle::Lp)  return lpPushExpr(qtType, expr);
    return expr;  // Qt: the wrapper's own surface already IS the wire type
}

// Wire value -> typed value.
static QString fromWireFor(const QString& qtType, ApiStyle style, const RecordSet& rs,
                           const QString& wire, const QString& qual = QString())
{
    const QString rec = recordFromWireExpr(rs, qtType, style, wire, qual);
    if (!rec.isEmpty()) return rec;
    if (style == ApiStyle::Lp)  return lpFromJsonExpr(qtType, wire);
    return toQVariantConversion(mapParamType(qtType), wire);
}

// ─── Optional record fields ──────────────────────────────────────────────
//
// `?T` is TWO-state: a value of T, or empty — never three. Each surface has
// exactly ONE empty inhabitant to spell that with, and they are different
// inhabitants, so the two surfaces answer differently:
//
//   Qt  — QVariant. Qt has no optional template; an INVALID QVariant is its
//         empty inhabitant. The value type is lost (a consumer cannot tell
//         `?tstr` from `?uint`), which is the same answer the experimental
//         client-stub backend gives for the same surface — see
//         lidl_gen_client.cpp's lidlFieldTypeQt. Deliberately identical: two
//         Qt consumer generators disagreeing about one contract is the bug
//         class this whole change is about.
//
//   Lp  — std::optional<T>, so the std surface KEEPS the value type.
//         std::nullopt is C++'s single empty inhabitant, and the encoder that
//         pairs with it is logos-protocol's Codec<std::optional<T>>. Same
//         answer the cdylib backend gives (lidlFieldTypeCdylib).
//
// EXCEPT over the untyped-JSON aliases, on the Lp surface only: LogosMap and
// LogosList are nlohmann::json, and json already has `null` among its
// inhabitants, so std::optional<LogosMap> would give `?any` TWO empty
// spellings and make it three-state. `?any` / `?{K:V}` / `?[any]` therefore
// collapse onto the bare alias — same two states, one C++ type. (Again the
// cdylib rule, verbatim.)
static bool lpAliasIsAlreadyNullable(const QString& lpType)
{
    return lpType == "LogosMap" || lpType == "LogosList";
}

// The C++ spelling of a record FIELD. Non-optional fields go through
// paramTypeFor unchanged, so a contract that declares no optional emits
// byte-for-byte what it emitted before.
static QString fieldTypeFor(const RecordField& f, ApiStyle style, const RecordSet& rs)
{
    const QString value = paramTypeFor(f.type, style, rs);
    if (!f.optional) return value;
    if (style == ApiStyle::Qt) return QStringLiteral("QVariant");
    if (lpAliasIsAlreadyNullable(value)) return value;
    return "std::optional<" + value + ">";
}

// True when some field actually materialises a std::optional on the Lp surface
// — gates the generated `#include <optional>`, so a contract with no optional
// (or one whose only optionals are untyped-JSON aliases) keeps its header
// byte-for-byte unchanged.
static bool recordsUseStdOptional(const RecordSet& rs)
{
    for (const RecordDef& d : rs)
        for (const RecordField& f : d.fields)
            if (f.optional && !lpAliasIsAlreadyNullable(paramTypeFor(f.type, ApiStyle::Lp, rs)))
                return true;
    return false;
}

// Whether an optional field is carried in a std::optional (as opposed to an
// alias that is already nullable, or the Qt surface's QVariant). Decides which
// encode/decode shape the conversions below emit.
static bool fieldIsWrappedOptional(const RecordField& f, ApiStyle style, const RecordSet& rs)
{
    return f.optional && style == ApiStyle::Lp
        && !lpAliasIsAlreadyNullable(paramTypeFor(f.type, style, rs));
}

// The struct declarations, emitted inside the wrapper class.
static void emitRecordStructs(QTextStream& s, const RecordSet& rs, ApiStyle style)
{
    if (rs.isEmpty()) return;
    s << "    // Record types declared by the contract.\n";
    for (const RecordDef& d : rs) {
        s << "    struct " << d.name << " {\n";
        for (const RecordField& f : d.fields)
            s << "        " << fieldTypeFor(f, style, rs) << " " << f.name << "{};\n";
        s << "    };\n";
    }
    s << "\n";
}

// The struct <-> wire conversions, emitted as file-local statics in the
// generated .cpp. Declared up front so records can reference each other (and
// themselves, through a list field) regardless of declaration order.
static void emitRecordConversions(QTextStream& s, const RecordSet& rs, ApiStyle style,
                                  const QString& className)
{
    if (rs.isEmpty()) return;
    const QString wire = (style == ApiStyle::Lp) ? "nlohmann::json" : "QVariant";
    const QString qual = className + "::";

    for (const RecordDef& d : rs) {
        s << "static " << wire << " " << recToWireFn(d.name)
          << "(const " << qual << d.name << "& v);\n";
        s << "static " << qual << d.name << " " << recFromWireFn(d.name)
          << "(const " << wire << "& w);\n";
    }
    s << "\n";

    for (const RecordDef& d : rs) {
        // Encode.
        s << "static " << wire << " " << recToWireFn(d.name)
          << "(const " << qual << d.name << "& v) {\n";
        if (style == ApiStyle::Lp) {
            s << "    nlohmann::json __j = nlohmann::json::object();\n";
            for (const RecordField& f : d.fields) {
                if (fieldIsWrappedOptional(f, style, rs)) {
                    // A record field is a NAMED slot, so empty is spelled by
                    // OMITTING the key — never by writing null. (A positional
                    // slot has no key to omit and writes null instead; arity
                    // must never change.) The round trip is therefore
                    // canonicalising, not identity: a peer that sent
                    // `"f": null` gets the key back omitted, and both spellings
                    // decode to the same single empty state.
                    s << "    if (v." << f.name << ".has_value()) __j[\"" << f.name << "\"] = "
                      << toWireFor(f.type, style, rs, "(*v." + f.name + ")") << ";\n";
                    continue;
                }
                s << "    __j[\"" << f.name << "\"] = "
                  << toWireFor(f.type, style, rs, "v." + f.name) << ";\n";
            }
            s << "    return __j;\n";
        } else {
            s << "    QVariantMap __m;\n";
            for (const RecordField& f : d.fields) {
                if (f.optional) {
                    // Same named-slot rule on the Qt surface: an INVALID
                    // QVariant is empty, and empty omits the key. Inserting it
                    // would encode `"f": null`, which is the positional
                    // spelling.
                    s << "    if (v." << f.name << ".isValid()) __m.insert(QStringLiteral(\""
                      << f.name << "\"), v." << f.name << ");\n";
                    continue;
                }
                // Qt's surface type IS the wire type for non-record fields, so
                // fromValue is what puts it in the map; records/containers
                // already produce a QVariant-compatible value.
                const QString v = toWireFor(f.type, style, rs, "v." + f.name);
                const bool isRec = recordShape(rs, f.type, nullptr) != RecordShape::None;
                s << "    __m.insert(QStringLiteral(\"" << f.name << "\"), "
                  << (isRec ? v : "QVariant::fromValue(" + v + ")")
                  << ");\n";
            }
            s << "    return __m;\n";
        }
        s << "}\n\n";

        // Decode. A missing / mistyped field keeps its default rather than
        // failing the whole call — same leniency the scalar paths use.
        s << "static " << qual << d.name << " " << recFromWireFn(d.name)
          << "(const " << wire << "& w) {\n";
        s << "    " << qual << d.name << " __out;\n";
        if (style == ApiStyle::Lp) {
            s << "    if (!w.is_object()) return __out;\n";
            for (const RecordField& f : d.fields) {
                const QString acc = "w.at(\"" + f.name + "\")";
                if (fieldIsWrappedOptional(f, style, rs)) {
                    // An absent key and an explicit null are the SAME state on
                    // decode, so both must leave the field nullopt. Testing
                    // only `contains` would decode `"f": null` through the
                    // value conversion and turn empty into a VALUE (0, "").
                    s << "    if (w.contains(\"" << f.name << "\") && !" << acc
                      << ".is_null()) __out." << f.name << " = "
                      << fromWireFor(f.type, style, rs, acc, qual) << ";\n";
                    continue;
                }
                s << "    if (w.contains(\"" << f.name << "\")) __out." << f.name << " = "
                  << fromWireFor(f.type, style, rs, acc, qual) << ";\n";
            }
        } else {
            s << "    const QVariantMap __m = w.toMap();\n";
            for (const RecordField& f : d.fields) {
                const QString acc = "__m.value(QStringLiteral(\"" + f.name + "\"))";
                if (f.optional) {
                    // Absent and null both arrive as an INVALID QVariant — the
                    // same state, as the contract requires. Converting (a
                    // `.toString()` on an optional `tstr`) would have turned
                    // empty into "", which is a value.
                    s << "    __out." << f.name << " = " << acc << ";\n";
                    continue;
                }
                s << "    __out." << f.name << " = "
                  << fromWireFor(f.type, style, rs, acc, qual) << ";\n";
            }
        }
        s << "    return __out;\n";
        s << "}\n\n";
    }
}

QString makeHeader(const QString& moduleName, const QString& className, const QJsonArray& methods, ApiStyle apiStyle, const QJsonArray& events, BindMode bindMode, const QJsonArray& records)
{
    if (apiStyle == ApiStyle::Lp)
        return makeHeaderLp(moduleName, className, methods, events, bindMode, records);
    const RecordSet rs = parseRecords(records);
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
    s << "#include \"logos_object.h\"\n\n";
    s << "class " << className << " {\n";
    s << "public:\n";
    emitRecordStructs(s, rs, apiStyle);
    if (bindMode == BindMode::Bound) {
        // Interface wrapper: the module to talk to is chosen at runtime.
        s << "    explicit " << className << "(LogosAPI* api, const QString& moduleName);\n\n";
    } else {
        s << "    explicit " << className << "(LogosAPI* api);\n\n";
    }
    // Event subscription surface — Qt-typed. Receive-side only: a consumer
    // wrapper subscribes, it does not source events.
    s << "    using RawEventCallback = std::function<void(const QString&, const QVariantList&)>;\n";
    s << "    using EventCallback = std::function<void(const QVariantList&)>;\n\n";
    s << "    bool on(const QString& eventName, RawEventCallback callback);\n";
    s << "    bool on(const QString& eventName, EventCallback callback);\n";
    // Typed event subscribers — generated from the `.lidl` sidecar shipped
    // with the dep's pre-built headers (via --events-from). One typed
    // adapter per declared event, callback-arg types follow apiStyle.
    // The generic `on(name, cb)` channel above stays available alongside them.
    for (const QJsonValue& ev : events) {
        const QJsonObject eo = ev.toObject();
        const QString evName = eo.value("name").toString();
        if (evName.isEmpty()) continue;
        // `on` + capitalized event name. `evName` is the verbatim name
        // the impl declared in its `logos_events:` block (typically
        // camelCase, e.g. `userLoggedIn`), so we just uppercase its
        // first letter — `toPascalCase` would clobber the internal
        // camelCase boundaries (snake_case input is its target).
        QString cap = evName;
        if (!cap.isEmpty()) cap[0] = cap[0].toUpper();
        const QString accessorName = QString("on") + cap;
        const QJsonArray evParams = eo.value("params").toArray();

        // Build the callback's parameter list using apiStyle's type table.
        QString cbParams;
        for (int i = 0; i < evParams.size(); ++i) {
            const QJsonObject p = evParams.at(i).toObject();
            QString qtPt = p.value("type").toString();
            QString pt = paramTypeFor(qtPt, apiStyle, rs);
            bool byRef = byRefFor(qtPt, pt, apiStyle, rs);
            if (byRef) cbParams += "const " + pt + "& ";
            else       cbParams += pt + " ";
            cbParams += p.value("name").toString();
            if (i + 1 < evParams.size()) cbParams += ", ";
        }
        s << "    bool " << accessorName
          << "(std::function<void(" << cbParams << ")> callback);\n";
    }
    if (!events.isEmpty()) s << "\n";
    // Methods
    for (const QJsonValue& v : methods) {
        const QJsonObject o = v.toObject();
        const bool invokable = o.value("isInvokable").toBool();
        if (!invokable) continue;
        const QString name = o.value("name").toString();
        const QString qtRet = o.value("returnType").toString();
        const QString ret = returnTypeFor(qtRet, apiStyle, rs);
        s << "    " << ret << " " << name << "(";
        QJsonArray params = o.value("parameters").toArray();
        for (int i = 0; i < params.size(); ++i) {
            QJsonObject p = params.at(i).toObject();
            QString qtPt = p.value("type").toString();
            QString pt = paramTypeFor(qtPt, apiStyle, rs);
            QString pn = p.value("name").toString();
            bool byRef = byRefFor(qtPt, pt, apiStyle, rs);
            if (byRef) s << "const " << pt << "& " << pn;
            else       s << pt << " " << pn;
            if (i + 1 < params.size()) s << ", ";
        }
        // Optional error out-channel: pass a logos::CallError* to distinguish
        // a failed remote call from a legitimately default-valued result.
        // Existing call sites compile unchanged.
        if (!params.isEmpty()) s << ", ";
        s << "logos::CallError* err = nullptr);\n";
        // Async overload: same params + callback + optional Timeout
        QString asyncCallbackType = (ret == "void")
            ? QString("std::function<void()>")
            : QString("std::function<void(") + ret + ")>";
        s << "    void " << name << "Async(";
        for (int i = 0; i < params.size(); ++i) {
            QJsonObject p = params.at(i).toObject();
            QString qtPt = p.value("type").toString();
            QString pt = paramTypeFor(qtPt, apiStyle, rs);
            QString pn = p.value("name").toString();
            bool byRef = byRefFor(qtPt, pt, apiStyle, rs);
            if (byRef) s << "const " << pt << "& " << pn;
            else       s << pt << " " << pn;
            if (i + 1 < params.size()) s << ", ";
        }
        if (params.size() > 0) s << ", ";
        s << asyncCallbackType << " callback, Timeout timeout = Timeout());\n";
    }
    s << "\nprivate:\n";
    // ensureReplica() is needed whenever the wrapper subscribes to events,
    // which on the Qt surface is always: the generic `on(...)` channel is
    // exposed even when the contract declares no typed events.
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

// The Qt consumer's rejection detector, emitted once per generated wrapper.
//
// A provider that REJECTS a call answers the canonical
// {"code":"dispatch_failed", "message":..., "origin":...} object as its RESULT,
// not as a transport error. Every provider flavour produces the same object
// (logos-qt-sdk `dispatchFailedVariant`, the generated cdylib dispatch, the Rust
// provider's `args::dispatch_failed`), and the Qt return table converts it like
// any other value — which ERASES it: `_result.toList()` on a map is `[]`,
// `.toString()` is "", `.toLongLong()` is 0. A caller then cannot tell "you sent
// me the wrong thing" from "the provider returned nothing".
//
// Detected here and folded into the logos::CallError out-channel the wrapper
// already uses to report a failed call, so a rejection reads exactly like every
// other failure on this surface — no new signature, no new type, and no change
// to any return value.
// Guarded because the umbrella (`logos_sdk.cpp`) textually #includes EVERY
// generated `<dep>_api.cpp`, so a module with more than one dependency puts
// several of these in ONE translation unit. Internal linkage handles the
// separate-TU case; only the preprocessor handles this one.
static void emitDispatchRejectionDetector(QTextStream& s)
{
    s << "#ifndef LOGOS_GENERATED_DISPATCH_REJECTION\n";
    s << "#define LOGOS_GENERATED_DISPATCH_REJECTION\n\n";
    s << "namespace {\n\n";
    s << "// True when `v` is the canonical provider REJECTION object rather than a\n";
    s << "// value; fills `out` with its {code, message, origin} on a match.\n";
    s << "//\n";
    s << "// The match is exact — those three fields, all strings, and that code — for the\n";
    s << "// same reason logos_rpc_status.h's isUnauthorizedSentinel is exact: an `any` or\n";
    s << "// map return carrying user data must never false-match.\n";
    s << "bool logosDispatchRejection(const QVariant& v, logos::CallError& out)\n";
    s << "{\n";
    s << "    QVariantMap m;\n";
    s << "    switch (v.userType()) {\n";
    s << "    case QMetaType::QVariantMap: m = v.toMap(); break;\n";
    s << "    // Defensive: some json_convert paths historically produced QJsonObject.\n";
    s << "    case QMetaType::QJsonObject: m = v.toJsonObject().toVariantMap(); break;\n";
    s << "    default: return false;\n";
    s << "    }\n";
    s << "    if (m.size() != 3) return false;\n";
    s << "    const QVariant code = m.value(QStringLiteral(\"code\"));\n";
    s << "    const QVariant message = m.value(QStringLiteral(\"message\"));\n";
    s << "    const QVariant origin = m.value(QStringLiteral(\"origin\"));\n";
    s << "    if (code.userType() != QMetaType::QString\n";
    s << "        || message.userType() != QMetaType::QString\n";
    s << "        || origin.userType() != QMetaType::QString) return false;\n";
    s << "    if (code.toString() != QStringLiteral(\"dispatch_failed\")) return false;\n";
    s << "    out.code = code.toString().toStdString();\n";
    s << "    out.message = message.toString().toStdString();\n";
    s << "    out.origin = origin.toString().toStdString();\n";
    s << "    return true;\n";
    s << "}\n\n";
    s << "} // namespace\n\n";
    s << "#endif  // LOGOS_GENERATED_DISPATCH_REJECTION\n\n";
}

QString makeSource(const QString& moduleName, const QString& className, const QString& headerBaseName, const QJsonArray& methods, ApiStyle apiStyle, const QJsonArray& events, BindMode bindMode, const QJsonArray& records)
{
    if (apiStyle == ApiStyle::Lp)
        return makeSourceLp(moduleName, className, headerBaseName, methods, events, bindMode, records);
    const RecordSet rs = parseRecords(records);
    // The rejection detector is only reachable from a method body, so a
    // contract with no invokable method must not emit it (an unused function in
    // an anonymous namespace is a -Wunused-function warning, and such a
    // contract's wrapper stays byte-identical to what it generated before).
    bool anyInvokable = false;
    for (const QJsonValue& mv : methods) {
        if (mv.toObject().value("isInvokable").toBool()) { anyInvokable = true; break; }
    }
    QString c;
    QTextStream s(&c);
    s << "#include \"" << headerBaseName << "\"\n\n";
    s << "#include <QDebug>\n";
    if (!rs.isEmpty() || anyInvokable) {
        // Record conversions build QVariantMaps; so does the rejection detector.
        s << "#include <QVariantMap>\n";
    }
    if (anyInvokable) {
        // The rejection detector reads a QJsonObject-shaped result defensively.
        s << "#include <QJsonObject>\n";
    }
    s << "\n";
    if (anyInvokable) emitDispatchRejectionDetector(s);
    emitRecordConversions(s, rs, apiStyle, className);
    // The expression every remote call uses to name its target module.
    // Static: the baked string literal "<moduleName>" (unchanged
    // behaviour). Bound: the m_moduleName member set from the runtime ctor
    // arg — so one interface wrapper can talk to any satisfying module.
    const QString targetExpr = (bindMode == BindMode::Bound)
        ? QStringLiteral("m_moduleName")
        : (QStringLiteral("\"") + moduleName + QStringLiteral("\""));
    if (bindMode == BindMode::Bound) {
        s << className << "::" << className << "(LogosAPI* api, const QString& moduleName) : m_api(api), m_client(api->getClient(moduleName)), m_moduleName(moduleName) {}\n\n";
    } else {
        s << className << "::" << className << "(LogosAPI* api) : m_api(api), m_client(api->getClient(\"" << moduleName << "\")), m_moduleName(QStringLiteral(\"" << moduleName << "\")) {}\n\n";
    }

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
    s << "    if (!callback) {\n";
    s << "        qWarning() << \"" << className << ": ignoring empty event callback for\" << eventName;\n";
    s << "        return false;\n";
    s << "    }\n";
    s << "    LogosObject* origin = ensureReplica();\n";
    s << "    if (!origin) {\n";
    s << "        return false;\n";
    s << "    }\n";
    s << "    m_client->onEvent(origin, eventName, callback);\n";
    s << "    return true;\n";
    s << "}\n\n";
    s << "bool " << className << "::on(const QString& eventName, EventCallback callback) {\n";
    s << "    if (!callback) {\n";
    s << "        qWarning() << \"" << className << ": ignoring empty event callback for\" << eventName;\n";
    s << "        return false;\n";
    s << "    }\n";
    s << "    return on(eventName, [callback](const QString&, const QVariantList& data) {\n";
    s << "        callback(data);\n";
    s << "    });\n";
    s << "}\n\n";

    // Typed event adapters — one per declared event. The callback type
    // uses the apiStyle's type surface; the body unmarshals from the
    // wire's QVariantList into typed args and invokes the user's
    // callback. Subscription uses the same `m_client->onEvent` channel
    // the generic `on(...)` uses.
    for (const QJsonValue& ev : events) {
        const QJsonObject eo = ev.toObject();
        const QString evName = eo.value("name").toString();
        if (evName.isEmpty()) continue;
        // `on` + capitalized event name. `evName` is the verbatim name
        // the impl declared in its `logos_events:` block (typically
        // camelCase, e.g. `userLoggedIn`), so we just uppercase its
        // first letter — `toPascalCase` would clobber the internal
        // camelCase boundaries (snake_case input is its target).
        QString cap = evName;
        if (!cap.isEmpty()) cap[0] = cap[0].toUpper();
        const QString accessorName = QString("on") + cap;
        const QJsonArray evParams = eo.value("params").toArray();

        // Callback signature
        QString cbParams;
        for (int i = 0; i < evParams.size(); ++i) {
            const QJsonObject p = evParams.at(i).toObject();
            QString qtPt = p.value("type").toString();
            QString pt = paramTypeFor(qtPt, apiStyle, rs);
            bool byRef = byRefFor(qtPt, pt, apiStyle, rs);
            if (byRef) cbParams += "const " + pt + "& ";
            else       cbParams += pt + " ";
            cbParams += p.value("name").toString();
            if (i + 1 < evParams.size()) cbParams += ", ";
        }
        s << "bool " << className << "::" << accessorName
          << "(std::function<void(" << cbParams << ")> callback) {\n";
        s << "    if (!callback) {\n";
        s << "        qWarning() << \"" << className << ": ignoring empty event callback for\" "
          << "<< QStringLiteral(\"" << evName << "\");\n";
        s << "        return false;\n";
        s << "    }\n";
        s << "    LogosObject* origin = ensureReplica();\n";
        s << "    if (!origin) return false;\n";
        s << "    m_client->onEvent(origin, QStringLiteral(\"" << evName << "\"), "
          << "[callback](const QString&, const QVariantList& _args) {\n";
        s << "        if (_args.size() < " << evParams.size() << ") return;\n";
        s << "        callback(";
        for (int i = 0; i < evParams.size(); ++i) {
            const QJsonObject p = evParams.at(i).toObject();
            QString qtPt = p.value("type").toString();
            // Build the QVariant → typed-arg conversion expression.
            const QString argExpr = QString("_args.at(%1)").arg(i);
            s << fromWireFor(qtPt, apiStyle, rs, argExpr);
            if (i + 1 < evParams.size()) s << ", ";
        }
        s << ");\n";
        s << "    });\n";
        s << "    return true;\n";
        s << "}\n\n";
    }

    for (const QJsonValue& v : methods) {
        const QJsonObject o = v.toObject();
        const bool invokable = o.value("isInvokable").toBool();
        if (!invokable) continue;
        const QString name = o.value("name").toString();
        const QString qtRet = o.value("returnType").toString();
        // Inside the class's own scope (parameter lists, bodies) a nested
        // record needs no qualification; a return type written before the
        // `Class::` in a definition does.
        const QString ret = returnTypeFor(qtRet, apiStyle, rs);
        const QString retQual = returnTypeFor(qtRet, apiStyle, rs, className + "::");
        QJsonArray params = o.value("parameters").toArray();

        // Helper closures kept inline so the signature and the call that
        // consumes it stay next to each other.
        auto emitParam = [&](const QJsonObject& p, bool& byRefOut) {
            QString qtPt = p.value("type").toString();
            QString pt = paramTypeFor(qtPt, apiStyle, rs);
            QString pn = p.value("name").toString();
            byRefOut = byRefFor(qtPt, pt, apiStyle, rs);
            if (byRefOut) s << "const " << pt << "& " << pn;
            else          s << pt << " " << pn;
        };
        auto wireArg = [&](const QJsonObject& p) -> QString {
            QString qtPt = p.value("type").toString();
            QString pn = p.value("name").toString();
            return toWireFor(qtPt, apiStyle, rs, pn);
        };

        // Signature
        s << retQual << " " << className << "::" << name << "(";
        for (int i = 0; i < params.size(); ++i) {
            bool byRef;
            emitParam(params.at(i).toObject(), byRef);
            if (i + 1 < params.size()) s << ", ";
        }
        if (!params.isEmpty()) s << ", ";
        s << "logos::CallError* err) {\n";

        // Body: perform call through the err-out overload. When the caller
        // passes a logos::CallError* it can distinguish a failed remote call
        // (e.g. the bound module is missing, or the provider REJECTED the
        // arguments) from a legitimately default-valued result; without it the
        // historical default-on-failure behavior is kept, now with a warning so
        // failures are at least visible in the module log.
        //
        // The result is captured even for a `void` return: a void method can be
        // rejected too, and the rejection object is the only place that says so.
        s << "    logos::CallError _err;\n";
        s << "    QVariant _result = ";

        // Wrap each argument in QVariant::fromValue so it becomes exactly ONE
        // element of the args list. A bare `QVariantList{v}` CONCATENATES a
        // QVariantList-typed arg (every `[T]` list) into the args list — sending
        // a 3-element [1,2,3] as three positional args — the historical "typed
        // arrays empty over the Qt path" bug. fromValue does not double-wrap an
        // already-QVariant (`any`) arg.
        s << "m_client->invokeRemoteMethod(" << targetExpr << ", \"" << name << "\", QVariantList{";
        for (int i = 0; i < params.size(); ++i) {
            s << "QVariant::fromValue(" << wireArg(params.at(i).toObject()) << ")";
            if (i + 1 < params.size()) s << ", ";
        }
        s << "}, Timeout(), &_err);\n";
        // A provider REJECTION arrives as the result, not as a transport error.
        // Fold it into the same error channel BEFORE the return table converts
        // it, or the conversion erases it (a rejected `[uint]` call answered []
        // — the whole list, not the bad element).
        s << "    if (_err.ok()) logosDispatchRejection(_result, _err);\n";
        s << "    if (err) *err = _err;\n";
        s << "    else if (!_err.ok()) qWarning() << \"" << className << "::" << name
          << ": remote call failed:\" << QString::fromStdString(_err.message);\n";

        // Return conversion
        const bool retIsRecord = recordShape(rs, qtRet, nullptr) != RecordShape::None;
        if (ret == "void") {
            // nothing
        } else if (retIsRecord) {
            s << "    return " << fromWireFor(qtRet, apiStyle, rs, "_result") << ";\n";
        } else if (ret == "bool") {
            s << "    return _result.toBool();\n";
        } else if (ret == "qlonglong") {
            s << "    return _result.toLongLong();\n";
        } else if (ret == "qulonglong") {
            s << "    return _result.toULongLong();\n";
        } else if (ret == "int") {
            s << "    return _result.toInt();\n";
        } else if (ret == "double") {
            s << "    return _result.toDouble();\n";
        } else if (ret == "float") {
            s << "    return _result.toFloat();\n";
        } else if (ret == "QString") {
            s << "    return _result.toString();\n";
        } else if (ret == "QStringList") {
            s << "    return _result.toStringList();\n";
        } else if (ret == "QByteArray") {
            // QVariant has no implicit conversion to QByteArray (unlike the
            // scalar to* accessors), so a `bstr` return needs an explicit
            // toByteArray() — matching the async path's qvariant_cast.
            s << "    return _result.toByteArray();\n";
        } else if (ret == "QJsonArray") {
            s << "    return qvariant_cast<QJsonArray>(_result);\n";
        } else if (ret == "QVariantList") {
            s << "    return _result.toList();\n";
        } else if (ret == "QVariantMap") {
            s << "    return _result.toMap();\n";
        } else if (ret == "LogosResult") {
            s << "    return _result.value<LogosResult>();\n";
        } else { // QVariant
            s << "    return _result;\n";
        }
        s << "}\n\n";

        // Async implementation
        s << "void " << className << "::" << name << "Async(";
        for (int i = 0; i < params.size(); ++i) {
            bool byRef;
            emitParam(params.at(i).toObject(), byRef);
            if (i + 1 < params.size()) s << ", ";
        }
        if (params.size() > 0) s << ", ";
        s << "std::function<void(" << (ret == "void" ? "void" : ret) << ")> callback, Timeout timeout) {\n";
        s << "    if (!callback) return;\n";
        s << "    m_client->invokeRemoteMethodAsync(" << targetExpr << ", \"" << name << "\", ";
        if (params.size() == 0) {
            s << "QVariantList()";
        } else {
            // Same one-element-per-arg wrapping as the sync path (see above): a
            // QVariantList-typed arg must not be spread across the args list.
            s << "QVariantList{";
            for (int i = 0; i < params.size(); ++i) {
                s << "QVariant::fromValue(" << wireArg(params.at(i).toObject()) << ")";
                if (i + 1 < params.size()) s << ", ";
            }
            s << "}";
        }
        s << ", [callback](QVariant v) {\n";
        // The async callback carries the value only — there is no CallError
        // parameter to fill, and adding one would change the generated public
        // surface. A rejection is at least made visible in the module log
        // instead of vanishing into the return conversion below.
        s << "        { logos::CallError _rej; if (logosDispatchRejection(v, _rej))\n";
        s << "              qWarning() << \"" << className << "::" << name
          << "Async: remote call failed:\" << QString::fromStdString(_rej.message); }\n";
        if (ret == "void") {
            s << "        (void)v; callback();\n";
        } else if (retIsRecord) {
            // A record decodes field by field; an invalid QVariant yields a
            // default-constructed struct, matching the scalar paths.
            s << "        callback(" << fromWireFor(qtRet, apiStyle, rs, "v", className + "::") << ");\n";
        } else {
            QString defaultVal;
            if (ret == "bool") defaultVal = "false";
            else if (ret == "int" || ret == "qlonglong" || ret == "qulonglong"
                     || ret == "double" || ret == "float") defaultVal = "0";
            else if (ret == "QString") defaultVal = "QString()";
            else if (ret == "QStringList") defaultVal = "QStringList()";
            else if (ret == "QJsonArray") defaultVal = "QJsonArray()";
            else if (ret == "QVariantList") defaultVal = "QVariantList()";
            else if (ret == "QVariantMap") defaultVal = "QVariantMap()";
            else defaultVal = ret + "{}";
            if (ret == "QVariant") {
                s << "        callback(v);\n";
            } else {
                s << "        callback(v.isValid() ? qvariant_cast<" << ret << ">(v) : " << defaultVal << ");\n";
            }
        }
        s << "    }, timeout);\n";
        s << "}\n\n";
    }
    return c;
}

// Join accumulated doc-comment lines into a description, preserving the
// original line breaks. Leading/trailing blank lines are dropped; interior
// blank lines (paragraph breaks) are kept.
static QString joinDocLines(QStringList lines)
{
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    return lines.join('\n');
}

QVector<ParsedMethod> parseProviderHeader(const QString& headerPath, QTextStream& err)
{
    QVector<ParsedMethod> methods;

    QFile file(headerPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        err << "Cannot open header file: " << headerPath << "\n";
        return methods;
    }

    QTextStream in(&file);
    QRegularExpression re(
        R"(^\s*LOGOS_METHOD\s+(.+?)\s+(\w+)\s*\(([^)]*)\)\s*;)"
    );

    // Accumulate comment lines immediately preceding a LOGOS_METHOD so the
    // doc comment becomes the method's description. Reset on any blank or
    // non-comment line, so only comments *adjacent* to the declaration count.
    QStringList pendingDoc;
    bool inBlockComment = false;

    while (!in.atEnd()) {
        QString rawLine = in.readLine();
        QString line = rawLine.trimmed();

        // Inside a multi-line /* ... */ block comment.
        if (inBlockComment) {
            QString text = line;
            int end = text.indexOf("*/");
            if (end >= 0) {
                text = text.left(end);
                inBlockComment = false;
            }
            text.remove(QRegularExpression(R"(^\*+\s?)")); // strip leading '*'
            text = text.trimmed();
            pendingDoc.append(text);
            continue;
        }

        auto match = re.match(rawLine);
        if (!match.hasMatch()) {
            // Only doc comments (/// or /** ... */ / /*! ... */) become the
            // description. Plain // and /* comments are ignored but leave any
            // pending doc intact; blank / code lines reset it so only comments
            // *adjacent* to the declaration attach.
            if (line.startsWith("///")) {
                QString text = line.mid(3);
                if (text.startsWith('<')) text = text.mid(1); // ///< trailing form
                text = text.trimmed();
                pendingDoc.append(text);
            } else if (line.startsWith("/**") || line.startsWith("/*!")) {
                QString text = line.mid(3);
                int end = text.indexOf("*/");
                if (end >= 0) text = text.left(end);
                else inBlockComment = true;
                text.remove(QRegularExpression(R"(^\*+\s?)"));
                text = text.trimmed();
                pendingDoc.append(text);
            } else if (line.startsWith("//") || line.startsWith("/*") || line.startsWith("*")) {
                // Non-doc comment: ignore, keep any pending doc comment.
            } else {
                pendingDoc.clear();
            }
            continue;
        }

        ParsedMethod m;
        m.returnType = normalizeType(match.captured(1));
        m.name = match.captured(2);
        m.description = joinDocLines(pendingDoc);
        pendingDoc.clear();

        QString paramStr = match.captured(3).trimmed();
        if (!paramStr.isEmpty()) {
            QStringList paramParts = paramStr.split(',');
            for (const QString& part : paramParts) {
                QString trimmed = part.trimmed();
                int eqIdx = trimmed.indexOf('=');
                if (eqIdx > 0) trimmed = trimmed.left(eqIdx).trimmed();
                int lastSpace = trimmed.lastIndexOf(' ');
                int lastAmp = trimmed.lastIndexOf('&');
                int splitAt = qMax(lastSpace, lastAmp);
                if (splitAt > 0) {
                    QString type = normalizeType(trimmed.left(splitAt + 1));
                    QString pname = trimmed.mid(splitAt + 1).trimmed();
                    m.params.append({type, pname});
                } else {
                    m.params.append({normalizeType(trimmed), QString("arg%1").arg(m.params.size())});
                }
            }
        }

        methods.append(m);
    }

    file.close();
    return methods;
}


// ─── ApiStyle::Lp (Qt-free) wrapper emission ─────────────────────────────
//
// A std-typed surface (the mapParamTypeStd / mapReturnTypeStd table above)
// whose generated body calls the logos-protocol C ABI through logos::LpClient
// instead of LogosAPIClient, so the wrapper's translation unit pulls in no Qt.
// This is the only remaining std-typed flavour; the retired ApiStyle::Std
// exposed the same signatures over a QVariant + LogosAPIClient body.
//
// Used for the cdylib outbound path (a Qt-free module calling its dependencies
// / subscribing to their events). The class still holds a single target;
// Static bakes it, Bound takes it at construction (interface dependencies).

// std value -> nlohmann::json push expression. nlohmann handles
// string/int64/double/bool/vector<string>/json (LogosMap/LogosList) directly;
// StdLogosResult is encoded as its {success,value,error} object.
static QString lpPushExpr(const QString& qtType, const QString& argName)
{
    const QString std = mapParamTypeStd(qtType);
    if (std == "StdLogosResult")
        return "nlohmann::json{{\"success\", " + argName + ".success}, {\"value\", "
             + argName + ".value}, {\"error\", " + argName + ".error}}";
    // Bytes must go out in the canonical tagged form; pushed raw, nlohmann would
    // serialize the vector as a plain JSON array of numbers.
    if (std == "std::vector<uint8_t>")
        return "logos::bytesToJson(" + argName + ")";
    return argName;
}

// nlohmann::json -> std value expression for a return value or an event arg.
// Lenient: a type mismatch yields the default-constructed value.
static QString lpFromJsonExpr(const QString& qtType, const QString& jv)
{
    const QString t = mapReturnTypeStd(qtType);
    if (t == "void")                     return QString();
    if (t == "std::string")              return "(" + jv + ".is_string() ? " + jv + ".get<std::string>() : std::string())";
    if (t == "int64_t")                  return "(" + jv + ".is_number_integer() ? " + jv + ".get<int64_t>() : (" + jv + ".is_number() ? static_cast<int64_t>(" + jv + ".get<double>()) : (int64_t)0))";
    if (t == "uint64_t")                 return "(" + jv + ".is_number_integer() ? " + jv + ".get<uint64_t>() : (" + jv + ".is_number() ? static_cast<uint64_t>(" + jv + ".get<double>()) : (uint64_t)0))";
    if (t == "double")                   return "(" + jv + ".is_number() ? " + jv + ".get<double>() : 0.0)";
    if (t == "bool")                     return "(" + jv + ".is_boolean() ? " + jv + ".get<bool>() : false)";
    if (t == "std::vector<std::string>") return "logos::jsonToStringVec(" + jv + ")";
    if (t == "std::vector<uint8_t>")     return "logos::jsonToBytes(" + jv + ")";
    // `any` (QVariant) is a raw json value of ANY shape — pass it through
    // unchanged. It shares the LogosMap std type with the `{tstr:any}` map
    // (QVariantMap), but only the map is forced to an object below; forcing
    // `any` to an object collapsed every non-object value (a string, a number,
    // an array) to `{}` (e.g. a proxy forwarding echoAny returned {} for "x").
    if (mapReturnType(qtType) == "QVariant") return jv;
    if (t == "LogosMap")                 return "(" + jv + ".is_object() ? " + jv + " : LogosMap::object())";
    if (t == "LogosList")                return "(" + jv + ".is_array() ? " + jv + " : LogosList::array())";
    if (t == "StdLogosResult")           return "logos::jsonToStdResult(" + jv + ")";
    return jv;
}

// Build the callback parameter list (std types, by-ref where appropriate) for
// a typed event accessor `on<Event>`.
static QString lpEventCbParams(const QJsonArray& evParams, const RecordSet& rs)
{
    QString cbParams;
    for (int i = 0; i < evParams.size(); ++i) {
        const QJsonObject p = evParams.at(i).toObject();
        const QString qtPt = p.value("type").toString();
        const QString pt = paramTypeFor(qtPt, ApiStyle::Lp, rs);
        if (byRefFor(qtPt, pt, ApiStyle::Lp, rs)) cbParams += "const " + pt + "& ";
        else                                      cbParams += pt + " ";
        cbParams += p.value("name").toString();
        if (i + 1 < evParams.size()) cbParams += ", ";
    }
    return cbParams;
}

static QString lpEventAccessorName(const QString& evName)
{
    QString cap = evName;
    if (!cap.isEmpty()) cap[0] = cap[0].toUpper();
    return QString("on") + cap;
}

QString makeHeaderLp(const QString& moduleName, const QString& className, const QJsonArray& methods, const QJsonArray& events, BindMode bindMode, const QJsonArray& records)
{
    (void)moduleName;
    const RecordSet rs = parseRecords(records);
    QString h;
    QTextStream s(&h);
    s << "#pragma once\n";
    s << "#include <cstdint>\n";
    s << "#include <string>\n";
    s << "#include <vector>\n";
    // Only when a field actually materialises one, so a contract with no
    // optional keeps its header byte-for-byte unchanged.
    if (recordsUseStdOptional(rs)) s << "#include <optional>\n";
    s << "#include <functional>\n";
    s << "#include <nlohmann/json.hpp>\n";
    s << "#include \"logos_json.h\"\n";
    s << "#include \"logos_result.h\"\n";
    s << "#include \"logos_call_error.h\"\n";
    s << "#include \"logos_lp_client.h\"\n";
    // Record maps are std::map on the Qt-free surface.
    if (!rs.isEmpty()) s << "#include <map>\n";
    s << "\n";

    s << "class " << className << " {\n";
    s << "public:\n";
    emitRecordStructs(s, rs, ApiStyle::Lp);
    if (bindMode == BindMode::Bound) {
        // Bound (interface) wrappers are THIN, copyable handles over
        // umbrella-owned persistent State, so a transient
        // `modules().bind_x(provider)` temporary can register an async
        // callback / event subscription that OUTLIVES the temporary: the
        // LpClient and its RAII subscriptions live in the umbrella for the
        // module's lifetime (mirroring the LogosAPI-owned-client model the
        // Qt/std flavor relies on). Owning the client by-value in the handle
        // would tear the subscription down when the temporary dies.
        s << "    struct State {\n";
        s << "        logos::LpClient client;\n";
        s << "        std::vector<logos::LpSubscription> subs;\n";
        s << "        State(const std::string& target, const std::string& origin) : client(target, origin) {}\n";
        s << "    };\n";
        s << "    explicit " << className << "(State* state) : m_state(state) {}\n\n";
    } else {
        s << "    explicit " << className << "(const std::string& origin);\n\n";
    }

    // Typed event subscribers — one per declared event.
    for (const QJsonValue& ev : events) {
        const QJsonObject eo = ev.toObject();
        const QString evName = eo.value("name").toString();
        if (evName.isEmpty()) continue;
        s << "    bool " << lpEventAccessorName(evName)
          << "(std::function<void(" << lpEventCbParams(eo.value("params").toArray(), rs) << ")> callback);\n";
    }
    if (!events.isEmpty()) s << "\n";

    // Methods: sync (with optional CallError out-param) + async overload.
    for (const QJsonValue& v : methods) {
        const QJsonObject o = v.toObject();
        if (!o.value("isInvokable").toBool()) continue;
        const QString name = o.value("name").toString();
        const QString ret = returnTypeFor(o.value("returnType").toString(), ApiStyle::Lp, rs);
        const QJsonArray params = o.value("parameters").toArray();

        s << "    " << ret << " " << name << "(";
        for (int i = 0; i < params.size(); ++i) {
            const QJsonObject p = params.at(i).toObject();
            const QString qtPt = p.value("type").toString();
            const QString pt = paramTypeFor(qtPt, ApiStyle::Lp, rs);
            if (byRefFor(qtPt, pt, ApiStyle::Lp, rs)) s << "const " << pt << "& " << p.value("name").toString();
            else                                     s << pt << " " << p.value("name").toString();
            if (i + 1 < params.size()) s << ", ";
        }
        if (!params.isEmpty()) s << ", ";
        s << "logos::CallError* err = nullptr);\n";

        const QString asyncCb = (ret == "void")
            ? QString("std::function<void()>")
            : QString("std::function<void(") + ret + ")>";
        s << "    void " << name << "Async(";
        for (int i = 0; i < params.size(); ++i) {
            const QJsonObject p = params.at(i).toObject();
            const QString qtPt = p.value("type").toString();
            const QString pt = paramTypeFor(qtPt, ApiStyle::Lp, rs);
            if (byRefFor(qtPt, pt, ApiStyle::Lp, rs)) s << "const " << pt << "& " << p.value("name").toString();
            else                                     s << pt << " " << p.value("name").toString();
            if (i + 1 < params.size()) s << ", ";
        }
        if (!params.isEmpty()) s << ", ";
        s << asyncCb << " callback);\n";
    }

    s << "\nprivate:\n";
    if (bindMode == BindMode::Bound) {
        s << "    State* m_state;  // umbrella-owned; the handle does not own it\n";
    } else {
        s << "    logos::LpClient m_client;\n";
        if (!events.isEmpty()) s << "    std::vector<logos::LpSubscription> m_subs;\n";
    }
    s << "};\n";
    return h;
}

QString makeSourceLp(const QString& moduleName, const QString& className, const QString& headerBaseName, const QJsonArray& methods, const QJsonArray& events, BindMode bindMode, const QJsonArray& records)
{
    const RecordSet rs = parseRecords(records);
    QString c;
    QTextStream s(&c);
    s << "#include \"" << headerBaseName << "\"\n";
    s << "#include <nlohmann/json.hpp>\n\n";
    emitRecordConversions(s, rs, ApiStyle::Lp, className);

    // How the wrapper reaches its persistent LpClient + subscription store.
    // Static (concrete dep): owns them by value — the wrapper itself is a
    // persistent member of the umbrella. Bound (interface): a thin handle
    // over umbrella-owned State, so a transient handle's async/event
    // registrations survive (the ctor is inline in the header).
    const QString clientExpr = (bindMode == BindMode::Bound) ? "m_state->client" : "m_client";
    const QString subsExpr   = (bindMode == BindMode::Bound) ? "m_state->subs"   : "m_subs";

    // Constructor: LpClient(target, origin). Static bakes the dep name in the
    // .cpp ctor; Bound's ctor is inline (takes the umbrella-owned State*).
    if (bindMode != BindMode::Bound)
        s << className << "::" << className << "(const std::string& origin)"
          << " : m_client(\"" << moduleName << "\", origin) {}\n\n";

    // Typed event adapters: subscribe via lp_subscribe (JSON array payload),
    // decode into typed args, keep the RAII subscription alive in m_subs.
    for (const QJsonValue& ev : events) {
        const QJsonObject eo = ev.toObject();
        const QString evName = eo.value("name").toString();
        if (evName.isEmpty()) continue;
        const QJsonArray evParams = eo.value("params").toArray();
        s << "bool " << className << "::" << lpEventAccessorName(evName)
          << "(std::function<void(" << lpEventCbParams(evParams, rs) << ")> callback) {\n";
        s << "    if (!callback) return false;\n";
        s << "    auto _sub = " << clientExpr << ".subscribe(\"" << evName << "\", [callback](nlohmann::json _a) {\n";
        s << "        if (!_a.is_array() || _a.size() < " << evParams.size() << ") return;\n";
        s << "        callback(";
        for (int i = 0; i < evParams.size(); ++i) {
            const QJsonObject p = evParams.at(i).toObject();
            s << fromWireFor(p.value("type").toString(), ApiStyle::Lp, rs,
                             QString("_a.at(%1)").arg(i), className + "::");
            if (i + 1 < evParams.size()) s << ", ";
        }
        s << ");\n";
        s << "    });\n";
        s << "    if (!_sub.valid()) return false;\n";
        s << "    " << subsExpr << ".push_back(std::move(_sub));\n";
        s << "    return true;\n";
        s << "}\n\n";
    }

    // Methods.
    for (const QJsonValue& v : methods) {
        const QJsonObject o = v.toObject();
        if (!o.value("isInvokable").toBool()) continue;
        const QString name = o.value("name").toString();
        const QString qtRet = o.value("returnType").toString();
        const QString ret = returnTypeFor(qtRet, ApiStyle::Lp, rs);
        const QString retQual = returnTypeFor(qtRet, ApiStyle::Lp, rs, className + "::");
        const QJsonArray params = o.value("parameters").toArray();

        auto emitParams = [&]() {
            for (int i = 0; i < params.size(); ++i) {
                const QJsonObject p = params.at(i).toObject();
                const QString qtPt = p.value("type").toString();
                const QString pt = paramTypeFor(qtPt, ApiStyle::Lp, rs);
                if (byRefFor(qtPt, pt, ApiStyle::Lp, rs)) s << "const " << pt << "& " << p.value("name").toString();
                else                                     s << pt << " " << p.value("name").toString();
                if (i + 1 < params.size()) s << ", ";
            }
        };
        auto emitArgsArray = [&]() {
            s << "    nlohmann::json _args = nlohmann::json::array();\n";
            for (const QJsonValue& pv : params) {
                const QJsonObject p = pv.toObject();
                s << "    _args.push_back(" << toWireFor(p.value("type").toString(), ApiStyle::Lp, rs, p.value("name").toString()) << ");\n";
            }
        };

        // Sync
        s << retQual << " " << className << "::" << name << "(";
        emitParams();
        if (!params.isEmpty()) s << ", ";
        s << "logos::CallError* err) {\n";
        emitArgsArray();
        if (ret == "void") {
            s << "    " << clientExpr << ".invoke(\"" << name << "\", _args, err);\n";
        } else {
            s << "    nlohmann::json _r = " << clientExpr << ".invoke(\"" << name << "\", _args, err);\n";
            s << "    return " << fromWireFor(qtRet, ApiStyle::Lp, rs, "_r", className + "::") << ";\n";
        }
        s << "}\n\n";

        // Async
        const QString asyncCb = (ret == "void")
            ? QString("std::function<void()>")
            : QString("std::function<void(") + ret + ")>";
        s << "void " << className << "::" << name << "Async(";
        emitParams();
        if (!params.isEmpty()) s << ", ";
        s << asyncCb << " callback) {\n";
        s << "    if (!callback) return;\n";
        emitArgsArray();
        s << "    " << clientExpr << ".invokeAsync(\"" << name << "\", _args, [callback](nlohmann::json _r) {\n";
        if (ret == "void") {
            s << "        (void)_r; callback();\n";
        } else {
            s << "        callback(" << fromWireFor(qtRet, ApiStyle::Lp, rs, "_r", className + "::") << ");\n";
        }
        s << "    });\n";
        s << "}\n\n";
    }
    return c;
}

// ── Umbrella (logos_sdk.h / logos_sdk.cpp) over a module's dependencies ──────

QString makeUmbrellaHeaderFromDeps(const QJsonArray& deps, const QStringList& interfaceNames, ApiStyle apiStyle, const QString& originName)
{
    const QStringList depNames = dependencyNames(deps);

    QString content;
    QTextStream s(&content);

    // Lp (Qt-free) umbrella: no LogosAPI. Each dep wrapper self-creates its
    // lp_client on behalf of `originName` (this module), so the struct is
    // default-constructible and the glue just does `new LogosModules()`.
    if (apiStyle == ApiStyle::Lp) {
        s << "#pragma once\n";
        s << "#include <string>\n";
        if (!interfaceNames.isEmpty()) {
            s << "#include <map>\n";
            s << "#include <memory>\n";
        }
        for (const QString& depName : depNames)
            s << "#include \"" << depName << "_api.h\"\n";
        for (const QString& ifaceName : interfaceNames)
            s << "#include \"" << ifaceName << "_api.h\"\n";
        s << "\n";
        s << "struct LogosModules {\n";
        s << "    LogosModules()";
        bool first = true;
        for (const QString& depName : depNames) {
            s << (first ? " : " : ",\n        ");
            first = false;
            s << depName << "(\"" << originName << "\")";
        }
        s << " {}\n";
        for (const QString& depName : depNames)
            s << "    " << toPascalCase(depName) << " " << depName << ";\n";
        // Interface dependencies: bound at runtime. The bound wrapper is a
        // THIN handle over per-provider State the umbrella OWNS for the
        // module's lifetime — so a transient `modules().bind_x(p)` temporary
        // can register an async callback / event subscription that outlives
        // it (the LpClient + RAII subscriptions persist in the map). Keyed by
        // provider so repeated binds to the same provider share one client.
        for (const QString& ifaceName : interfaceNames) {
            const QString className = toPascalCase(ifaceName);
            s << "    " << className << " bind_" << ifaceName << "(const std::string& moduleName) {\n";
            s << "        auto& _st = m_" << ifaceName << "_bound[moduleName];\n";
            s << "        if (!_st) _st = std::make_unique<" << className << "::State>(moduleName, \"" << originName << "\");\n";
            s << "        return " << className << "(_st.get());\n";
            s << "    }\n";
        }
        for (const QString& ifaceName : interfaceNames) {
            const QString className = toPascalCase(ifaceName);
            s << "    std::map<std::string, std::unique_ptr<" << className << "::State>> m_"
              << ifaceName << "_bound;\n";
        }
        s << "};\n";
        return content;
    }

    // The shape doesn't depend on apiStyle — each dep emits a single
    // `<name>_api.h` whose class signature shape was already decided
    // at codegen time. The umbrella just `#include`s and aggregates
    // each wrapper into the flat `LogosModules` struct.
    //
    // Only the modules explicitly listed in `metadata.json#
    // dependencies` are exposed. Apps that need to manage the core
    // (basecamp, logoscore) use liblogos' C API directly rather than
    // the typed `LogosModules` aggregate.
    //
    // Interface dependencies (`metadata.json#interface_dependencies`) are
    // NOT fixed members — they bind to a runtime-chosen module — so each
    // gets a `bind_<name>(moduleName)` factory instead, returning a bound
    // wrapper by value.
    s << "#pragma once\n";
    // <string> is only needed for the std::string bind_<iface> overloads;
    // omit it when there are no interfaces so the umbrella stays identical
    // to its historical form for dependency-only modules.
    if (!interfaceNames.isEmpty()) s << "#include <string>\n";
    s << "#include \"logos_api.h\"\n";
    s << "#include \"logos_api_client.h\"\n\n";
    for (const QString& depName : depNames)
        s << "#include \"" << depName << "_api.h\"\n";
    for (const QString& ifaceName : interfaceNames)
        s << "#include \"" << ifaceName << "_api.h\"\n";
    s << "\n";

    s << "struct LogosModules {\n";
    s << "    explicit LogosModules(LogosAPI* api) : api(api)";
    for (const QString& depName : depNames)
        s << ", \n        " << depName << "(api)";
    s << " {}\n";
    s << "    LogosAPI* api;\n";
    for (const QString& depName : depNames)
        s << "    " << toPascalCase(depName) << " " << depName << ";\n";
    // Bind factories — one per interface dependency. Two overloads so
    // both Qt-typed (QString) and std-typed (std::string) call sites can
    // pass the runtime module name without converting at the call site.
    for (const QString& ifaceName : interfaceNames) {
        const QString className = toPascalCase(ifaceName);
        s << "    " << className << " bind_" << ifaceName << "(const QString& moduleName) {\n";
        s << "        return " << className << "(api, moduleName);\n";
        s << "    }\n";
        s << "    " << className << " bind_" << ifaceName << "(const std::string& moduleName) {\n";
        s << "        return " << className << "(api, QString::fromStdString(moduleName));\n";
        s << "    }\n";
    }
    s << "};\n";
    return content;
}

QString makeUmbrellaSourceFromDeps(const QJsonArray& deps, const QStringList& interfaceNames)
{
    // Each dep emits one wrapper `.cpp` (Qt or std — decided at codegen time,
    // file name is the same either way), `#include`'d here. Interface wrappers
    // (`<name>_api.cpp`) are #include'd the same way.
    QString content;
    QTextStream s(&content);
    s << "#include \"logos_sdk.h\"\n\n";
    for (const QString& depName : dependencyNames(deps))
        s << "#include \"" << depName << "_api.cpp\"\n";
    for (const QString& ifaceName : interfaceNames)
        s << "#include \"" << ifaceName << "_api.cpp\"\n";
    s << "\n";
    return content;
}
