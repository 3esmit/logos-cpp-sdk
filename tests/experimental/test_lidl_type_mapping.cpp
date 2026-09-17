#include <gtest/gtest.h>
#include "lidl_gen_client.h"
#include "lidl_emit_common.h"

#include <cstring>

#include "lidl/serializer.hpp"

// ---------------------------------------------------------------------------
// lidlTypeToQt
// ---------------------------------------------------------------------------

TEST(LidlTypeToQt, Tstr)
{
    TypeExpr te = { TypeExpr::Primitive, "tstr", {} };
    EXPECT_EQ(lidlTypeToQt(te), "QString");
}

TEST(LidlTypeToQt, Bstr)
{
    TypeExpr te = { TypeExpr::Primitive, "bstr", {} };
    EXPECT_EQ(lidlTypeToQt(te), "QByteArray");
}

// LIDL int/uint are 64-bit everywhere (int64_t/uint64_t in C++ impls, i64/u64 in
// Rust), so the Qt spelling has to be 64-bit too — one LIDL type, one type per
// language. `int` truncated, and for `uint` it also flipped the signedness.
TEST(LidlTypeToQt, Int)
{
    TypeExpr te = { TypeExpr::Primitive, "int", {} };
    EXPECT_EQ(lidlTypeToQt(te), "qlonglong");
}

TEST(LidlTypeToQt, Uint)
{
    TypeExpr te = { TypeExpr::Primitive, "uint", {} };
    EXPECT_EQ(lidlTypeToQt(te), "qulonglong");
}

TEST(LidlTypeToQt, Float64)
{
    TypeExpr te = { TypeExpr::Primitive, "float64", {} };
    EXPECT_EQ(lidlTypeToQt(te), "double");
}

TEST(LidlTypeToQt, Bool)
{
    TypeExpr te = { TypeExpr::Primitive, "bool", {} };
    EXPECT_EQ(lidlTypeToQt(te), "bool");
}

TEST(LidlTypeToQt, Result)
{
    TypeExpr te = { TypeExpr::Primitive, "result", {} };
    EXPECT_EQ(lidlTypeToQt(te), "LogosResult");
}

TEST(LidlTypeToQt, Any)
{
    TypeExpr te = { TypeExpr::Primitive, "any", {} };
    EXPECT_EQ(lidlTypeToQt(te), "QVariant");
}

TEST(LidlTypeToQt, ArrayOfTstr)
{
    TypeExpr elem = { TypeExpr::Primitive, "tstr", {} };
    TypeExpr te = { TypeExpr::Array, "", { elem } };
    EXPECT_EQ(lidlTypeToQt(te), "QStringList");
}

TEST(LidlTypeToQt, ArrayOfInt)
{
    TypeExpr elem = { TypeExpr::Primitive, "int", {} };
    TypeExpr te = { TypeExpr::Array, "", { elem } };
    EXPECT_EQ(lidlTypeToQt(te), "QList<qlonglong>");
}

TEST(LidlTypeToQt, MapType)
{
    TypeExpr key = { TypeExpr::Primitive, "tstr", {} };
    TypeExpr val = { TypeExpr::Primitive, "int", {} };
    TypeExpr te = { TypeExpr::Map, "", { key, val } };
    EXPECT_EQ(lidlTypeToQt(te), "QMap<QString, qlonglong>");
}

TEST(LidlTypeToQt, OptionalType)
{
    TypeExpr inner = { TypeExpr::Primitive, "tstr", {} };
    TypeExpr te = { TypeExpr::Optional, "", { inner } };
    EXPECT_EQ(lidlTypeToQt(te), "std::optional<QString>");
}

// A Named type is a RECORD declared by the contract, and the client generator
// emits a struct of that name — so the Qt spelling is the struct, not an opaque
// QVariant. One LIDL type, one type per language.
TEST(LidlTypeToQt, NamedTypeIsItsRecordStruct)
{
    TypeExpr te = { TypeExpr::Named, "MyStruct", {} };
    EXPECT_EQ(lidlTypeToQt(te), "MyStruct");
}

// ---------------------------------------------------------------------------
// lidlTypeToStd
// ---------------------------------------------------------------------------

