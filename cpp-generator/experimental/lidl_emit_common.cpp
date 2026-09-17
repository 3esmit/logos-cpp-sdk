#include "lidl_emit_common.h"

QString lidlToPascalCase(const QString& name)
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

// A type "bottoms out at `any`" when its scalar LEAF is `any` — or an
// unrecognised primitive, which this table has always spelled QVariant too.
// Optionality and container nesting are transparent to the question:
// `[[any]]`, `{tstr: [any]}` and `?any` all bottom out at `any`.
bool lidlQtBottomsOutAtAny(const TypeExpr& te)
{
    switch (te.kind) {
    case TypeExpr::Primitive:
        return !(te.name == "void" || te.name == "tstr" || te.name == "bstr"
              || te.name == "int"  || te.name == "uint" || te.name == "float64"
              || te.name == "bool" || te.name == "result");
    case TypeExpr::Named:
        // A record declared by the contract: a real struct, never a blob.
        return false;
    case TypeExpr::Array:
        // A degenerate Array carrying no element (unreachable from the parser,
        // constructible by hand or over the JSON bridge) keeps the opaque
        // spelling rather than being described as typed.
        return te.elements.size() != 1 || lidlQtBottomsOutAtAny(te.elements[0]);
    case TypeExpr::Map:
        return te.elements.size() != 2 || lidlQtBottomsOutAtAny(te.elements[1]);
    case TypeExpr::Optional:
        // Through optionalValueType(), so `??T` answers for T — optionality is
        // idempotent under the two-state rule.
        return te.elements.empty() || lidlQtBottomsOutAtAny(optionalValueType(te));
    }
    return true;
}

bool lidlQtNeedsElementLoop(const TypeExpr& te)
{
    if (lidlQtBottomsOutAtAny(te)) return false;   // QVariant / List / Map
    switch (te.kind) {
    case TypeExpr::Array:
        // `[tstr]` is QStringList, which crosses whole (QMetaType::QStringList
        // is in qvariantToNlohmann's closed set). Every other typed array is
        // QList<T>, which is not.
        return !(te.elements[0].kind == TypeExpr::Primitive
                 && te.elements[0].name == "tstr");
    case TypeExpr::Map:
    case TypeExpr::Optional:
        return true;
    case TypeExpr::Primitive:
    case TypeExpr::Named:
        return false;
    }
    return false;
}

// The LIDL contract spelling. Mirrors logos-lidl's serializeTypeExpr; see the
// header for why it is a copy and what pins it.
QString lidlTypeToLidlText(const TypeExpr& te)
{
    switch (te.kind) {
    case TypeExpr::Primitive:
    case TypeExpr::Named:
        return QString::fromStdString(te.name);
    case TypeExpr::Array:
        if (te.elements.size() != 1) return QStringLiteral("any");
        return "[" + lidlTypeToLidlText(te.elements[0]) + "]";
    case TypeExpr::Map:
        if (te.elements.size() != 2) return QStringLiteral("any");
        return "{" + lidlTypeToLidlText(te.elements[0]) + ": "
             + lidlTypeToLidlText(te.elements[1]) + "}";
    case TypeExpr::Optional:
        if (te.elements.empty()) return QStringLiteral("any");
        return "? " + lidlTypeToLidlText(te.elements[0]);
    }
    return QStringLiteral("any");
}

QString lidlTypeToQt(const TypeExpr& te)
{
    return lidlTypeToQt(te, [](const QString& n) { return n; });
}

