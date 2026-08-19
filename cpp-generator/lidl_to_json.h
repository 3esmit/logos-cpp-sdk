#ifndef LIDL_TO_JSON_H
#define LIDL_TO_JSON_H

// THE BOUNDARY between the LIDL frontend and the legacy consumer emitter.
//
// generator_lib (makeHeader / makeSource / makeHeaderLp / makeSourceLp)
// consumes a JSON surface, not an AST — because the same emitter also serves
// the metaobject-introspection path, which has only Qt type NAMES to offer.
// These three functions are the only place a `ModuleDecl` is turned into that
// surface, so they are the only place a property of a TypeExpr can be lost.
//
// They live in their own translation unit rather than inside `main.cpp` so the
// conversion is linkable, and therefore testable: the rule that the two LIDL
// optionality spellings must emit byte-identical code is a property of
// (frontend -> JSON -> emitter) end to end, and cannot be asserted on either
// half alone.

#include <QJsonArray>
#include <QString>
#include <QTextStream>

#include "experimental/lidl_compat.h"

// A TypeExpr -> the Qt type NAME the emitter keys off. One Qt type mapper:
// this delegates to `lidlTypeToQt` (experimental/lidl_emit_common.cpp) rather
// than keeping a second table.
QString lidlTypeExprToQtTypeName(const TypeExpr& te);

// getMethods()-shaped: [ { name, returnType, isInvokable, parameters:[{name,type}] } ]
QJsonArray moduleMethodsToJson(const ModuleDecl& mod);

// [ { name, fields: [ { name, type, optional } ] } ]
//
// `type` is the Qt name of the field's VALUE type (optionality stripped) and
// `optional` is the answer for BOTH spellings — `? name: T` and `name: ?T`
// produce the identical object here, which is what makes the emitted code
// identical. Read via fieldIsOptional()/fieldValueType(); never re-derived.
QJsonArray moduleRecordsToJson(const ModuleDecl& mod);

// [ { name, params: [ { name, type } ] } ]
QJsonArray moduleEventsToJson(const ModuleDecl& mod);

// Report the optional slots this path still flattens — the POSITIONAL ones
// (method parameters, return types, event parameters). Record fields are no
// longer among them: they carry `optional` through the JSON above.
void noteOptionalPositionalSlots(const ModuleDecl& mod, const QString& where,
                                 QTextStream& err);

#endif // LIDL_TO_JSON_H
