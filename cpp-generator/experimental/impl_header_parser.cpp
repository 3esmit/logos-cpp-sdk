#include "impl_header_parser.h"

#include "metadata_dependencies.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <functional>
#include <set>
#include <QStringList>

// ---------------------------------------------------------------------------
// Strip leading declaration specifiers / attributes from a return-type string.
// ---------------------------------------------------------------------------

static QString stripDeclarationSpecifiers(QString string)
{
    static const QRegularExpression attributeRe("\\[\\[[^\\]]*\\]\\]");
    static const QRegularExpression specifierRe(
        "^(static|virtual|inline|explicit|constexpr|consteval|friend)\\s+");

    string.remove(attributeRe);
    string = string.trimmed();

    QRegularExpressionMatch specifierMatch = specifierRe.match(string);
    while (specifierMatch.hasMatch()) {
        string = string.mid(specifierMatch.capturedLength()).trimmed();
        specifierMatch = specifierRe.match(string);
    }

    return string;
}

// ---------------------------------------------------------------------------
// C++ type string → LIDL TypeExpr
// ---------------------------------------------------------------------------

// The records the header declares, discovered by scanForRecords() before any
// method is parsed. A bare `Blob` in a signature is only a record if the header
// actually declared `struct Blob { ... };` — otherwise it stays the opaque
// `any` it always was.
static QSet<QString> g_recordNames;

// C++ spellings seen that have NO LIDL type, and were mapped onto the nearest
// one that does. Collected here because cppTypeToLidl has no diagnostic channel
// (same reason g_recordNames is file-static); parseImplHeader drains it.
static QStringList g_unmappableSpellings;

// ---------------------------------------------------------------------------
// Spellings with NO LIDL type at all.
//
// These used to reach the `any` fallback at the bottom of cppTypeToLidl, and
// `any` is ADMITTED by every backend gate — so the declaration was accepted, the
// published contract said `any`, and the generated dispatch handed the raw
// nlohmann::json straight to the author's parameter. That either worked by luck
// through nlohmann's implicit conversions, threw at call time, or emitted a
// non-canonical wire value. Nothing said a word.
//
// Now every one of them is recorded here and parseImplHeader turns the list into
// a hard parse error naming the offending C++ type and the fix.
//
// cppTypeToLidl still RETURNS the historical `any` for these: the diagnostic and
// the mapping are separate, so a declaration whose diagnostic is later discarded
// (a helper struct that never reaches the contract, a reserved lifecycle hook)
// produces byte-identical output to before.
struct UnsupportedSpelling {
    QString context;    // "method 'foo': parameter 'bar'"
    QString record;     // non-empty when this came from a record field
    QString declared;   // the full spelling as written on the declaration
    QString offending;  // the spelling that has no LIDL type (may be nested)
    QString hint;       // what to write instead
};
static QList<UnsupportedSpelling> g_unsupported;

// ---------------------------------------------------------------------------
// STRUCTURE the record scanner could not read.
//
// #127 closed the hole where an unknown TYPE was admitted as `any`. This is the
// same hole one level down: a line inside a `struct` body that the scanner
// could not read as a field was `continue`d, and the struct was published
// anyway — MINUS that field. A field list is not a detail of a record, it IS
// the record: the promise every other language binds to. Publishing a shorter
// one silently ships a contract that disagrees with the header, and nothing
// downstream can tell, because a contract with three fields and a contract with
// two are both perfectly well-formed.
//
// Same discipline as g_unsupported: collected here, withdrawn when the struct
// turns out never to reach the contract (an internal helper promises nothing),
// and a hard parse error otherwise.
struct UnreadableDecl {
    QString record;   // the struct it was found in — the withdrawal key
    QString text;     // the declaration as written, comments removed
    QString hint;     // what to write instead
};
static QList<UnreadableDecl> g_unreadable;

// Structs that were opened but published no field at all. Referenced by the API
// they are still a defect — the emitted contract names a `type` it never
// declares — but no single line is at fault, so they are reported separately.
static QStringList g_emptyRecords;

// Drop the qualifiers that are about how a value is PASSED rather than what it
// is: cppTypeToLidl normalizes with this, and the diagnostics compare against it
// so `const nlohmann::json&` and `nlohmann::json` are recognised as the same
// spelling instead of reading as a type nested inside itself.
static QString normalizeCppSpelling(const QString& raw)
{
    QString t = raw.trimmed();
    t.remove(QRegularExpression("^const\\s+"));
    t.remove(QRegularExpression("\\s*&$"));
    return t.trimmed();
}

// Collapse whitespace so `unsigned  long   int` and `unsigned long int` are one
// key, and drop the redundant `int` from the multi-word integer spellings.
static QString normalizeNumericSpelling(QString t)
{
    t = t.simplified();
    static const QRegularExpression trailingInt("\\s+int$");
    if (t != "int" && t.contains(' '))
        t.remove(trailingInt);
    return t;
}

// What to write instead. Every hint names a spelling that IS in the contract,
// because "unsupported" without a replacement just moves the guesswork.
static QString unsupportedHint(const QString& t)
{
    static const QString kWidenNote =
        "Widening is source-compatible for every caller; a narrow type on the "
        "wire is not, which is why LIDL has none.";

    // uint8_t has exactly ONE meaning in this contract and it is not a number.
    if (t == "uint8_t" || t == "std::uint8_t")
        return "uint8_t means BYTES here, and only as `std::vector<uint8_t>` "
               "(LIDL `bstr`). For a small number declare `uint64_t` (LIDL "
               "`uint`); for binary data declare `std::vector<uint8_t>`.";

    const QString n = normalizeNumericSpelling(t);
    static const QSet<QString> kUnsigned = {
        "unsigned", "unsigned char", "unsigned short", "unsigned long",
        "unsigned long long", "uint16_t", "uint32_t", "size_t",
        "uintptr_t", "uintmax_t",
        "std::uint16_t", "std::uint32_t", "std::size_t", "std::uintptr_t"
    };
    static const QSet<QString> kSigned = {
        "char", "signed char", "signed", "short", "int", "long", "long long",
        "int8_t", "int16_t", "int32_t", "ssize_t", "ptrdiff_t", "intptr_t",
        "intmax_t", "std::int8_t", "std::int16_t", "std::int32_t",
        "std::ptrdiff_t", "std::intptr_t"
    };
    static const QSet<QString> kFloating = { "float", "long double" };

    if (kUnsigned.contains(n))
        return "LIDL numbers are 64-bit only. Declare it `uint64_t` (LIDL "
               "`uint`). " + kWidenNote;
    if (kSigned.contains(n))
        return "LIDL numbers are 64-bit only. Declare it `int64_t` (LIDL "
               "`int`). " + kWidenNote;
    if (kFloating.contains(n))
        return "LIDL has one floating type, `float64`. Declare it `double`.";

    if (t.startsWith("std::set<") || t.startsWith("std::unordered_set<")
        || t.startsWith("std::multiset<"))
        return "LIDL has no set type. Declare it `std::vector<T>` (LIDL `[T]`); "
               "uniqueness is not carried on the wire, so the module has to "
               "enforce it either way.";
    if (t.startsWith("std::pair<") || t.startsWith("std::tuple<"))
        return "LIDL has no pair or tuple. Declare a `struct` in this header — "
               "it becomes a contract `type` with named fields — or, for "
               "key/value data, `std::map<std::string, V>` (LIDL `{tstr: V}`). "
               "A struct is usually the right answer: positional pairs have no "
               "field names for a consumer in another language to bind to.";
    if (t.startsWith("std::map<") || t.startsWith("std::unordered_map<")
        || t.startsWith("std::multimap<"))
        return "LIDL map keys are always `tstr`. Declare it "
               "`std::map<std::string, V>` / `std::unordered_map<std::string, "
               "V>`, or a `[T]` of a struct carrying the key as a field.";
    if (t.startsWith("std::list<") || t.startsWith("std::deque<")
        || t.startsWith("std::array<") || t.startsWith("std::forward_list<"))
        return "LIDL's sequence type is `[T]`, spelled `std::vector<T>`. "
               "Declare it that way.";
    if (t.startsWith("Q"))
        return "Qt types cannot appear in a universal impl header — the "
               "module's own translation units are Qt-free, and Qt is confined "
               "to the generated glue. Use the std spelling (`std::string`, "
               "`std::vector<T>`, `std::map<std::string, T>`) or the untyped "
               "`LogosMap` / `LogosList`.";
    if (t.endsWith("*") || t.endsWith("&&"))
        return "A pointer or rvalue reference has no wire form. Pass the value "
               "(by value or `const T&`), or a `struct` declared in this "
               "header.";

    return "The recognised spellings are: `bool`, `int64_t`, `uint64_t`, "
           "`double`, `std::string`, `std::vector<uint8_t>` (bytes), "
           "`std::optional<T>`, `std::vector<T>`, `std::map<std::string, T>`, "
           "`std::unordered_map<std::string, T>`, `LogosMap` / `LogosList` / "
           "`nlohmann::json` (untyped JSON), `StdLogosResult` and `void` as "
           "returns, plus any `struct` declared in this header. Rewrite the "
           "declaration with one of them, or declare a struct for it.";
}