TEST(LidlTypeToStd, Tstr)
{
    TypeExpr te = { TypeExpr::Primitive, "tstr", {} };
    EXPECT_EQ(lidlTypeToStd(te), "std::string");
}

TEST(LidlTypeToStd, Bstr)
{
    TypeExpr te = { TypeExpr::Primitive, "bstr", {} };
    EXPECT_EQ(lidlTypeToStd(te), "std::vector<uint8_t>");
}

TEST(LidlTypeToStd, Int)
{
    TypeExpr te = { TypeExpr::Primitive, "int", {} };
    EXPECT_EQ(lidlTypeToStd(te), "int64_t");
}

TEST(LidlTypeToStd, Uint)
{
    TypeExpr te = { TypeExpr::Primitive, "uint", {} };
    EXPECT_EQ(lidlTypeToStd(te), "uint64_t");
}

TEST(LidlTypeToStd, Float64)
{
    TypeExpr te = { TypeExpr::Primitive, "float64", {} };
    EXPECT_EQ(lidlTypeToStd(te), "double");
}

TEST(LidlTypeToStd, Bool)
{
    TypeExpr te = { TypeExpr::Primitive, "bool", {} };
    EXPECT_EQ(lidlTypeToStd(te), "bool");
}

TEST(LidlTypeToStd, Result)
{
    TypeExpr te = { TypeExpr::Primitive, "result", {} };
    EXPECT_EQ(lidlTypeToStd(te), "LogosResult");
}

TEST(LidlTypeToStd, ArrayOfTstr)
{
    TypeExpr elem = { TypeExpr::Primitive, "tstr", {} };
    TypeExpr te = { TypeExpr::Array, "", { elem } };
    EXPECT_EQ(lidlTypeToStd(te), "std::vector<std::string>");
}

TEST(LidlTypeToStd, ArrayOfInt)
{
    TypeExpr elem = { TypeExpr::Primitive, "int", {} };
    TypeExpr te = { TypeExpr::Array, "", { elem } };
    EXPECT_EQ(lidlTypeToStd(te), "std::vector<int64_t>");
}

TEST(LidlTypeToStd, ArrayOfBool)
{
    TypeExpr elem = { TypeExpr::Primitive, "bool", {} };
    TypeExpr te = { TypeExpr::Array, "", { elem } };
    EXPECT_EQ(lidlTypeToStd(te), "std::vector<bool>");
}

// ---------------------------------------------------------------------------
// lidlIsStdConvertible
// ---------------------------------------------------------------------------

TEST(LidlIsStdConvertible, PrimitivesAreConvertible)
{
    EXPECT_TRUE(lidlIsStdConvertible({ TypeExpr::Primitive, "tstr", {} }));
    EXPECT_TRUE(lidlIsStdConvertible({ TypeExpr::Primitive, "bstr", {} }));
    EXPECT_TRUE(lidlIsStdConvertible({ TypeExpr::Primitive, "int", {} }));
    EXPECT_TRUE(lidlIsStdConvertible({ TypeExpr::Primitive, "uint", {} }));
    EXPECT_TRUE(lidlIsStdConvertible({ TypeExpr::Primitive, "float64", {} }));
    EXPECT_TRUE(lidlIsStdConvertible({ TypeExpr::Primitive, "bool", {} }));
}

TEST(LidlIsStdConvertible, ResultIsNotConvertible)
{
    EXPECT_FALSE(lidlIsStdConvertible({ TypeExpr::Primitive, "result", {} }));
}

TEST(LidlIsStdConvertible, AnyIsNotConvertible)
{
    EXPECT_FALSE(lidlIsStdConvertible({ TypeExpr::Primitive, "any", {} }));
}

TEST(LidlIsStdConvertible, SimpleArraysAreConvertible)
{
    TypeExpr elem = { TypeExpr::Primitive, "tstr", {} };
    EXPECT_TRUE(lidlIsStdConvertible({ TypeExpr::Array, "", { elem } }));
}

TEST(LidlIsStdConvertible, MapIsNotConvertible)
{
    TypeExpr key = { TypeExpr::Primitive, "tstr", {} };
    TypeExpr val = { TypeExpr::Primitive, "int", {} };
    EXPECT_FALSE(lidlIsStdConvertible({ TypeExpr::Map, "", { key, val } }));
}

// ---------------------------------------------------------------------------
// lidlToPascalCase
// ---------------------------------------------------------------------------

