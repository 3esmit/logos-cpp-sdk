#include <gtest/gtest.h>
#include "lidl_gen_client.h"
#include "lidl_emit_common.h"

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
    EXPECT_EQ(lidlTypeToQt(te), "QVariantList");
}

TEST(LidlTypeToQt, MapType)
{
    TypeExpr key = { TypeExpr::Primitive, "tstr", {} };
    TypeExpr val = { TypeExpr::Primitive, "int", {} };
    TypeExpr te = { TypeExpr::Map, "", { key, val } };
    EXPECT_EQ(lidlTypeToQt(te), "QVariantMap");
}

TEST(LidlTypeToQt, OptionalType)
{
    TypeExpr inner = { TypeExpr::Primitive, "tstr", {} };
    TypeExpr te = { TypeExpr::Optional, "", { inner } };
    EXPECT_EQ(lidlTypeToQt(te), "QVariant");
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

TEST(LidlTypeToQt, OptionalIsUntypedQVariant)
{
    // Deliberate, and the one mapping in the Qt table that loses the value
    // type: there is no metatype for an optional, and this name is read by the
    // host to marshal a QVariant across the plugin boundary. Two-state survives
    // (an invalid QVariant is the empty inhabitant); T does not.
    EXPECT_EQ(lidlTypeToQt(opt({ TypeExpr::Primitive, "tstr", {} })), "QVariant");
    EXPECT_EQ(lidlTypeToQt(opt({ TypeExpr::Named, "Blob", {} })), "QVariant");
}