// `context` names the declaration being typed ("method 'foo': parameter 'bar'")
// and `declared` the full spelling on it, so a nested offender reports both the
// element that has no LIDL type and the declaration that carries it. `record` is
// set only while typing a struct's fields, so a diagnostic can be withdrawn when
// the struct turns out never to reach the contract.
//
// `nameEmitted` marks the slots whose C++ spelling the generator WRITES OUT into
// code the author's own declaration has to match — a record field's codec, an
// event's generated body. In those the derived spelling is a constraint on the
// author; everywhere else the generated code only has to consume or produce a
// value, and can adapt to whatever the author declared.
static TypeExpr cppTypeToLidl(const QString& raw, const QString& context = QString(),
                              const QString& declared = QString(),
                              const QString& record = QString(),
                              bool nameEmitted = false)
{
    // Normalize: strip const, &, leading/trailing whitespace
    QString t = normalizeCppSpelling(raw);

    // Primitives
    if (t == "bool")     return { TypeExpr::Primitive, "bool", {} };
    if (t == "int64_t")  return { TypeExpr::Primitive, "int", {} };
    if (t == "uint64_t") return { TypeExpr::Primitive, "uint", {} };
    if (t == "double")   return { TypeExpr::Primitive, "float64", {} };
    if (t == "void")     return { TypeExpr::Primitive, "void", {} };

    // std::string
    if (t == "std::string")
        return { TypeExpr::Primitive, "tstr", {} };

    // std::vector<T>
    static QRegularExpression vecRe("^std::vector\\s*<\\s*(.+)\\s*>$");
    QRegularExpressionMatch m = vecRe.match(t);
    if (m.hasMatch()) {
        QString inner = m.captured(1).trimmed();
        if (inner == "std::string") {
            TypeExpr elem = { TypeExpr::Primitive, "tstr", {} };
            return { TypeExpr::Array, "", { elem } };
        }
        if (inner == "uint8_t") {
            return { TypeExpr::Primitive, "bstr", {} };
        }
        // std::vector<std::vector<uint8_t>> — an array of byte strings. Spelled
        // out so it lands on `[bstr]` rather than the opaque `any` fallback
        // below, which would emit a bare QVariant into the Qt-free TU. As
        // `[bstr]` it goes through the cdylib list codec
        // (lidlBytesListFromJson / lidlBytesListToJson), so each element keeps
        // the canonical tagged {"_bytes": base64url} form on the wire.
        if (inner == "std::vector<uint8_t>") {
            TypeExpr elem = { TypeExpr::Primitive, "bstr", {} };
            return { TypeExpr::Array, "", { elem } };
        }
        if (inner == "int64_t") {
            TypeExpr elem = { TypeExpr::Primitive, "int", {} };
            return { TypeExpr::Array, "", { elem } };
        }
        if (inner == "uint64_t") {
            TypeExpr elem = { TypeExpr::Primitive, "uint", {} };
            return { TypeExpr::Array, "", { elem } };
        }
        if (inner == "double") {
            TypeExpr elem = { TypeExpr::Primitive, "float64", {} };
            return { TypeExpr::Array, "", { elem } };
        }
        if (inner == "bool") {
            TypeExpr elem = { TypeExpr::Primitive, "bool", {} };
            return { TypeExpr::Array, "", { elem } };
        }
        // Anything else: recurse. That is what makes `std::vector<Blob>` a
        // [Blob] and `std::vector<std::map<std::string, int64_t>>` a
        // [{tstr: int}]. Without it the element list above was exhaustive and
        // every other vector fell all the way through to the opaque `any`,
        // which then encoded a record as a LogosMap.
        return { TypeExpr::Array, "", { cppTypeToLidl(inner, context, declared, record, nameEmitted) } };
    }

    // Qt collection types — pass through directly (non-std-convertible)
    if (t == "QVariantMap")
        return { TypeExpr::Map, "", { {TypeExpr::Primitive, "tstr", {}}, {TypeExpr::Primitive, "any", {}} } };
    if (t == "QVariantList")
        return { TypeExpr::Array, "", { {TypeExpr::Primitive, "any", {}} } };
    if (t == "QStringList")
        return { TypeExpr::Array, "", { {TypeExpr::Primitive, "tstr", {}} } };

    // LogosMap / LogosList — nlohmann::json aliases; same LIDL shape as the Qt types
    // but flagged so the generator emits an nlohmann→Qt conversion in the glue.
    if (t == "LogosMap")
        return { TypeExpr::Map, "", { {TypeExpr::Primitive, "tstr", {}}, {TypeExpr::Primitive, "any", {}} } };
    if (t == "LogosList")
        return { TypeExpr::Array, "", { {TypeExpr::Primitive, "any", {}} } };

    // The alias spelled out. LogosMap / LogosList ARE nlohmann::json, and real
    // modules write the underlying name — test_fullapi_cpp's `echoAny` /
    // `fireAnyEvent` / `anyEvent`, and both full_api interface headers, all
    // declare `nlohmann::json`. It reached `any` ONLY through the fallback at
    // the bottom of this function, so naming it here is a PREREQUISITE for
    // turning that fallback into an error: without this branch the whole
    // cross-language conformance chain stops building.
    //
    // Mapped to the bare `any` primitive rather than LogosMap's `{tstr: any}` /
    // LogosList's `[any]`: `nlohmann::json` is an untyped value of ANY kind, not
    // specifically an object or an array. That is the type the fallback already
    // produced for it, so nothing about the published contract moves.
    if (t == "nlohmann::json" || t == "json")
        return { TypeExpr::Primitive, "any", {} };

    // StdLogosResult — pure C++ result type for universal impls. The generator
    // emits a StdLogosResult→Qt LogosResult conversion in the glue layer.
    if (t == "StdLogosResult")
        return { TypeExpr::Primitive, "result", {} };

    // std::map / std::unordered_map<std::string, T> -> {tstr: T}. Absent before,
    // so a typed map was unspellable header-first and fell through to `any`.
    //
    // Both containers, because logos_codec.h specializes Codec for both and they
    // are the same wire shape — a JSON object. Only the KEY is constrained: a
    // non-`std::string` key falls through to the unsupported report below, since
    // `{tstr: T}` is the only map LIDL has.
    static QRegularExpression mapRe(
        "^std::(?:unordered_)?map\\s*<\\s*std::string\\s*,\\s*(.+)\\s*>$");
    QRegularExpressionMatch mm = mapRe.match(t);
    if (mm.hasMatch()) {
        // ...with one boundary. In a `nameEmitted` slot the generator WRITES the
        // spelling out — a record field's codec says `Codec<std::map<...>>` and
        // an event's generated body repeats the parameter list the author
        // declared. It has to pick one of the two container names there, and
        // picking the wrong one is a compile error in code the author never
        // wrote. Method parameters and returns have no such constraint: they go
        // through logos::JsonArg / deduced logos::toJson, which instantiate with
        // whatever the author declared.
        if (t.startsWith("std::unordered_map") && nameEmitted && !context.isEmpty()) {
            UnsupportedSpelling u;
            u.context = context;
            u.record = record;
            u.declared = declared.isEmpty() ? t : declared;
            u.offending = t;
            u.hint = "`{tstr: T}` has two C++ spellings and this slot's spelling "
                     "is written into generated code your own declaration has to "
                     "match, so it can only be one of them: declare it "
                     "`std::map<std::string, T>`. (A method parameter or return "
                     "may use either container — those are decoded and encoded "
                     "through your declared type, not a named one.)";
            g_unsupported.append(u);
        }
        TypeExpr val = cppTypeToLidl(mm.captured(1).trimmed(), context, declared, record, nameEmitted);
        return { TypeExpr::Map, "", { {TypeExpr::Primitive, "tstr", {}}, val } };
    }

    // std::optional<T> -> ?T. Absent before, and the failure was silent: the
    // fallback at the bottom of this function maps ANY unrecognised spelling to
    // the opaque `any`, so a header-first C++ provider could not express
    // optionality at all — it declared `std::optional<std::string>` and
    // published a contract saying `any`, with no diagnostic.
    //
    // The derived contract uses the type-kind spelling (`name: ?T`). C++ has
    // only one spelling, LIDL has two, and they are bound to the same meaning —
    // so which one is emitted is a serialization choice, not a semantic one.
    static QRegularExpression optRe("^std::optional\\s*<\\s*(.+)\\s*>$");
    QRegularExpressionMatch om = optRe.match(t);
    if (om.hasMatch()) {
        TypeExpr inner = cppTypeToLidl(om.captured(1).trimmed(), context, declared, record, nameEmitted);
        // std::optional<std::optional<T>> has NO LIDL type.
        //
        // `?T` is two-state, and optionality is idempotent under that rule — so
        // the nearest contract type is plain `?T`, and that is what gets
        // published. But the author's C++ has THREE states (nullopt / an engaged
        // outer holding nullopt / a value), and the wire has two: accepting the
        // declaration as-is would make `optional(nullopt)` and `nullopt` encode
        // to the same null and decode back as one of them, silently. So it maps
        // down, the generated codec is written for std::optional<T>, and the
        // author's own declaration stops compiling against it — deliberately.
        // Say why here, where the reason is known, rather than leaving a
        // conversion error in generated code the author never wrote.
        if (inner.kind == TypeExpr::Optional) {
            g_unmappableSpellings << t;
            return inner;
        }
        return { TypeExpr::Optional, "", { inner } };
    }

    // A record the header declared. Checked LAST so it can never shadow a
    // builtin spelling, and gated on the declared set so an unknown type keeps
    // the historical `any` fallback rather than naming a struct nobody emits.
    if (g_recordNames.contains(t))
        return { TypeExpr::Named, t.toStdString(), {} };

    // NOTHING above matched: this spelling has no LIDL type.
    //
    // It used to return the opaque `any` right here, silently. `any` is admitted
    // by every backend gate, so the declaration was accepted and the generated
    // dispatch handed the raw nlohmann::json to the author's parameter with no
    // `logos::fromJson<>` and no check — the one hole left open after #113-#122
    // closed it for every TYPED slot. A `std::vector<uint32_t>` parameter
    // published `[any]` and worked by accident; a
    // `std::vector<std::pair<std::string, std::string>>` published `[any]` and
    // shipped raw binary through a UTF-8 string.
    //
    // The return value is UNCHANGED (`any`) on purpose: mapping and diagnosis
    // are separate concerns. A diagnostic that is later withdrawn — a helper
    // struct that never reaches the contract, a reserved lifecycle hook — must
    // leave the emitted output byte-identical to what it was.
    //
    // An empty spelling is not a C++ type at all, it is this line-based parser
    // failing to find one (a macro, a member initialiser). Reporting "'' has no
    // LIDL type" would be noise, so it keeps the old behaviour.
    if (!t.isEmpty() && !context.isEmpty()) {
        UnsupportedSpelling u;
        u.context = context;
        u.record = record;
        u.declared = declared.isEmpty() ? t : declared;
        u.offending = t;
        u.hint = unsupportedHint(t);
        g_unsupported.append(u);
    }
    return { TypeExpr::Primitive, "any", {} };
}