TEST(LidlToPascalCase, SnakeCase)
{
    EXPECT_EQ(lidlToPascalCase("my_module"), "MyModule");
}

TEST(LidlToPascalCase, SingleWord)
{
    EXPECT_EQ(lidlToPascalCase("wallet"), "Wallet");
}

TEST(LidlToPascalCase, Empty)
{
    EXPECT_EQ(lidlToPascalCase(""), "Module");
}

TEST(LidlToPascalCase, MultipleDelimiters)
{
    EXPECT_EQ(lidlToPascalCase("a_b_c"), "ABC");
}

TEST(LidlToPascalCase, LeadingUnderscore)
{
    EXPECT_EQ(lidlToPascalCase("_test"), "Test");
}

// ---------------------------------------------------------------------------
// Optionality
//
// `?T` is TWO-state: a value of T, or empty. The two surfaces answer it
// differently because only one of them HAS an optional type — the std surface
// keeps T inside std::optional, the Qt surface loses it, because Qt has no
// optional metatype and an invalid QVariant is its single empty inhabitant.
// ---------------------------------------------------------------------------

static TypeExpr opt(const TypeExpr& inner)
{
    return { TypeExpr::Optional, "", { inner } };
}

TEST(LidlTypeToStd, OptionalKeepsTheValueType)
{
    EXPECT_EQ(lidlTypeToStd(opt({ TypeExpr::Primitive, "tstr", {} })),
              "std::optional<std::string>");
    EXPECT_EQ(lidlTypeToStd(opt({ TypeExpr::Primitive, "uint", {} })),
              "std::optional<uint64_t>");
    EXPECT_EQ(lidlTypeToStd(opt({ TypeExpr::Primitive, "bstr", {} })),
              "std::optional<std::vector<uint8_t>>");
}

// Two-state, so optionality is idempotent: `??T` denotes the same two states as
// `?T` and must not become std::optional<std::optional<T>> — that would be a
// third state on the C++ side with nowhere to put it on the wire.
TEST(LidlTypeToStd, NestedOptionalCollapses)
{
    const TypeExpr t = opt(opt({ TypeExpr::Primitive, "tstr", {} }));
    EXPECT_EQ(lidlTypeToStd(t), "std::optional<std::string>");
}

// A degenerate Optional carrying no element is unreachable from the parser but
// constructible by hand and over the JSON bridge. It must not recurse forever.
TEST(LidlTypeToStd, DegenerateOptionalTerminates)
{
    EXPECT_EQ(lidlTypeToStd(TypeExpr{ TypeExpr::Optional, "", {} }), "QVariant");
}

// `?T` KEEPS T on the Qt surface. It used to be a bare QVariant — the one row
// in this table that lost the value type, so a consumer could not tell `?tstr`
// from `?uint` — while the std table next door kept both through
// std::optional. The two surfaces now answer the same question the same way.
TEST(LidlTypeToQt, OptionalKeepsItsValueType)
{
    EXPECT_EQ(lidlTypeToQt(opt({ TypeExpr::Primitive, "tstr", {} })), "std::optional<QString>");
    EXPECT_EQ(lidlTypeToQt(opt({ TypeExpr::Primitive, "uint", {} })), "std::optional<qulonglong>");
    EXPECT_EQ(lidlTypeToQt(opt({ TypeExpr::Named, "Blob", {} })), "std::optional<Blob>");
}

// ---------------------------------------------------------------------------
// The widened Qt table — every row, and the rows that must NOT widen
//
// The table is the whole of this change's contract with the Qt emitters: they
// spell signatures from it and decide, from lidlQtNeedsElementLoop, whether a
// value may cross whole. A wrong row here is a silently-dropped payload three
// layers down, so each is stated on its own.
// ---------------------------------------------------------------------------

namespace {

TypeExpr prim(const char* n) { return TypeExpr{ TypeExpr::Primitive, n, {} }; }
TypeExpr named(const char* n) { return TypeExpr{ TypeExpr::Named, n, {} }; }
TypeExpr arr(const TypeExpr& e) { return TypeExpr{ TypeExpr::Array, "", { e } }; }
TypeExpr map(const TypeExpr& v) { return TypeExpr{ TypeExpr::Map, "", { prim("tstr"), v } }; }
TypeExpr optOf(const TypeExpr& e) { return TypeExpr{ TypeExpr::Optional, "", { e } }; }

}  // namespace

