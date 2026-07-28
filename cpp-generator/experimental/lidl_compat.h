#ifndef LIDL_COMPAT_H
#define LIDL_COMPAT_H

// Bridge the cpp-generator's Qt-flavored codegen backends onto the canonical
// logos-lidl frontend. The lexer / parser / AST / serializer / validator now
// live in logos-lidl (std-typed, language-neutral); this header brings those
// types into the global scope the backends use unqualified and adds thin
// Qt-friendly shims so the existing emission code (QTextStream) keeps
// compiling. The backends themselves (impl-header parsing, gen_client,
// gen_cdylib) stay here — they are the C++/Qt-specific parts.

#include "lidl/ast.hpp"
#include "lidl/parser.hpp"
#include "lidl/serializer.hpp"
#include "lidl/validator.hpp"

#include <QString>
#include <QTextStream>
#include <string>

// The canonical AST, in the global scope the generator backends reference it
// from (they predate the logos-lidl extraction and use the unqualified names).
using lidl::TypeExpr;
using lidl::FieldDecl;
using lidl::ParamDecl;
using lidl::MethodDecl;
using lidl::EventDecl;
using lidl::TypeDecl;
using lidl::ModuleDecl;

// std::string -> QString, and let QTextStream accept std::string directly so
// emission of AST string fields (`s << md.name`) keeps compiling unchanged.
inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline QTextStream& operator<<(QTextStream& s, const std::string& v)
{
    return s << QString::fromStdString(v);
}

// Name-compatible shims over the canonical frontend so the call sites that used
// the deleted experimental lexer/parser/serializer/validator keep their shape.
using LidlParseResult = lidl::ParseResult;
using LidlValidationResult = lidl::ValidationResult;

inline lidl::ParseResult lidlParse(const QString& source)
{
    return lidl::parse(source.toStdString());
}
inline QString lidlSerialize(const ModuleDecl& module)
{
    return QString::fromStdString(lidl::serialize(module));
}
inline lidl::ValidationResult lidlValidate(const ModuleDecl& module)
{
    return lidl::validate(module);
}

// A record whose ONLY field is a `tstr` named `_bytes` is indistinguishable on
// the wire from a canonical tagged byte string: `isTaggedBytes()` is checked
// BEFORE `is_object()` in both logos_codec.h and logos_json_convert.cpp, so
// such a record silently decodes as a byte string and the struct is gone. The
// ambiguity is inherent to the tagged form — the codec's own comment says not
// to name a map key `_bytes` — but a generator can at least refuse to emit the
// one shape that is guaranteed to misdecode, instead of leaving it to be
// discovered at runtime.
inline bool lidlRecordCollidesWithBytesTag(const TypeDecl& t)
{
    return t.fields.size() == 1
        && t.fields[0].name == "_bytes"
        && t.fields[0].type.kind == TypeExpr::Primitive
        && t.fields[0].type.name == "tstr";
}

// Returns false and fills `error` when any declared record cannot round-trip.
inline bool lidlCheckRecords(const ModuleDecl& m, QString* error)
{
    for (const TypeDecl& t : m.types) {
        if (lidlRecordCollidesWithBytesTag(t)) {
            if (error)
                *error = QString("type '%1': a record whose only field is a tstr named "
                                 "'_bytes' is wire-identical to a tagged byte string and "
                                 "would decode as bytes, not as the record. Rename the "
                                 "field or give the record another field.")
                             .arg(qs(t.name));
            return false;
        }
    }
    return true;
}

#endif // LIDL_COMPAT_H