// Remove comments from the already-merged logical lines, honouring string and
// character literals and carrying block-comment state across lines.
//
// The record scanner used to strip with a bare `indexOf("//")`. That is right
// for `std::string name;   // what it is` and WRONG for
// `std::string url = "http://x";`, which it truncates inside the literal — the
// declaration then no longer ends in ';', and the field vanished. Harmless
// enough while an unreadable line was merely skipped; now that it is a build
// error, the same truncation would reject valid code, so the strip has to know
// what a literal is. Block comments are removed for the same reason: a field
// annotated `std::string id;  /* note */` did not end in ';' either.
static QStringList stripCommentsFrom(const QStringList& lines)
{
    QStringList out;
    bool inBlock = false;
    for (const QString& line : lines) {
        QString kept;
        bool inStr = false;
        bool inChr = false;
        for (int i = 0; i < line.size(); ++i) {
            const QChar c = line[i];
            const QChar n = (i + 1 < line.size()) ? line[i + 1] : QChar();
            if (inBlock) {
                if (c == '*' && n == '/') { inBlock = false; ++i; }
                continue;
            }
            if (inStr || inChr) {
                kept += c;
                if (c == '\\' && i + 1 < line.size()) { kept += n; ++i; }
                else if (inStr && c == '"') inStr = false;
                else if (inChr && c == '\'') inChr = false;
                continue;
            }
            if (c == '/' && n == '*') { inBlock = true; ++i; continue; }
            if (c == '/' && n == '/') break;
            if (c == '"') inStr = true;
            else if (c == '\'') inChr = true;
            kept += c;
        }
        out.append(kept.trimmed());
    }
    return out;
}