TEST(LidlTypeToQtWidened, TypedArrays)
{
    EXPECT_EQ(lidlTypeToQt(arr(prim("uint"))),    "QList<qulonglong>");
    EXPECT_EQ(lidlTypeToQt(arr(prim("int"))),     "QList<qlonglong>");
    EXPECT_EQ(lidlTypeToQt(arr(prim("bool"))),    "QList<bool>");
    EXPECT_EQ(lidlTypeToQt(arr(prim("float64"))), "QList<double>");
    EXPECT_EQ(lidlTypeToQt(arr(prim("bstr"))),    "QList<QByteArray>");
    EXPECT_EQ(lidlTypeToQt(arr(named("Point"))),  "QList<Point>");
}

// `[tstr]` stays QStringList — the one typed array Qt has a native spelling
// for, and the one already in qvariantToNlohmann's closed userType() set. It
// crosses WHOLE, so it must not be given an element loop it does not need.
TEST(LidlTypeToQtWidened, ArrayOfTstrStaysQStringList)
{
    EXPECT_EQ(lidlTypeToQt(arr(prim("tstr"))), "QStringList");
    EXPECT_FALSE(lidlQtNeedsElementLoop(arr(prim("tstr"))));
}

TEST(LidlTypeToQtWidened, TypedMaps)
{
    EXPECT_EQ(lidlTypeToQt(map(prim("uint"))),   "QMap<QString, qulonglong>");
    EXPECT_EQ(lidlTypeToQt(map(prim("tstr"))),   "QMap<QString, QString>");
    EXPECT_EQ(lidlTypeToQt(map(prim("bstr"))),   "QMap<QString, QByteArray>");
    EXPECT_EQ(lidlTypeToQt(map(named("Point"))), "QMap<QString, Point>");
}

TEST(LidlTypeToQtWidened, RecursionAtDepth)
{
    EXPECT_EQ(lidlTypeToQt(arr(arr(prim("uint")))),  "QList<QList<qulonglong>>");
    EXPECT_EQ(lidlTypeToQt(map(arr(prim("uint")))),  "QMap<QString, QList<qulonglong>>");
    EXPECT_EQ(lidlTypeToQt(arr(map(prim("uint")))),  "QList<QMap<QString, qulonglong>>");
    EXPECT_EQ(lidlTypeToQt(arr(optOf(prim("tstr")))), "QList<std::optional<QString>>");
    EXPECT_EQ(lidlTypeToQt(optOf(arr(prim("uint")))), "std::optional<QList<qulonglong>>");
    EXPECT_EQ(lidlTypeToQt(arr(arr(named("Point")))), "QList<QList<Point>>");
}

// THE `any` RULE. `any` is the only row the widened table keeps untyped, and it
// is not an omission: QVariant is the sole Qt type that carries bytes AND an
// exact uint64 AND arbitrary nesting, so every narrower spelling would LOSE
// something. A type whose scalar leaf is `any` therefore keeps the QVariant-
// family name at every depth.
TEST(LidlTypeToQtWidened, AnyBottomedShapesKeepTheQVariantSpelling)
{
    EXPECT_EQ(lidlTypeToQt(prim("any")),              "QVariant");
    EXPECT_EQ(lidlTypeToQt(arr(prim("any"))),         "QVariantList");
    EXPECT_EQ(lidlTypeToQt(map(prim("any"))),         "QVariantMap");
    EXPECT_EQ(lidlTypeToQt(optOf(prim("any"))),       "QVariant");
    EXPECT_EQ(lidlTypeToQt(arr(arr(prim("any")))),    "QVariantList");
    EXPECT_EQ(lidlTypeToQt(map(arr(prim("any")))),    "QVariantMap");
    EXPECT_EQ(lidlTypeToQt(arr(map(prim("any")))),    "QVariantList");
    EXPECT_EQ(lidlTypeToQt(optOf(arr(prim("any")))),  "QVariant");
}

