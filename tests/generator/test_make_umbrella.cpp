#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include "generator_lib.h"

// The umbrella aggregates a module's declared `metadata.json#dependencies` into
// `LogosModules`. A dependency entry is either a bare name or an object holding
// that name alongside the constraints an installer resolves it by, and the two
// forms have to generate identical code — the constraints are the installer's
// business, not the generator's.
//
// What makes this worth asserting on rather than trusting: the aggregate is
// emitted by several passes over the same array (includes, constructor
// initialisers, members), so a form only one pass understands yields a member
// whose type was never included — an aggregate that no longer compiles, and one
// that nothing catches until a module builds against it.

namespace {

QJsonArray depsMixedForms()
{
    QJsonObject withVersion;
    withVersion["name"] = "dep_b";
    withVersion["version"] = "=1.2.3";

    QJsonObject withSigner;
    withSigner["name"] = "dep_c";
    withSigner["version"] = "^2.0";
    withSigner["signer"] = "did:jwk:abc";

    QJsonArray deps;
    deps.append("dep_a");
    deps.append(withVersion);
    deps.append(withSigner);
    return deps;
}

} // namespace

// Lp is the umbrella every `interface: universal` core module and every cdylib
// module generates (logos-plugin-qt picks --api-style lp for both).
TEST(MakeUmbrellaTest, LpAggregatesDependenciesDeclaredInEitherForm)
{
    const QString h = makeUmbrellaHeaderFromDeps(depsMixedForms(), {}, ApiStyle::Lp, "sample_module");

    EXPECT_TRUE(h.contains("#include \"dep_a_api.h\"")) << h.toStdString();
    EXPECT_TRUE(h.contains("#include \"dep_b_api.h\"")) << h.toStdString();
    EXPECT_TRUE(h.contains("#include \"dep_c_api.h\"")) << h.toStdString();

    EXPECT_TRUE(h.contains("DepA dep_a;")) << h.toStdString();
    EXPECT_TRUE(h.contains("DepB dep_b;")) << h.toStdString();
    EXPECT_TRUE(h.contains("DepC dep_c;")) << h.toStdString();

    // Lp wrappers self-create their lp_client on behalf of this module.
    EXPECT_TRUE(h.contains("dep_b(\"sample_module\")")) << h.toStdString();
    EXPECT_TRUE(h.contains("dep_c(\"sample_module\")")) << h.toStdString();
}

TEST(MakeUmbrellaTest, QtAggregatesDependenciesDeclaredInEitherForm)
{
    const QString h = makeUmbrellaHeaderFromDeps(depsMixedForms(), {}, ApiStyle::Qt);

    EXPECT_TRUE(h.contains("#include \"dep_a_api.h\"")) << h.toStdString();
    EXPECT_TRUE(h.contains("#include \"dep_b_api.h\"")) << h.toStdString();
    EXPECT_TRUE(h.contains("#include \"dep_c_api.h\"")) << h.toStdString();

    EXPECT_TRUE(h.contains("DepA dep_a;")) << h.toStdString();
    EXPECT_TRUE(h.contains("DepB dep_b;")) << h.toStdString();
    EXPECT_TRUE(h.contains("DepC dep_c;")) << h.toStdString();

    EXPECT_TRUE(h.contains("dep_b(api)")) << h.toStdString();
    EXPECT_TRUE(h.contains("dep_c(api)")) << h.toStdString();
}

// Every dep a member declaration mentions must have been included, in both
// flavors — the pairing is the invariant, independent of which form declared it.
TEST(MakeUmbrellaTest, EveryMemberTypeIsIncluded)
{
    for (ApiStyle style : {ApiStyle::Lp, ApiStyle::Qt}) {
        const QString h = makeUmbrellaHeaderFromDeps(depsMixedForms(), {}, style, "sample_module");
        for (const QString& dep : {QStringLiteral("dep_a"), QStringLiteral("dep_b"), QStringLiteral("dep_c")}) {
            const bool included = h.contains("#include \"" + dep + "_api.h\"");
            const bool member = h.contains(toPascalCase(dep) + " " + dep + ";");
            EXPECT_EQ(included, member)
                << "'" << dep.toStdString() << "' is a member without an include (or vice versa):\n"
                << h.toStdString();
        }
    }
}

TEST(MakeUmbrellaTest, SourceIncludesEveryDependencyWrapper)
{
    const QString c = makeUmbrellaSourceFromDeps(depsMixedForms(), {"some_iface"});

    EXPECT_TRUE(c.contains("#include \"dep_a_api.cpp\"")) << c.toStdString();
    EXPECT_TRUE(c.contains("#include \"dep_b_api.cpp\"")) << c.toStdString();
    EXPECT_TRUE(c.contains("#include \"dep_c_api.cpp\"")) << c.toStdString();
    EXPECT_TRUE(c.contains("#include \"some_iface_api.cpp\"")) << c.toStdString();
}

// An entry that names nothing is dropped rather than emitted as an empty
// member, and drops out of the aggregate entirely.
TEST(MakeUmbrellaTest, EntryNamingNothingIsDropped)
{
    QJsonArray deps;
    deps.append("dep_a");
    deps.append(QJsonObject{});
    deps.append(QJsonObject{{"version", "=1.0.0"}});

    for (ApiStyle style : {ApiStyle::Lp, ApiStyle::Qt}) {
        const QString h = makeUmbrellaHeaderFromDeps(deps, {}, style, "sample_module");
        EXPECT_TRUE(h.contains("DepA dep_a;")) << h.toStdString();
        EXPECT_FALSE(h.contains("#include \"_api.h\"")) << h.toStdString();
        EXPECT_FALSE(h.contains("Module ;")) << h.toStdString();
    }
}

// A module with no dependencies still gets a compilable, empty aggregate.
TEST(MakeUmbrellaTest, NoDependenciesStillEmitsTheAggregate)
{
    const QString lp = makeUmbrellaHeaderFromDeps({}, {}, ApiStyle::Lp, "sample_module");
    EXPECT_TRUE(lp.contains("struct LogosModules {")) << lp.toStdString();
    EXPECT_TRUE(lp.contains("LogosModules() {}")) << lp.toStdString();

    const QString qt = makeUmbrellaHeaderFromDeps({}, {}, ApiStyle::Qt);
    EXPECT_TRUE(qt.contains("struct LogosModules {")) << qt.toStdString();
    EXPECT_TRUE(qt.contains("LogosAPI* api;")) << qt.toStdString();
}