// A `struct` DEFINITION opening, in the forms C++ is actually written in:
//
//     struct Name {          K&R — the only form the scanner used to accept
//
//     struct Name            Allman — the opening brace on the next line
//     {
//
// plus the base-clause spelling of either. `struct Name;` is a forward
// declaration and stays out: there is no body to read.
//
// Allman was not a harmless stylistic omission. The struct was not a record AT
// ALL, so every mention of it in a signature fell through to the `any` fallback
// — which since #127 is a hard error whose hint tells the author to "declare a
// struct", the very thing they did declare. Where a brace sits cannot decide
// what a header means.
struct StructOpen {
    QString name;
    QString text;                    // the opening as written, for diagnostics
    bool hasBase = false;
    bool bodyOnOpeningLine = false;  // `struct P { int64_t a; };` all on one line
    int bodyStart = -1;              // index of the first line INSIDE the body
};

static bool matchStructOpen(const QStringList& code, int i, StructOpen& out)
{
    // `[^{;]` in the base clause keeps `struct Name;` and the brace itself out
    // of the capture.
    static const QRegularExpression kandrRe(
        "^struct\\s+(\\w+)\\s*(:[^{;]*)?\\{(.*)$");
    static const QRegularExpression headRe("^struct\\s+(\\w+)\\s*(:[^{;]*)?$");

    const QRegularExpressionMatch km = kandrRe.match(code.at(i));
    if (km.hasMatch()) {
        out.name = km.captured(1);
        out.text = code.at(i);
        out.hasBase = !km.captured(2).trimmed().isEmpty();
        out.bodyOnOpeningLine = !km.captured(3).trimmed().isEmpty();
        out.bodyStart = i + 1;
        return true;
    }

    const QRegularExpressionMatch hm = headRe.match(code.at(i));
    if (!hm.hasMatch()) return false;
    // Allman: the next line carrying any code at all has to open the body.
    // Anything else and this was not a definition (a `struct Name` mentioned in
    // some other construct), so it is left alone exactly as before.
    for (int j = i + 1; j < code.size(); ++j) {
        if (code.at(j).isEmpty()) continue;
        if (!code.at(j).startsWith('{')) return false;
        out.name = hm.captured(1);
        out.text = code.at(i);
        out.hasBase = !hm.captured(2).trimmed().isEmpty();
        out.bodyOnOpeningLine = !code.at(j).mid(1).trimmed().isEmpty();
        out.bodyStart = j + 1;
        return true;
    }
    return false;
}

static void reportUnreadable(const QString& record, const QString& text,
                             const QString& hint)
{
    g_unreadable.append({ record, text.trimmed(), hint });
}

// Net brace depth a line adds, ignoring braces inside string and character
// literals: `std::string s = "{";` is balanced code even though it is not
// balanced text, and a body scan that believed the text would never find the
// end of the struct.
static int braceDelta(const QString& line)
{
    int delta = 0;
    bool inStr = false;
    bool inChr = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (inStr || inChr) {
            if (c == '\\') { ++i; continue; }
            if (inStr && c == '"') inStr = false;
            else if (inChr && c == '\'') inChr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '\'') inChr = true;
        else if (c == '{') ++delta;
        else if (c == '}') --delta;
    }
    return delta;
}

// What a contract field looks like. Named once because three diagnostics quote
// it, and a hint that describes a different grammar than the one enforced is
// worse than no hint.
static const QString kFieldFormHint = QStringLiteral(
    "A contract field is ONE declaration per line, ending in `;` — `Type name;`, "
    "optionally with a default (`= v` or `{v}`). A declaration wrapped across "
    "several lines is joined for you; two declarations sharing one line are not.");

