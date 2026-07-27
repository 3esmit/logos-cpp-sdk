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

TEST(LidlTypeToQt, NamedType)
{
    TypeExpr te = { TypeExpr::Named, "MyStruct", {} };
    EXPECT_EQ(lidlTypeToQt(te), "QVariant");
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
