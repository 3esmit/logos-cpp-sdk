#include <gtest/gtest.h>
#include "generator_lib.h"

TEST(MapParamTypeTest, KnownTypes)
{
    EXPECT_EQ(mapParamType("bool"), "bool");
    EXPECT_EQ(mapParamType("int"), "int");
    EXPECT_EQ(mapParamType("double"), "double");
    EXPECT_EQ(mapParamType("float"), "float");
    EXPECT_EQ(mapParamType("QString"), "QString");
    EXPECT_EQ(mapParamType("QStringList"), "QStringList");
    EXPECT_EQ(mapParamType("QJsonArray"), "QJsonArray");
    EXPECT_EQ(mapParamType("QVariantList"), "QVariantList");
    EXPECT_EQ(mapParamType("QVariantMap"), "QVariantMap");
    EXPECT_EQ(mapParamType("QVariant"), "QVariant");
    EXPECT_EQ(mapParamType("void"), "void");
}

TEST(MapParamTypeTest, ConstRef)
{
    EXPECT_EQ(mapParamType("const QString&"), "QString");
    EXPECT_EQ(mapParamType("const int&"), "int");
}

TEST(MapParamTypeTest, UnknownType)
{
    EXPECT_EQ(mapParamType("MyCustomType"), "QVariant");
    EXPECT_EQ(mapParamType("SomeClass*"), "QVariant");
}

TEST(MapParamTypeTest, LogosResultFallsBack)
{
    // LogosResult is NOT in the param known set
    EXPECT_EQ(mapParamType("LogosResult"), "QVariant");
}

// LIDL int/uint are 64-bit, and since the Qt spelling became qlonglong/qulonglong
// they have to be in the known set — otherwise every int/uint method silently
// degrades to an opaque QVariant in the generated lp/std consumer wrappers.
TEST(MapParamType, SixtyFourBitIntegersAreKnown)
{
    EXPECT_EQ(mapParamType("qlonglong"), "qlonglong");
    EXPECT_EQ(mapParamType("qulonglong"), "qulonglong");
    EXPECT_EQ(mapParamType("const qlonglong&"), "qlonglong");
    // An unknown spelling still falls back.
    EXPECT_EQ(mapParamType("SomeRecord"), "QVariant");
}

