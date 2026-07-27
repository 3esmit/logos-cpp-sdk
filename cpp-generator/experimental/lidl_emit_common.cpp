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

QString lidlTypeToQt(const TypeExpr& te)
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
        if (te.name == "any")     return "QVariant";
        return "QVariant";
    case TypeExpr::Named:
        // A record declared by the contract: its generated struct. One LIDL
        // type, one type per language — a record is not a QVariant blob.
        return QString::fromStdString(te.name);
    case TypeExpr::Array:
        if (te.elements.size() == 1
            && te.elements[0].kind == TypeExpr::Primitive
            && te.elements[0].name == "tstr") {
            return "QStringList";
        }
        // A list of records is a typed list: QVariantList could not hold a
        // record without Q_DECLARE_METATYPE, and the point of a record is that
        // the consumer gets the struct.
        if (te.elements.size() == 1 && te.elements[0].kind == TypeExpr::Named)
            return "QList<" + QString::fromStdString(te.elements[0].name) + ">";
        return "QVariantList";
    case TypeExpr::Map:
        if (te.elements.size() == 2 && te.elements[1].kind == TypeExpr::Named)
            return "QMap<QString, " + QString::fromStdString(te.elements[1].name) + ">";
        return "QVariantMap";
    case TypeExpr::Optional:
        return "QVariant";
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
    if (te.kind == TypeExpr::Optional) return "QVariant";
    if (te.kind == TypeExpr::Named)    return "QVariant";
    return "QVariant";
}