// Find `struct Name { Type field; ... };` blocks and turn them into `type`
// declarations.
//
// The parser used to SKIP any line starting with `struct`, which meant a record
// could not be declared header-first at all — the only way to get one was a
// hand-written .lidl. Worse, a method mentioning the struct still parsed: its
// type fell through to the opaque `any`, so the contract silently disagreed
// with the header.
//
// Two things beyond that are new here, and they are the same idea from opposite
// ends. The body is read as DECLARATIONS rather than lines — physical lines are
// joined until the `;`, exactly as the caller already joins a method signature
// until its parentheses balance — so a wrapped field is the field the author
// wrote rather than nothing at all. And whatever is left over after that, and
// after the constructs that definitively are NOT fields, is reported instead of
// skipped: the scanner may not quietly decide that a line it cannot read was
// not worth publishing.
static std::vector<TypeDecl> scanForRecords(const QStringList& lines)
{
    // `\{[^;]*\}` accepts a brace initialiser beside the `=` form: a field
    // written `std::string id{"none"};` is a field, and dropping it published a
    // record whose defaults decided which members a consumer could see.
    static QRegularExpression fieldRe(
        "^([\\w:<>,\\s\\*]+?)\\s+(\\w+)\\s*(=[^;]*|\\{[^;]*\\})?;$");
    static QRegularExpression accessRe("^(public|private|protected)\\s*:");
    // Declarations that are legitimately not fields, and are skipped by rule
    // rather than by failing to match. A `static` data member is not part of
    // the object's value and never reaches the wire.
    static QRegularExpression notAFieldRe(
        "^(using|typedef|friend|template|static_assert|static|constexpr|inline)\\b");
    static QRegularExpression nestedTypeRe("^(struct|class|union|enum)\\b");

    // Comments come off ONCE, up front, so every rule below sees code.
    const QStringList code = stripCommentsFrom(lines);

    // TWO passes. A record field may name another record (`Blob inner;` inside
    // Wrapper), and cppTypeToLidl only answers Named() for a name already in
    // g_recordNames — so every struct name has to be registered before any
    // field is typed. One pass silently typed such a field as `any`, and the
    // generated codec then tried to encode a Blob as a LogosMap.
    for (int i = 0; i < code.size(); ++i) {
        StructOpen so;
        if (matchStructOpen(code, i, so))
            g_recordNames.insert(so.name);
    }

    std::vector<TypeDecl> out;
    for (int i = 0; i < code.size(); ++i) {
        StructOpen so;
        if (!matchStructOpen(code, i, so)) continue;

        TypeDecl td;
        td.name = so.name.toStdString();
        // Withdraw this struct's diagnostics if it turns out to declare no
        // fields at all — nothing is published, so nothing is misreported.
        const int diagMark = g_unsupported.size();

        if (so.hasBase) {
            reportUnreadable(
                so.name, so.text,
                QString("`struct %1` has a base class, and this parser reads one "
                        "header as text — the inherited members are not in front "
                        "of it. Publishing the struct would drop exactly the "
                        "fields it cannot see. Declare the record without a base "
                        "and give it the inherited fields explicitly.")
                    .arg(so.name));
        }

        if (so.bodyOnOpeningLine) {
            // The body shares the opening line, and the scan below starts on the
            // NEXT one, so there is nothing for it to read. Say so instead of
            // publishing an empty record.
            reportUnreadable(so.name, so.text,
                             QString("The body shares the line with the opening "
                                     "brace. Put each field on its own line. %1")
                                 .arg(kFieldFormHint));
        } else {
            // The body is read as DECLARATIONS, not lines: physical lines are
            // joined until the declaration is whole, which is a `;` at the
            // struct's own brace depth — or a `}` there, which is how a member
            // function DEFINED inline ends. Depth is tracked because a member
            // function's body, and a nested type's, are declarations of their
            // own that a `;` inside them must not be mistaken for the end of.
            QString acc;
            int depth = 1;   // inside the struct
            for (int j = so.bodyStart; j >= 0 && j < code.size(); ++j) {
                QString body = code.at(j);
                // An access specifier may share the line with a declaration, as
                // in the class-body parser. Strip it before anything else, or
                // `public: std::string id;` reads as a field whose TYPE is
                // `public: std::string`.
                while (true) {
                    const QRegularExpressionMatch am = accessRe.match(body);
                    if (!am.hasMatch()) break;
                    body = body.mid(am.capturedEnd()).trimmed();
                }
                if (body.isEmpty()) continue;

                const int delta = braceDelta(body);
                if (depth + delta <= 0) {
                    // End of the struct. Anything still accumulating never
                    // became a whole declaration — report it rather than
                    // dropping it on the way out.
                    if (!acc.isEmpty())
                        reportUnreadable(so.name, acc, kFieldFormHint);
                    break;
                }

                acc = acc.isEmpty() ? body : acc + ' ' + body;
                depth += delta;
                // Not a whole declaration yet: a field wrapped across physical
                // lines is still the same field, and a `;` inside an inline
                // member-function body does not end the member.
                if (depth != 1 || !(acc.endsWith(';') || acc.endsWith('}')))
                    continue;

                const QString decl = acc;
                acc.clear();

                // The DECLARATOR is everything before the first `=` or `{`. A
                // default value may legally contain parentheses
                // (`std::string id = makeId();`), and only parentheses in the
                // declarator make the line a member function.
                qsizetype cut = decl.size();
                const qsizetype eq = decl.indexOf('=');
                const qsizetype brace = decl.indexOf('{');
                if (eq >= 0) cut = qMin(cut, eq);
                if (brace >= 0) cut = qMin(cut, brace);
                if (decl.left(cut).contains('('))
                    continue;   // member function / constructor / destructor
                if (notAFieldRe.match(decl).hasMatch())
                    continue;

                if (nestedTypeRe.match(decl).hasMatch()) {
                    // A nested type is not a field — and the scanner used to
                    // walk straight into its body, folding the INNER type's
                    // members into this record's field list and stopping at the
                    // inner `};`, so the published record was made of another
                    // type's fields and missing all of its own.
                    reportUnreadable(
                        so.name, decl,
                        QString("A nested type is not a field, and its own "
                                "members were being folded into `%1`. Declare it "
                                "at namespace scope — it becomes a contract "
                                "`type` of its own — and give `%1` a field of "
                                "that type.")
                            .arg(so.name));
                    continue;
                }

                const QRegularExpressionMatch fm = fieldRe.match(decl);
                if (!fm.hasMatch()) {
                    reportUnreadable(so.name, decl, kFieldFormHint);
                    continue;
                }
                FieldDecl fd;
                fd.name = fm.captured(2).toStdString();
                const QString spelling = fm.captured(1).trimmed();
                fd.type = cppTypeToLidl(
                    spelling,
                    QString("type '%1': field '%2'").arg(so.name, fm.captured(2)),
                    spelling, so.name, /*nameEmitted=*/true);
                td.fields.push_back(fd);
            }
        }

        if (!td.fields.empty()) {
            out.push_back(td);
        } else {
            while (g_unsupported.size() > diagMark) g_unsupported.removeLast();
            if (!g_emptyRecords.contains(so.name))
                g_emptyRecords.append(so.name);
        }
    }
    return out;
}

// Keep only the records the module's API actually mentions.
//
// An impl header routinely declares helper structs that are none of a
// consumer's business — `struct PendingAction` inside the class, a
// `struct ModuleSource` next to it. Publishing every struct as a contract
// `type` changes the module's PUBLISHED interface as a side effect of an
// internal refactor, which is not something deriving a contract from a header
// is allowed to do. A struct earns its place in the contract by appearing in a
// method or event signature — transitively, since a published record's own
// fields may name others.
//
// Returns that referenced set. It is the withdrawal key for BOTH diagnostic
// channels: a struct the API never names promises nothing, so neither an
// unsupported field type nor a line the scanner could not read is a defect in
// it. The set — not the published types — is what a structural diagnostic is
// tested against, because the very failures being reported are the ones that
// keep a struct OUT of module.types.
static std::set<std::string> keepOnlyReferencedRecords(ModuleDecl& module)
{
    auto mention = [](const TypeExpr& te, std::set<std::string>& out) {
        std::function<void(const TypeExpr&)> walk = [&](const TypeExpr& t) {
            if (t.kind == TypeExpr::Named) out.insert(t.name);
            for (const TypeExpr& e : t.elements) walk(e);
        };
        walk(te);
    };

    std::set<std::string> referenced;
    for (const MethodDecl& md : module.methods) {
        mention(md.returnType, referenced);
        for (const ParamDecl& pd : md.params) mention(pd.type, referenced);
    }
    for (const EventDecl& ed : module.events)
        for (const ParamDecl& pd : ed.params) mention(pd.type, referenced);

    // Transitive closure: a referenced record's fields may name more records.
    bool grew = true;
    while (grew) {
        grew = false;
        for (const TypeDecl& td : module.types) {
            if (!referenced.count(td.name)) continue;
            for (const FieldDecl& fd : td.fields) {
                std::set<std::string> here;
                mention(fd.type, here);
                for (const std::string& n : here)
                    if (referenced.insert(n).second) grew = true;
            }
        }
    }

    std::vector<TypeDecl> kept;
    for (const TypeDecl& td : module.types)
        if (referenced.count(td.name)) kept.push_back(td);
    module.types = std::move(kept);
    return referenced;
}

// ---------------------------------------------------------------------------
// Parse a single method declaration line
// ---------------------------------------------------------------------------

