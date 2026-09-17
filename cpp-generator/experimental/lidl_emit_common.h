// Shared LIDL-type -> target-type-name mapping used by the C++/Qt code-emitting
// backends. These are the language-specific half of codegen (the canonical
// frontend — lexer/parser/AST/serializer/validator — lives in logos-lidl); each
// SDK keeps its own type-name mapping (Qt vs std vs Rust).
#pragma once

#include <QString>
#include <functional>

#include "lidl_compat.h"

QString lidlToPascalCase(const QString& name);
QString lidlTypeToQt(const TypeExpr& te);

// The same table, with a hook on how a `Named` (record) type is spelled.
//
// A generated wrapper nests its record structs in the wrapper class
// (`InfoModule::Point`, so two dependencies may both declare a `Point`), so a
// type written OUTSIDE that class scope — a return type before the `Class::` of
// a definition — has to qualify them. Before the table produced composites, the
// only shapes that could mention a record were `Point`, `QList<Point>` and
// `QMap<QString, Point>`, and each emitter matched those three by hand on the
// finished string. That stops working the moment `?Point`,
// `QList<QList<Point>>` or `QMap<QString, ?Point>` are spellable.
//
// So the qualification happens DURING the walk, at the one place that knows a
// name is a record, and there is still one table: lidlTypeToQt(te) is exactly
// this with an identity hook.
QString lidlTypeToQt(const TypeExpr& te,
                     const std::function<QString(const QString&)>& recordName);
QString lidlTypeToStd(const TypeExpr& te);
bool lidlIsStdConvertible(const TypeExpr& te);

// The LIDL CONTRACT spelling of a type — `tstr`, `[Point]`, `{tstr: uint}`,
// `? tstr`. Mirrors logos-lidl's `serializeTypeExpr` (src/serializer.cpp) byte
// for byte, so a type name published in getMethods() is the same text the
// module's own `.lidl` artifact carries for that slot.
//
// It is a COPY, and that is a defect this cannot fix from here:
// `serializeTypeExpr` is file-local to logos-lidl's serializer.cpp and the
// public headers expose no type-expression printer, so there is nothing to
// delegate to. The pairing is asserted instead — tests/experimental
// round-trips each shape through `lidl::serialize` and compares — so the two
// cannot drift silently. When logos-lidl exports a `typeToString`, delete this
// and call it.
QString lidlTypeToLidlText(const TypeExpr& te);

// True when the Qt spelling of `te` is one of the QVariant-family names —
// QVariant / QVariantList / QVariantMap — i.e. when the type's scalar leaf is
// `any` (or an unrecognised primitive). Those slots keep the untyped spelling
// because QVariant is the only Qt type that holds bytes AND an exact uint64 AND
// arbitrary nesting all at once, so widening them would LOSE information.
bool lidlQtBottomsOutAtAny(const TypeExpr& te);

// True when the Qt spelling of `te` is a TYPED container or a std::optional —
// `QList<T>`, `QMap<QString, T>`, `std::optional<T>` — and therefore must be
// encoded and decoded by a generator-emitted ELEMENT LOOP.
//
// THIS IS THE ONE PREDICATE THAT KEEPS THE WIDENED TABLE FROM DESTROYING DATA.
// logos-protocol's qvariantToNlohmann matches a CLOSED userType() set
// (QByteArray, LogosResult, the integer types, QStringList, QVariantList,
// QVariantMap, the QJson types); `QList<qulonglong>` matches NONE of them and
// serialises to JSON null. The other direction fails just as quietly:
// `qvariant_cast<QList<qulonglong>>` of a QVariantList yields an EMPTY list.
// Neither direction warns. So a name this returns true for must never be
// handed to logos::qt::toWire / fromWire<T> (or to QVariant::fromValue /
// qvariant_cast) as a whole value — only its ELEMENTS may cross, one at a time.
//
// QStringList is deliberately NOT one of these: it is in the closed set, so it
// crosses whole and always did.
bool lidlQtNeedsElementLoop(const TypeExpr& te);
