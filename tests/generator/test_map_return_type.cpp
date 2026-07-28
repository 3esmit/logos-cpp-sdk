#include <gtest/gtest.h>
#include "generator_lib.h"

TEST(MapReturnTypeTest, KnownTypes)
{
    EXPECT_EQ(mapReturnType("bool"), "bool");
    EXPECT_EQ(mapReturnType("int"), "int");
    EXPECT_EQ(mapReturnType("double"), "double");
    EXPECT_EQ(mapReturnType("float"), "float");
    EXPECT_EQ(mapReturnType("QString"), "QString");
    EXPECT_EQ(mapReturnType("QStringList"), "QStringList");
    EXPECT_EQ(mapReturnType("QJsonArray"), "QJsonArray");
    EXPECT_EQ(mapReturnType("QVariantList"), "QVariantList");
    EXPECT_EQ(mapReturnType("QVariantMap"), "QVariantMap");
    EXPECT_EQ(mapReturnType("QVariant"), "QVariant");
    EXPECT_EQ(mapReturnType("LogosResult"), "LogosResult");
}

TEST(MapReturnTypeTest, VoidHandling)
{
    EXPECT_EQ(mapReturnType("void"), "void");
    EXPECT_EQ(mapReturnType(""), "void");
}

TEST(MapReturnTypeTest, ConstRef)
{
    EXPECT_EQ(mapReturnType("const QString&"), "QString");
}

TEST(MapReturnTypeTest, UnknownType)
{
    EXPECT_EQ(mapReturnType("MyCustomType"), "QVariant");
}

// LIDL int/uint are 64-bit, and since the Qt spelling became qlonglong/qulonglong
// they have to be in the known set — otherwise every int/uint method silently
// degrades to an opaque QVariant in the generated lp/std consumer wrappers.
TEST(MapReturnType, SixtyFourBitIntegersAreKnown)
{
    EXPECT_EQ(mapReturnType("qlonglong"), "qlonglong");
    EXPECT_EQ(mapReturnType("qulonglong"), "qulonglong");
    EXPECT_EQ(mapReturnType("const qlonglong&"), "qlonglong");
    // An unknown spelling still falls back.
    EXPECT_EQ(mapReturnType("SomeRecord"), "QVariant");
}

