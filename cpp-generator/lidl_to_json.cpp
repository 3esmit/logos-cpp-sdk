#include "lidl_to_json.h"

#include <QJsonObject>
#include <QStringList>

#include "experimental/lidl_emit_common.h"   // lidlTypeToQt — the one Qt type mapper

// Convert a TypeExpr → Qt-typed string name (same surface the
// metaobject-introspection path produces for methods, so generator_lib
// can consume both via one code path).
//
// ONE Qt type mapper. No-return is handled at the MethodDecl level before this
// function is called; a TypeExpr always denotes an actual value type.
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
// spelling and there is no spelling divergence to fix.
//
// `lidlTypeToQt` DOES now answer `?T` with std::optional<T>. This path cannot
// keep it: generator_lib is keyed on flat type NAMES and folds every widened
// spelling back (legacyQtBase), because encoding one correctly needs an element
// loop it has no tree to derive. So the loss is this emitter's, not the
// mapping's — the TypeExpr-driven Qt consumer emitters keep the value type —
// and the note says which surface is affected rather than claiming the table
// still flattens.
void noteOptionalPositionalSlots(const ModuleDecl& mod, const QString& where,
                                 QTextStream& err)
{
    QStringList optSlots;
    for (const MethodDecl& md : mod.methods) {
        for (const ParamDecl& pd : md.params)
            if (paramIsOptional(pd))
                optSlots << (qs(md.name) + "(" + qs(pd.name) + ")");
        if (md.returnType && typeIsOptional(*md.returnType))
            optSlots << (qs(md.name) + "() return");
    }
    for (const EventDecl& ed : mod.events)
        for (const ParamDecl& pd : ed.params)
            if (paramIsOptional(pd))
                optSlots << (qs(ed.name) + "(" + qs(pd.name) + ")");
    if (optSlots.isEmpty()) return;
    err << "Note: " << where << ": optional positional slot(s) ["
        << optSlots.join(", ")
        << "] are generated as untyped QVariant (LogosMap on the lp surface) by "
           "THIS emitter, which is keyed on flat type names and folds "
           "std::optional<T> back to QVariant. `?T` keeps its two states (an "
           "invalid QVariant / a JSON null is the empty one) but loses T here. "
           "The TypeExpr-driven Qt consumer emitters keep it as "
           "std::optional<T>; record fields are unaffected on every surface — "
           "they carry optionality through.\n";
}

// Build a getMethods()-shaped QJsonArray (the surface makeHeader/makeSource
// consume) from a parsed ModuleDecl. Every interface method is invokable.
QJsonArray moduleMethodsToJson(const ModuleDecl& mod)
{
    QJsonArray arr;
    for (const MethodDecl& m : mod.methods) {
        QJsonObject o;
        o["name"] = qs(m.name);
        o["returnType"] = m.returnType
            ? lidlTypeExprToQtTypeName(*m.returnType)
            : QStringLiteral("void");
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