// `kind` is "method" or "event" — it only labels the diagnostics an unsupported
// C++ spelling produces, so the report matches the section the declaration was
// written in rather than the function that happens to parse both.
static bool parseMethodLine(const QString& line, MethodDecl& out,
                            const QString& kind = "method")
{
    // Find the parameter list: everything between the last '(' and ')'
    int parenOpen = -1;
    int parenClose = -1;
    int depth = 0;
    for (int i = line.size() - 1; i >= 0; --i) {
        if (line[i] == ')') {
            if (parenClose < 0) parenClose = i;
            depth++;
        } else if (line[i] == '(') {
            depth--;
            if (depth == 0) {
                parenOpen = i;
                break;
            }
        }
    }
    if (parenOpen < 0 || parenClose < 0)
        return false;

    QString paramStr = line.mid(parenOpen + 1, parenClose - parenOpen - 1).trimmed();

    // Everything before '(' is "returnType methodName"
    QString prefix = line.left(parenOpen).trimmed();

    // The method name is the last identifier token in prefix
    int nameEnd = prefix.size();
    while (nameEnd > 0 && prefix[nameEnd - 1].isSpace())
        nameEnd--;
    int nameStart = nameEnd;
    while (nameStart > 0 && (prefix[nameStart - 1].isLetterOrNumber() || prefix[nameStart - 1] == '_'))
        nameStart--;

    if (nameStart >= nameEnd)
        return false;

    const QString methodName = prefix.mid(nameStart, nameEnd - nameStart);

    // Reject if the extracted name is a C++ keyword — this filters out
    // member variable declarations like "std::function<void(...)> onEvent"
    // where the parser would mistakenly extract "void" as the method name.
    static const QSet<QString> cppKeywords = {
        "void", "int", "bool", "char", "short", "long", "double", "float",
        "auto", "return", "if", "else", "for", "while", "do", "switch",
        "case", "break", "continue", "const", "static", "inline", "virtual"
    };
    if (cppKeywords.contains(methodName))
        return false;
    out.name = methodName.toStdString();
    QString retTypeStr = stripDeclarationSpecifiers(prefix.left(nameStart).trimmed());
    out.returnType = cppTypeToLidl(
        retTypeStr, QString("%1 '%2': return type").arg(kind, methodName), retTypeStr,
        QString(), /*nameEmitted=*/kind == "event");
    // Flag methods whose impl returns LogosMap / LogosList so the generator
    // can emit nlohmann→Qt conversion code in the glue layer.
    out.jsonReturn = (retTypeStr == "LogosMap" || retTypeStr == "LogosList");
    // Flag methods whose impl returns StdLogosResult so the generator can
    // emit a StdLogosResult→Qt LogosResult conversion in the glue layer.
    out.resultReturn = (retTypeStr == "StdLogosResult");

    // Parse parameters
    out.params.clear();
    if (!paramStr.isEmpty()) {
        // Split by comma, respecting template depth
        QStringList parts;
        int start = 0;
        int tdepth = 0;
        for (int i = 0; i < paramStr.size(); ++i) {
            if (paramStr[i] == '<') tdepth++;
            else if (paramStr[i] == '>') tdepth--;
            else if (paramStr[i] == ',' && tdepth == 0) {
                parts.append(paramStr.mid(start, i - start).trimmed());
                start = i + 1;
            }
        }
        parts.append(paramStr.mid(start).trimmed());

        for (const QString& part : parts) {
            if (part.isEmpty()) continue;
            QString p = part.trimmed();
            int pNameEnd = p.size();
            while (pNameEnd > 0 && p[pNameEnd - 1].isSpace())
                pNameEnd--;
            int pNameStart = pNameEnd;
            while (pNameStart > 0 && (p[pNameStart - 1].isLetterOrNumber() || p[pNameStart - 1] == '_'))
                pNameStart--;

            if (pNameStart >= pNameEnd) continue;

            ParamDecl pd;
            const QString pName = p.mid(pNameStart, pNameEnd - pNameStart);
            const QString pSpelling = p.left(pNameStart).trimmed();
            pd.name = pName.toStdString();
            pd.type = cppTypeToLidl(
                p.left(pNameStart),
                QString("%1 '%2': parameter '%3'").arg(kind, methodName, pName),
                pSpelling, QString(), /*nameEmitted=*/kind == "event");
            out.params.push_back(pd);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

// Join doc-comment lines preserving line breaks (drop leading/trailing blanks).
static QString joinDocLines(QStringList lines)
{
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    return lines.join('\n');
}

ImplParseResult parseImplHeader(const QString& headerPath,
                                const QString& className,
                                const QString& metadataPath,
                                QTextStream& err)
{
    ImplParseResult result;

    // Every file-static above is per-parse state: one process generates for more
    // than one module. g_recordNames is cleared HERE as well as beside
    // scanForRecords, because a name left over from the previous module's header
    // would otherwise be visible while this one's metadata events are typed.
    g_unmappableSpellings.clear();
    g_unsupported.clear();
    g_unreadable.clear();
    g_emptyRecords.clear();
    g_recordNames.clear();

    QJsonArray metadataEvents;

    // --- Read metadata.json ---
    {
        QFile mf(metadataPath);
        if (!mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            result.error = "Failed to open metadata file: " + metadataPath;
            return result;
        }
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(mf.readAll(), &pe);
        if (pe.error != QJsonParseError::NoError) {
            result.error = "Failed to parse metadata JSON: " + pe.errorString();
            return result;
        }
        QJsonObject obj = doc.object();
        result.module.name = obj.value("name").toString().toStdString();
        result.module.version = obj.value("version").toString().toStdString();
        result.module.description = obj.value("description").toString().toStdString();
        result.module.category = obj.value("category").toString().toStdString();
        const QJsonArray deps = obj.value("dependencies").toArray();
        for (const QString& depName : dependencyNames(deps))
            result.module.depends.push_back(depName.toStdString());

        // Events declared in metadata.json. Only READ here — their parameter
        // types are C++ spellings like any other, and typing them requires the
        // record set, which does not exist until the header has been scanned.
        // They used to be typed right here, against whatever g_recordNames the
        // PREVIOUS module's parse left behind.
        metadataEvents = obj.value("events").toArray();
    }

    // --- Read and parse header ---
    QFile hf(headerPath);
    if (!hf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.error = "Failed to open header file: " + headerPath;
        return result;
    }
    QString source = QString::fromUtf8(hf.readAll());
    hf.close();

    // Records first: cppTypeToLidl consults the declared set, so the structs
    // have to be known before a single signature is looked at.
    // Split into physical lines, then merge any whose parentheses are still
    // open into one logical line. The scanner below is line-based — it only
    // accepts a method when a single trimmed line ends in ';' and
    // parseMethodLine finds a balanced '(...)' on it — so without this a method
    // signature wrapped across several physical lines is silently dropped.
    // Parens inside comments / string / char literals are ignored.
    QStringList lines;
    {
        const QStringList physical = source.split('\n');
        QString acc;
        int parenDepth = 0;
        bool inBlockComment = false;
        for (const QString& phys : physical) {
            bool inStr = false;
            bool inChr = false;
            for (int i = 0; i < phys.size(); ++i) {
                const QChar c = phys[i];
                const QChar n = (i + 1 < phys.size()) ? phys[i + 1] : QChar();
                if (inBlockComment) {
                    if (c == '*' && n == '/') { inBlockComment = false; ++i; }
                } else if (inStr) {
                    if (c == '\\') ++i; else if (c == '"') inStr = false;
                } else if (inChr) {
                    if (c == '\\') ++i; else if (c == '\'') inChr = false;
                } else if (c == '/' && n == '*') {
                    inBlockComment = true; ++i;
                } else if (c == '/' && n == '/') {
                    break;
                } else if (c == '"') {
                    inStr = true;
                } else if (c == '\'') {
                    inChr = true;
                } else if (c == '(') {
                    ++parenDepth;
                } else if (c == ')') {
                    if (parenDepth > 0) --parenDepth;
                }
            }
            if (acc.isEmpty())
                acc = phys;
            else
                acc += ' ' + phys.trimmed();
            if (parenDepth <= 0) {
                lines.append(acc);
                acc.clear();
            }
        }
        if (!acc.isEmpty())
            lines.append(acc);
    }

    // Records, before any signature is examined: cppTypeToLidl() consults the
    // declared set, so a `Blob` parameter only becomes Named("Blob") once the
    // struct has been seen. Reset per parse — the set is file-static and a
    // single process generates for more than one module.
    g_recordNames.clear();
    result.module.types = scanForRecords(lines);

    // Now the record set exists, the metadata-declared events can be typed. They
    // stay AHEAD of the header's `logos_events:` events, as they always were.
    for (const QJsonValue& ev : metadataEvents) {
        QJsonObject evObj = ev.toObject();
        EventDecl ed;
        ed.name = evObj.value("name").toString().toStdString();
        ed.description = evObj.value("description").toString().toStdString();
        const QJsonArray params = evObj.value("params").toArray();
        for (const QJsonValue& pv : params) {
            QJsonObject po = pv.toObject();
            ParamDecl pd;
            const QString pName = po.value("name").toString();
            const QString pType = po.value("type").toString();
            pd.name = pName.toStdString();
            pd.type = cppTypeToLidl(
                pType,
                QString("event '%1': parameter '%2' (declared in metadata.json)")
                    .arg(evObj.value("name").toString(), pName),
                pType, QString(), /*nameEmitted=*/true);
            ed.params.push_back(pd);
        }
        if (!ed.name.empty())
            result.module.events.push_back(ed);
    }

    // State machine: find "class <className>", then collect declarations.
    // `InLogosEvents` is entered by the literal `logos_events:` token
    // (mirrors Qt's `signals:`) — methods declared there are parsed as
    // EventDecls and appended to ModuleDecl.events instead of .methods.
    enum State { LookingForClass, InClass, InPublic, InPrivate, InLogosEvents };
    State state = LookingForClass;
    int braceDepth = 0;

    // Accumulates doc-comment lines adjacent to a method so the doc comment
    // becomes the method's description. Reset on any blank / non-comment line.
    QStringList pendingDoc;
    bool inBlockComment = false;

    QRegularExpression classRe("\\bclass\\s+" + QRegularExpression::escape(className) + "\\b");
    QRegularExpression accessRe("^\\s*(public|private|protected)\\s*:");
    QRegularExpression eventsRe("^\\s*logos_events\\s*:");
    QRegularExpression ctorDtorRe("^\\s*~?" + QRegularExpression::escape(className) + "\\s*\\(");

    for (const QString& rawLine : lines) {
        QString line = rawLine.trimmed();

        switch (state) {
        case LookingForClass:
            if (classRe.match(line).hasMatch()) {
                state = InClass;
                for (QChar c : line) {
                    if (c == '{') braceDepth++;
                    else if (c == '}') braceDepth--;
                }
            }
            break;

        case InClass:
        case InPublic:
        case InPrivate:
        case InLogosEvents:
            // Inside a multi-line /** ... */ doc-comment block: capture its
            // text (skip brace counting — comments don't affect scope).
            if (inBlockComment) {
                QString t = line;
                int end = t.indexOf("*/");
                if (end >= 0) { t = t.left(end); inBlockComment = false; }
                t.remove(QRegularExpression(R"(^\*+\s?)"));
                t = t.trimmed();
                pendingDoc.append(t);
                break;
            }

            // Count braces only on real code lines. Braces inside a doc/line
            // comment (e.g. `/// returns { "k": v }`) must not affect scope
            // tracking, or an unbalanced brace in a comment would make the
            // parser think the class ended early and drop later declarations.
            if (!(line.startsWith("//") || line.startsWith("/*") || line.startsWith("*"))) {
                for (QChar c : line) {
                    if (c == '{') braceDepth++;
                    else if (c == '}') braceDepth--;
                }

                if (braceDepth <= 0) {
                    state = LookingForClass;
                    goto done;
                }
            }

            // A section specifier may be followed by a declaration on the
            // *same* physical line — e.g. clang-format / prettier collapse
            //     logos_events:
            //         void versionReady(const std::string& version);
            // into `logos_events : void versionReady(const std::string& version);`.
            // Strip any leading specifiers, updating the section state, and
            // let whatever remains fall through to the declaration parser
            // below — otherwise everything after the colon is discarded and
            // the same valid C++ is parsed differently based on formatting.
            //
            // `logos_events:` takes precedence over the standard access
            // specifiers: it's a separate section that the codegen pulls
            // event prototypes from. (At preprocess time, `logos_events`
            // expands to `public`, but the raw source still carries the
            // token we recognise here.)
            bool specifierStripped = false;
            while (true) {
                QRegularExpressionMatch em = eventsRe.match(line);
                if (em.hasMatch()) {
                    state = InLogosEvents;
                    line = line.mid(em.capturedEnd()).trimmed();
                    specifierStripped = true;
                    continue;
                }
                QRegularExpressionMatch am = accessRe.match(line);
                if (am.hasMatch()) {
                    QString spec = am.captured(1);
                    if (spec == "public") state = InPublic;
                    else state = InPrivate;
                    line = line.mid(am.capturedEnd()).trimmed();
                    specifierStripped = true;
                    continue;
                }
                break;
            }
            // A *bare* specifier (nothing after the colon) is a section
            // boundary and resets any pending doc-comment, mirroring Qt's
            // `signals:`. But when a declaration shares the line, the doc
            // comment preceding the whole line must still attach to that
            // declaration — otherwise documentation, like the declaration
            // itself (#76), would become formatting-dependent. So only clear
            // here for the bare form; the same-line form keeps pendingDoc and
            // attaches it in the declaration parser below.
            if (specifierStripped && line.isEmpty())
                pendingDoc.clear();

            // Only doc comments (/// or /** ... */ / /*! ... */) accumulate as
            // the pending description for the next method. Plain // and /*
            // comments are ignored but leave pending doc intact; blank /
            // preprocessor lines reset it so only *adjacent* comments attach.
            if (line.startsWith("///")) {
                QString text = line.mid(3);
                if (text.startsWith('<')) text = text.mid(1); // ///< trailing form
                text = text.trimmed();
                pendingDoc.append(text);
                break;
            }
            if (line.startsWith("/**") || line.startsWith("/*!")) {
                QString text = line.mid(3);
                int end = text.indexOf("*/");
                if (end >= 0) text = text.left(end);
                else inBlockComment = true;
                text.remove(QRegularExpression(R"(^\*+\s?)"));
                text = text.trimmed();
                pendingDoc.append(text);
                break;
            }
            if (line.startsWith("//") || line.startsWith("/*") || line.startsWith("*")) {
                break; // non-doc comment: ignore, keep pending doc
            }
            if (line.isEmpty() || line.startsWith("#")) {
                pendingDoc.clear();
                break;
            }

            if (ctorDtorRe.match(line).hasMatch()) {
                pendingDoc.clear();
                break;
            }

            if (line.startsWith("typedef") || line.startsWith("using")
                || line.startsWith("friend") || line.startsWith("enum")
                || line.startsWith("struct")) {
                pendingDoc.clear();
                break;
            }

            if (state == InLogosEvents) {
                // Inside `logos_events:` — every bare prototype is an event.
                // Events are always void-returning by definition, so we
                // re-use parseMethodLine to extract name + params and
                // discard the return type.
                if (line.endsWith(';')) {
                    QString decl = line.left(line.size() - 1).trimmed();
                    MethodDecl md;
                    if (parseMethodLine(decl, md, "event")) {
                        EventDecl ed;
                        ed.name = md.name;
                        ed.params = md.params;
                        ed.description = joinDocLines(pendingDoc).toStdString();
                        result.module.events.push_back(ed);
                    }
                }
                pendingDoc.clear();
                break;
            }

            if (state != InPublic) { pendingDoc.clear(); break; }

            if (line.contains("std::function<")) {
                // A std::function member is not a method — skip it so the
                // `parseMethodLine` path below doesn't choke on the nested
                // parens in its type. (Events are declared in a typed
                // `logos_events:` section, parsed above — there is no longer
                // any special `std::function emitEvent` member to detect.)
                pendingDoc.clear();
                break;
            }

            if (line.endsWith(';')) {
                QString decl = line.left(line.size() - 1).trimmed();
                MethodDecl md;
                // Withdraw the declaration's diagnostics if it turns out to be a
                // reserved lifecycle hook: it is not part of the contract, so an
                // unsupported spelling in it is not a contract defect.
                const int diagMark = g_unsupported.size();
                if (parseMethodLine(decl, md)) {
                    // LogosModuleContext lifecycle hooks / context accessors are
                    // framework plumbing, not part of the module's API contract.
                    // An impl commonly overrides `onContextReady()` (and could
                    // re-declare an accessor) in its own public section, so the
                    // header parser would otherwise emit them into the derived
                    // LIDL — breaking cdylib eligibility (e.g. the inherited
                    // accessors' Qt-free-subset check) and exposing non-API
                    // methods. Skip the reserved names regardless of access.
                    static const QSet<QString> reserved = {
                        "onContextReady", "modules", "modulePath",
                        "instanceId", "instancePersistencePath"
                    };
                    if (!reserved.contains(qs(md.name))) {
                        md.description = joinDocLines(pendingDoc).toStdString();
                        result.module.methods.push_back(md);
                    } else {
                        while (g_unsupported.size() > diagMark)
                            g_unsupported.removeLast();
                    }
                }
            }
            pendingDoc.clear();
            break;
        }
    }

done:
    // Now that every signature is known, drop the structs the API never
    // mentions — a header's internal helpers must not become published
    // contract types.
    const std::set<std::string> referenced = keepOnlyReferencedRecords(result.module);

    // STRUCTURE the scanner could not read is a BUILD ERROR, not a shorter
    // record.
    //
    // Reported before the type diagnostics below because it is the more
    // fundamental failure: when the scanner could not read a struct's body, the
    // types it did manage to read there are not a trustworthy account of it
    // either. Reported after keepOnlyReferencedRecords, and tested against the
    // REFERENCED set rather than the published one, for the reason given on that
    // function: a helper struct the API never mentions may be as unreadable as
    // it likes, while a struct that failed to publish anything is exactly the
    // case that has to be caught.
    {
        QStringList reports;
        QSet<QString> seen;
        for (const UnreadableDecl& u : g_unreadable) {
            if (!referenced.count(u.record.toStdString()))
                continue;  // struct never reaches the contract
            const QString line =
                QString("  type '%1': `%2`\n    could not be read as a field. %3")
                    .arg(u.record, u.text, u.hint);
            if (seen.contains(line)) continue;
            seen.insert(line);
            reports << line;
        }
        // A struct the API NAMES that published no field at all. Nothing above
        // need have fired — a body of nothing but member functions reads
        // perfectly well and yields no record — and the emitted contract would
        // then reference a `type` it never declares, which no reader of the
        // .lidl can resolve and no backend can generate.
        for (const QString& name : g_emptyRecords) {
            if (!referenced.count(name.toStdString())) continue;
            bool explained = false;
            for (const UnreadableDecl& u : g_unreadable)
                if (u.record == name) { explained = true; break; }
            if (explained) continue;
            reports << QString(
                           "  type '%1' is named by this module's API but declares "
                           "no field this parser could read, so no `type %1` is "
                           "emitted and the contract would name a type it never "
                           "declares.\n    %2")
                           .arg(name, kFieldFormHint);
        }
        if (!reports.isEmpty()) {
            result.error =
                headerPath + ": " + QString::number(reports.size())
                + (reports.size() == 1 ? " declaration in a struct this module "
                                         "publishes could not be read.\n\n"
                                       : " declarations in structs this module "
                                         "publishes could not be read.\n\n")
                + reports.join("\n\n")
                + "\n\nA `struct` in this header becomes a contract `type`, and its "
                  "field list IS the promise consumers in every language bind to. "
                  "Each of these used to be skipped, and the record published "
                  "without it — a contract missing a field is as well-formed as one "
                  "that has it, so nothing downstream could tell.\n";
            return result;
        }
    }

    // A C++ spelling with no LIDL type is a BUILD ERROR, not a silent `any`.
    //
    // Reported after keepOnlyReferencedRecords so a helper struct that never
    // reaches the contract cannot fail the build: publishing is what makes a
    // declaration's type a promise, and an internal struct promises nothing.
    {
        std::set<std::string> published;
        for (const TypeDecl& td : result.module.types) published.insert(td.name);

        QStringList reports;
        QSet<QString> seen;
        for (const UnsupportedSpelling& u : g_unsupported) {
            if (!u.record.isEmpty() && !published.count(u.record.toStdString()))
                continue;  // struct dropped: not part of the contract
            QString line = "  " + u.context;
            // "declared X, whose element Y" only when Y really is nested inside
            // X — not when the two differ by a `const` and an `&`.
            if (normalizeCppSpelling(u.declared) != u.offending)
                line += QString(" is declared `%1`, whose element `%2` has no "
                                "LIDL type.\n    ").arg(u.declared, u.offending);
            else
                line += QString(" is `%1`, which has no LIDL type.\n    ")
                            .arg(u.offending);
            line += u.hint;
            if (seen.contains(line)) continue;
            seen.insert(line);
            reports << line;
        }
        if (!reports.isEmpty()) {
            result.error =
                headerPath + ": " + QString::number(reports.size())
                + (reports.size() == 1 ? " declaration uses" : " declarations use")
                + " a C++ type that has no LIDL type.\n\n"
                + reports.join("\n\n")
                + "\n\nEach of these used to be published as the opaque `any`, with no "
                  "diagnostic. `any` is admitted by every backend gate, so the generated "
                  "dispatch handed the raw JSON straight to the parameter with no decode "
                  "and no check — the value either converted by luck, threw at call time, "
                  "or went onto the wire in a form no other language decodes.\n";
            return result;
        }
    }

    if (!g_unmappableSpellings.isEmpty()) {
        g_unmappableSpellings.removeDuplicates();
        err << "Warning: " << headerPath << ": " << g_unmappableSpellings.join(", ")
            << " has no LIDL type. `?T` is TWO-state — a value or empty — so a "
               "nested optional cannot denote a third state; the contract "
               "publishes the collapsed `?T`, and the generated codec is "
               "written for std::optional<T>. Declare it that way, or the "
               "generated code will not compile against this header.\n";
    }

    if (result.module.methods.empty()) {
        err << "Warning: no public methods found in class " << className
            << " in " << headerPath << "\n";
    }

    return result;
}
