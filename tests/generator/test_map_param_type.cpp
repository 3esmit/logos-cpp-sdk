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


// ---------------------------------------------------------------------------
// The widened-Qt-spelling FOLD
//
// lidlTypeToQt now answers `[uint]` with QList<qulonglong>, `{tstr: uint}` with
// QMap<QString, qulonglong> and `?tstr` with std::optional<QString>. This
// emitter is keyed on flat type NAMES and cannot encode any of them: doing it
// correctly needs an element loop per level, and it has no tree to derive the
// levels from (lidl_to_json flattens the contract to strings before it gets
// here, because the same emitter also serves the metaobject-introspection path).
//
// So they are folded back to the name this emitter already produced, and BOTH
// surfaces it feeds — the legacy Qt consumer and the Qt-free lp one, whose
// table is DERIVED from this one via mapParamTypeStd — stay byte-for-byte what
// they were.
//
// This is a freeze, and these tests are what makes it one. A future change that
// lets a widened spelling through would otherwise be invisible until a
// QList<qulonglong> reached QVariant::fromValue at runtime, where
// qvariantToNlohmann answers null and nothing warns.
// ---------------------------------------------------------------------------

TEST(WidenedSpellingFold, TypedArraysFoldToQVariantList)
{
    EXPECT_EQ(mapParamType("QList<qulonglong>"), "QVariantList");
    EXPECT_EQ(mapParamType("QList<qlonglong>"), "QVariantList");
    EXPECT_EQ(mapParamType("QList<QByteArray>"), "QVariantList");
    EXPECT_EQ(mapParamType("QList<QList<qulonglong>>"), "QVariantList");
    EXPECT_EQ(mapReturnType("QList<qulonglong>"), "QVariantList");
    EXPECT_EQ(mapReturnType("const QList<double>&"), "QVariantList");
}

TEST(WidenedSpellingFold, TypedMapsFoldToQVariantMap)
{
    EXPECT_EQ(mapParamType("QMap<QString, qulonglong>"), "QVariantMap");
    EXPECT_EQ(mapParamType("QMap<QString, QList<qulonglong>>"), "QVariantMap");
    EXPECT_EQ(mapReturnType("QMap<QString, QString>"), "QVariantMap");
}

TEST(WidenedSpellingFold, OptionalsFoldToQVariant)
{
    EXPECT_EQ(mapParamType("std::optional<QString>"), "QVariant");
    EXPECT_EQ(mapParamType("std::optional<QList<qulonglong>>"), "QVariant");
    EXPECT_EQ(mapReturnType("std::optional<qulonglong>"), "QVariant");
}

// The fold must not swallow the names it is meant to leave alone: QStringList
// is not a `QList<...>` spelling, and a record name reaches this emitter only
// after recordCppType has declined it.
TEST(WidenedSpellingFold, LeavesTheUnwidenedNamesAlone)
{
    EXPECT_EQ(mapParamType("QStringList"), "QStringList");
    EXPECT_EQ(mapReturnType("QStringList"), "QStringList");
    EXPECT_EQ(mapParamType("QVariantList"), "QVariantList");
    EXPECT_EQ(mapParamType("QVariantMap"), "QVariantMap");
    EXPECT_EQ(mapParamType("qulonglong"), "qulonglong");
    EXPECT_EQ(mapReturnType("LogosResult"), "LogosResult");
}