// `??T` denotes the SAME two states as `?T` — optionality is idempotent under
// the two-state rule — so it must collapse rather than become a three-state
// std::optional<std::optional<T>>. Recursed through optionalValueType(), which
// is the frontend's own accessor.
TEST(LidlTypeToQtWidened, NestedOptionalCollapses)
{
    EXPECT_EQ(lidlTypeToQt(optOf(optOf(prim("tstr")))), "std::optional<QString>");
    EXPECT_EQ(lidlTypeToQt(optOf(optOf(optOf(prim("uint"))))), "std::optional<qulonglong>");
}

// Degenerate shapes are unreachable from the parser but constructible by hand
// and over the JSON bridge. They must terminate, and they must land on the
// opaque spelling rather than being described as typed.
TEST(LidlTypeToQtWidened, DegenerateShapesTerminate)
{
    EXPECT_EQ(lidlTypeToQt(TypeExpr{ TypeExpr::Optional, "", {} }), "QVariant");
    EXPECT_EQ(lidlTypeToQt(TypeExpr{ TypeExpr::Array, "", {} }),    "QVariantList");
    EXPECT_EQ(lidlTypeToQt(TypeExpr{ TypeExpr::Map, "", {} }),      "QVariantMap");
}

// The record-qualifier hook. A wrapper nests its record structs in the wrapper
// class, so a type written outside that scope has to qualify them — at EVERY
// depth, which is what a string match on the finished name could not do once
// `?Point` and `QList<QList<Point>>` became spellable.
TEST(LidlTypeToQtWidened, RecordQualifierReachesEveryDepth)
{
    auto qual = [](const QString& n) { return "Wrapper::" + n; };
    EXPECT_EQ(lidlTypeToQt(named("Point"), qual),              "Wrapper::Point");
    EXPECT_EQ(lidlTypeToQt(optOf(named("Point")), qual),       "std::optional<Wrapper::Point>");
    EXPECT_EQ(lidlTypeToQt(arr(arr(named("Point"))), qual),    "QList<QList<Wrapper::Point>>");
    EXPECT_EQ(lidlTypeToQt(map(optOf(named("Point"))), qual),
              "QMap<QString, std::optional<Wrapper::Point>>");
    // Nothing else is touched by the hook.
    EXPECT_EQ(lidlTypeToQt(arr(prim("uint")), qual), "QList<qulonglong>");
}

// ---------------------------------------------------------------------------
// lidlQtNeedsElementLoop — the predicate that keeps the widened table from
// destroying data.
//
// A name it answers true for must NEVER reach logos::qt::toWire /
// fromWire<T> (or QVariant::fromValue / qvariant_cast) as a whole value:
// qvariantToNlohmann matches a closed userType() set, so QList<qulonglong>
// serialises to null, and qvariant_cast back yields an EMPTY list. Silently,
// both directions. So a false negative here is a silently dropped payload.
// ---------------------------------------------------------------------------

TEST(LidlQtNeedsElementLoop, TrueForEveryTypedContainerAndOptional)
{
    EXPECT_TRUE(lidlQtNeedsElementLoop(arr(prim("uint"))));
    EXPECT_TRUE(lidlQtNeedsElementLoop(arr(prim("bstr"))));
    EXPECT_TRUE(lidlQtNeedsElementLoop(arr(named("Point"))));
    EXPECT_TRUE(lidlQtNeedsElementLoop(map(prim("uint"))));
    EXPECT_TRUE(lidlQtNeedsElementLoop(map(named("Point"))));
    EXPECT_TRUE(lidlQtNeedsElementLoop(optOf(prim("tstr"))));
    EXPECT_TRUE(lidlQtNeedsElementLoop(arr(arr(prim("uint")))));
    EXPECT_TRUE(lidlQtNeedsElementLoop(optOf(arr(prim("uint")))));
}

// The QVariant-native spellings cross whole and always did. Emitting a loop for
// them would be a gratuitous change to shipped output.
TEST(LidlQtNeedsElementLoop, FalseForEverythingQVariantNative)
{
    EXPECT_FALSE(lidlQtNeedsElementLoop(prim("tstr")));
    EXPECT_FALSE(lidlQtNeedsElementLoop(prim("uint")));
    EXPECT_FALSE(lidlQtNeedsElementLoop(prim("bstr")));
    EXPECT_FALSE(lidlQtNeedsElementLoop(prim("any")));
    EXPECT_FALSE(lidlQtNeedsElementLoop(prim("result")));
    EXPECT_FALSE(lidlQtNeedsElementLoop(named("Point")));   // a record SCALAR
    EXPECT_FALSE(lidlQtNeedsElementLoop(arr(prim("tstr"))));  // QStringList
    EXPECT_FALSE(lidlQtNeedsElementLoop(arr(prim("any"))));   // QVariantList
    EXPECT_FALSE(lidlQtNeedsElementLoop(map(prim("any"))));   // QVariantMap
    EXPECT_FALSE(lidlQtNeedsElementLoop(optOf(prim("any")))); // QVariant
}

