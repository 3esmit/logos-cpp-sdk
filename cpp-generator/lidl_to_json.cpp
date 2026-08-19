#include "lidl_to_json.h"

#include <QJsonObject>
#include <QStringList>

#include "experimental/lidl_emit_common.h"   // lidlTypeToQt — the one Qt type mapper

// Convert a TypeExpr → Qt-typed string name (same surface the
// metaobject-introspection path produces for methods, so generator_lib
// can consume both via one code path).
//
// ONE Qt type mapper. This used to be a near-duplicate of `lidlTypeToQt`
// (experimental/lidl_emit_common.cpp) and the two disagreed: this copy had no
// `void` case, so a `-> void` method reaching it as Primitive("void") from the
// impl-header parser fell through to QVariant and generated
// `QVariant doVoid(...)`. (The .lidl parser spells the same thing
// Named("void"), which survived only by accident — mapReturnType's
// `base == "void"` early-out.) The lp/std tables are DERIVED from this name, so
// the same bug produced `LogosMap doVoid(...)` on the Qt-free surface: not a
// Qt-only defect, a front-end one. It is now a delegation, so there is one
// table to disagree with.
QString lidlTypeExprToQtTypeName(const TypeExpr& te)
{
    return lidlTypeToQt(te);
}

// Report every optional slot this path still flattens into a bare type name.
//
// A record FIELD no longer does: moduleRecordsToJson below carries `optional`
// alongside the value type, and the emitter reconstitutes it (QVariant on the
// Qt surface, std::optional<T> on the Lp one). What is still flattened is every
// POSITIONAL slot — a method parameter, a return type, an event parameter.
// Those have no name to hang a flag on, so they only ever had the type-kind
// spelling and there is no spelling divergence to fix; what they lose is the
// value TYPE, exactly as `lidlTypeToQt` documents (`?T` -> QVariant, and via
// the derived std table -> LogosMap). Two-stateness survives — an invalid
// QVariant / a JSON null is the empty inhabitant — but the consumer gets no
// compile-time check on the value and cannot tell `?tstr` from `?uint`.
//
// Widening those means changing the generated method SIGNATURES, which is a
// source break for every existing caller and buys nothing for the
// one-declaration-two-spellings rule. So they stay flattened, and say so.
void noteOptionalPositionalSlots(const ModuleDecl& mod, const QString& where,
                                 QTextStream& err)
{
    QStringList optSlots;
    for (const MethodDecl& md : mod.methods) {
        for (const ParamDecl& pd : md.params)
            if (paramIsOptional(pd))
                optSlots << (qs(md.name) + "(" + qs(pd.name) + ")");
        if (typeIsOptional(md.returnType))
            optSlots << (qs(md.name) + "() return");
    }
    for (const EventDecl& ed : mod.events)
        for (const ParamDecl& pd : ed.params)
            if (paramIsOptional(pd))
                optSlots << (qs(ed.name) + "(" + qs(pd.name) + ")");
    if (optSlots.isEmpty()) return;
    err << "Note: " << where << ": optional positional slot(s) ["
        << optSlots.join(", ")
        << "] are generated as untyped QVariant (LogosMap on the lp surface). A "
           "positional slot has no name to carry an optional flag, so `?T` keeps "
           "its two states (an invalid QVariant / a JSON null is the empty one) "
           "but loses T. Record fields are unaffected — they carry optionality "
           "through.\n";
}

// Build a getMethods()-shaped QJsonArray (the surface makeHeader/makeSource
// consume) from a parsed ModuleDecl. Every interface method is invokable.
QJsonArray moduleMethodsToJson(const ModuleDecl& mod)
{
    QJsonArray arr;
    for (const MethodDecl& m : mod.methods) {
        QJsonObject o;
        o["name"] = qs(m.name);
        o["returnType"] = lidlTypeExprToQtTypeName(m.returnType);
        o["isInvokable"] = true;
        QJsonArray params;
        for (const ParamDecl& p : m.params) {
            QJsonObject po;
            po["type"] = lidlTypeExprToQtTypeName(p.type);
            po["name"] = qs(p.name);
            params.append(po);
        }
        o["parameters"] = params;
        arr.append(o);
    }
    return arr;
}

// Build the records QJsonArray ({ name, fields:[{name,type,optional}] }) from a
// parsed ModuleDecl — the contract's `type Foo { ... }` declarations, which
// generator_lib turns into structs nested in the wrapper class.
//
// OPTIONALITY SURVIVES HERE, and it is the whole reason this object has three
// keys instead of two. A field has two equivalent spellings — the flag
// (`? name: T`) and the type kind (`name: ?T`) — which logos-lidl's docs/spec.md
// binds to ONE meaning and requires to produce byte-identical code. Flattening
// `fd.type` into a name answered that question two different ways from one
// contract: the flag spelling kept T (so `? maybe: tstr` became a bare `QString`
// that cannot be empty at all, silently defaulting), the type spelling collapsed
// to QVariant. Both answers came from reading the verbatim spelling instead of
// asking.
//
// So: `type` is the value type with optionality stripped (fieldValueType), and
// `optional` is true for either spelling (fieldIsOptional). Those accessors are
// the frontend's, and are the ONLY correct source — `fd.optional` alone and
// `fd.type.kind == Optional` alone are the same bug from opposite sides.
QJsonArray moduleRecordsToJson(const ModuleDecl& mod)
{
    QJsonArray arr;
    for (const TypeDecl& td : mod.types) {
        QJsonObject o;
        o["name"] = qs(td.name);
        QJsonArray fields;
        for (const FieldDecl& fd : td.fields) {
            QJsonObject f;
            f["name"] = qs(fd.name);
            f["type"] = lidlTypeExprToQtTypeName(fieldValueType(fd));
            f["optional"] = fieldIsOptional(fd);
            fields.append(f);
        }
        o["fields"] = fields;
        arr.append(o);
    }
    return arr;
}

// Build the events QJsonArray ({ name, params:[{name,type}] }) — same shape
// loadEventsFromLidl produces — from a parsed ModuleDecl.
QJsonArray moduleEventsToJson(const ModuleDecl& mod)
{
    QJsonArray arr;
    for (const EventDecl& ed : mod.events) {
        QJsonObject o;
        o["name"] = qs(ed.name);
        QJsonArray params;
        for (const ParamDecl& pd : ed.params) {
            QJsonObject p;
            p["name"] = qs(pd.name);
            p["type"] = lidlTypeExprToQtTypeName(pd.type);
            params.append(p);
        }
        o["params"] = params;
        arr.append(o);
    }
    return arr;
}