QString lidlTypeToQt(const TypeExpr& te,
                     const std::function<QString(const QString&)>& recordName)
{
    switch (te.kind) {
    case TypeExpr::Primitive:
        if (te.name == "void")    return "void";
        if (te.name == "tstr")    return "QString";
        if (te.name == "bstr")    return "QByteArray";
        // 64-bit, and unsigned stays unsigned. LIDL int/uint are int64_t/uint64_t
        // everywhere else (C++ impls, Rust's i64/u64), so spelling them `int`
        // here broke the 1-1 mapping and truncated: a Qt consumer reading a
        // `uint` return got a SIGNED 32-bit value. qlonglong/qulonglong rather
        // than qint64/quint64 so the generated introspection matches the names
        // Qt's own metaobject normalisation produces.
        if (te.name == "int")     return "qlonglong";
        if (te.name == "uint")    return "qulonglong";
        if (te.name == "float64") return "double";
        if (te.name == "bool")    return "bool";
        if (te.name == "result")  return "LogosResult";
        // `any` — KEPT untyped, and it is the only row here that is. QVariant is
        // the sole Qt type that carries bytes AND an exact uint64 AND arbitrary
        // nesting, so narrowing it would lose what it was chosen to hold.
        if (te.name == "any")     return "QVariant";
        return "QVariant";
    case TypeExpr::Named:
        // A record declared by the contract: its generated struct. One LIDL
        // type, one type per language — a record is not a QVariant blob.
        return recordName(QString::fromStdString(te.name));
    case TypeExpr::Array:
        // `[any]` (and anything else whose leaf is `any`) keeps QVariantList:
        // there is no narrower Qt list that can hold those elements.
        if (lidlQtBottomsOutAtAny(te)) return "QVariantList";
        // `[tstr]` is QStringList — the one typed array Qt has a native
        // spelling for, and the one this table already produced.
        if (te.elements[0].kind == TypeExpr::Primitive
            && te.elements[0].name == "tstr") {
            return "QStringList";
        }
        // Every other `[T]` — including a list of records, which could not ride
        // a QVariantList without Q_DECLARE_METATYPE — is the typed list. The
        // element spelling is this same table applied recursively, so
        // `[[uint]]` is QList<QList<qulonglong>> and `[?tstr]` is
        // QList<std::optional<QString>>.
        return "QList<" + lidlTypeToQt(te.elements[0], recordName) + ">";
    case TypeExpr::Map:
        if (lidlQtBottomsOutAtAny(te)) return "QVariantMap";
        // The key is spelled QString unconditionally, as it always has been: a
        // JSON object key IS a string, so a contract that writes a non-tstr key
        // does not change what crosses the wire.
        return "QMap<QString, " + lidlTypeToQt(te.elements[1], recordName) + ">";
    case TypeExpr::Optional:
        // `?T` -> std::optional<T>. This row used to be a bare QVariant and was
        // the ONE mapping in this table that lost the value type: a Qt consumer
        // could not tell `?tstr` from `?uint`, while the std surface next door
        // kept both through std::optional.
        //
        // The objection that kept it QVariant was that the name is read as a
        // METATYPE — the legacy consumer path and getMethods() introspection
        // both handed it to the host to marshal, and there is no metatype called
        // `std::optional<QString>`. Both halves of that are now false:
        // getMethods() publishes the LIDL spelling (lidlTypeToQtWire), and the
        // string-keyed legacy emitter folds every widened spelling back to the
        // name it used before (legacyQtBase in generator_lib.cpp). What is left
        // reading this row is the TypeExpr-driven Qt emitters, which emit
        // element loops rather than a metatype lookup.
        //
        // Recursed through optionalValueType() rather than elements[0], because
        // optionality is idempotent under the two-state rule: `??T` denotes the
        // same two states as `?T` and must not become
        // std::optional<std::optional<T>>. A degenerate Optional carrying no
        // element keeps the opaque fallback instead of recursing forever
        // (lidlQtBottomsOutAtAny answers true for it).
        if (lidlQtBottomsOutAtAny(te)) return "QVariant";
        return "std::optional<" + lidlTypeToQt(optionalValueType(te), recordName) + ">";
    }
    return "QVariant";
}

bool lidlIsStdConvertible(const TypeExpr& te)
{
    if (te.kind == TypeExpr::Primitive) {
        return te.name == "tstr" || te.name == "bstr"
            || te.name == "int" || te.name == "uint"
            || te.name == "float64" || te.name == "bool";
    }
    if (te.kind == TypeExpr::Array && te.elements.size() == 1) {
        const TypeExpr& elem = te.elements[0];
        if (elem.kind == TypeExpr::Primitive) {
            return elem.name == "tstr" || elem.name == "bstr"
                || elem.name == "int" || elem.name == "uint"
                || elem.name == "float64" || elem.name == "bool";
        }
    }
    return false;
}

QString lidlTypeToStd(const TypeExpr& te)
{
    if (te.kind == TypeExpr::Primitive) {
        if (te.name == "tstr")    return "std::string";
        if (te.name == "bstr")    return "std::vector<uint8_t>";
        if (te.name == "int")     return "int64_t";
        if (te.name == "uint")    return "uint64_t";
        if (te.name == "float64") return "double";
        if (te.name == "bool")    return "bool";
        if (te.name == "result")  return "LogosResult";
        if (te.name == "any")     return "QVariant";
        return "QVariant";
    }
    if (te.kind == TypeExpr::Array && te.elements.size() == 1) {
        const TypeExpr& elem = te.elements[0];
        if (elem.kind == TypeExpr::Primitive) {
            if (elem.name == "tstr")    return "std::vector<std::string>";
            if (elem.name == "bstr")    return "std::vector<std::vector<uint8_t>>";
            if (elem.name == "int")     return "std::vector<int64_t>";
            if (elem.name == "uint")    return "std::vector<uint64_t>";
            if (elem.name == "float64") return "std::vector<double>";
            if (elem.name == "bool")    return "std::vector<bool>";
        }
        return "QVariantList";
    }
    if (te.kind == TypeExpr::Map)      return "QVariantMap";
    // `?T` -> std::optional<T>. The std surface HAS an optional, so unlike the
    // Qt table above this one keeps the value type. std::nullopt is C++'s single
    // empty inhabitant, which is what makes the mapping two-state; the encoder
    // that pairs with it is logos-protocol's Codec<std::optional<T>>.
    //
    // Recurse through optionalValueType() rather than elements[0]: optionality
    // is idempotent under the two-state rule, so `??T` denotes the same two
    // states as `?T` and must not become std::optional<std::optional<T>>.
    // A degenerate Optional carrying no element (unreachable from the parser,
    // constructible by hand or over the JSON bridge) keeps the opaque fallback
    // instead of recursing forever.
    if (te.kind == TypeExpr::Optional) {
        if (te.elements.empty()) return "QVariant";
        return "std::optional<" + lidlTypeToStd(optionalValueType(te)) + ">";
    }
    if (te.kind == TypeExpr::Named)    return "QVariant";
    return "QVariant";
}