// A record SCALAR is false above because this predicate answers only for the
// TYPE TABLE's spellings; the emitters add the record shapes themselves (a
// struct has no metatype either). Stated so the false above is not read as
// "a record may cross whole".
TEST(LidlQtNeedsElementLoop, RecordScalarIsTheEmittersJobNotThisPredicates)
{
    EXPECT_FALSE(lidlQtNeedsElementLoop(named("Point")));
    EXPECT_EQ(lidlTypeToQt(named("Point")), "Point");
}

// ---------------------------------------------------------------------------
// lidlTypeToLidlText — the CONTRACT spelling published in getMethods()
//
// It is a copy of logos-lidl's serializeTypeExpr, which is file-local to that
// repo's serializer.cpp with no public printer to delegate to. So the pairing
// is asserted: each shape is round-tripped through lidl::serialize() and the
// type text is read back out of the emitted `.lidl`. If logos-lidl ever changes
// how it prints a type, these fail rather than the two drifting in silence.
// ---------------------------------------------------------------------------

namespace {

// The type text logos-lidl itself writes for `method m(p: <T>)`.
QString serializerSpelling(const TypeExpr& te)
{
    ModuleDecl m;
    m.name = "probe";
    MethodDecl md;
    md.name = "m";
    ParamDecl p;
    p.name = "p";
    p.type = te;
    md.params.push_back(p);
    md.returnType = TypeExpr{ TypeExpr::Primitive, "bool", {} };
    m.methods.push_back(md);

    const QString text = QString::fromStdString(lidl::serialize(m));
    const int open = text.indexOf("method m(p: ");
    if (open < 0) return QStringLiteral("<not found>");
    const int start = open + int(strlen("method m(p: "));
    const int close = text.indexOf(')', start);
    return text.mid(start, close - start);
}

}  // namespace

TEST(LidlTypeToLidlText, MatchesTheSerializerForEveryShape)
{
    const TypeExpr shapes[] = {
        prim("tstr"), prim("uint"), prim("int"), prim("bstr"), prim("bool"),
        prim("float64"), prim("any"), named("Point"),
        arr(prim("uint")), arr(prim("tstr")), arr(named("Point")),
        map(prim("uint")), map(prim("any")), map(named("Point")),
        optOf(prim("tstr")), optOf(named("Point")),
        arr(arr(prim("uint"))), map(arr(prim("uint"))), arr(map(prim("uint"))),
        arr(optOf(prim("tstr"))), optOf(arr(prim("uint"))),
    };
    for (const TypeExpr& te : shapes) {
        const QString mine = lidlTypeToLidlText(te);
        EXPECT_EQ(mine, serializerSpelling(te))
            << "lidlTypeToLidlText has drifted from lidl::serialize for this shape";
    }
}

// The spellings themselves, so a reader can see what getMethods() now publishes
// without running the serializer in their head.
TEST(LidlTypeToLidlText, PublishesTheContractVocabulary)
{
    EXPECT_EQ(lidlTypeToLidlText(prim("tstr")),        "tstr");
    EXPECT_EQ(lidlTypeToLidlText(prim("uint")),        "uint");
    EXPECT_EQ(lidlTypeToLidlText(arr(prim("uint"))),   "[uint]");
    EXPECT_EQ(lidlTypeToLidlText(arr(named("Point"))), "[Point]");
    EXPECT_EQ(lidlTypeToLidlText(map(prim("uint"))),   "{tstr: uint}");
    EXPECT_EQ(lidlTypeToLidlText(optOf(prim("tstr"))), "? tstr");
    EXPECT_EQ(lidlTypeToLidlText(arr(arr(prim("uint")))), "[[uint]]");
}
