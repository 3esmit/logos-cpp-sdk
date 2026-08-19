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

// ── The origin-bound Qt umbrella (UmbrellaBinding::ExplicitOrigin) ───────────
//
// The Qt umbrella used to have exactly one shape: `LogosModules(LogosAPI* api)`,
// with every member built as `<dep>(api)`. That single line is what kept the Qt
// type surface out of reach for a module with no LogosAPI — a cdylib, whose
// provider surface is the std `logos_module_impl.h` C ABI and whose generated
// glue emits an unconditional `new LogosModules()`. This flavour is that
// umbrella with the identity object removed and the module's OWN NAME baked in
// instead, matching the shape the Lp flavour has always had.
//
// The wrappers it aggregates are logos-qt-generator's
// (`--backend consumer --binding origin`); the two tools have to agree on a
// constructor signature, and these tests pin this side of it. The other side is
// pinned in logos-qt-sdk, which compiles both halves together
// (tests/qt-generator/fixtures/origin_umbrella_tu.cpp).

TEST(MakeUmbrellaTest, QtExplicitOriginIsDefaultConstructibleAndHoldsNoLogosApi)
{
    const QString h = makeUmbrellaHeaderFromDeps(depsMixedForms(), {}, ApiStyle::Qt,
                                                 "sample_module",
                                                 UmbrellaBinding::ExplicitOrigin);

    // Default-constructible: what `new LogosModules()` in the cdylib glue needs.
    EXPECT_TRUE(h.contains("LogosModules() : dep_a(QStringLiteral(\"sample_module\"))"))
        << h.toStdString();
    EXPECT_FALSE(h.contains("LogosAPI")) << h.toStdString();
    EXPECT_FALSE(h.contains("logos_api.h")) << h.toStdString();
    EXPECT_FALSE(h.contains("logos_api_client.h")) << h.toStdString();

    // Still the Qt type surface — same members, same PascalCase wrapper types,
    // same includes. Only the binding moved.
    EXPECT_TRUE(h.contains("DepA dep_a;")) << h.toStdString();
    EXPECT_TRUE(h.contains("DepB dep_b;")) << h.toStdString();
    EXPECT_TRUE(h.contains("DepC dep_c;")) << h.toStdString();
    EXPECT_TRUE(h.contains("#include \"dep_a_api.h\"")) << h.toStdString();
}

// THE assertion of the whole change: every origin the umbrella writes is the
// CONSUMING module's own name, stated as a literal. Not derived from an api
// object, not defaulted, not inherited from whoever constructed the umbrella.
//
// The trap this guards is specific and has been measured: `LpBridge::forTarget`
// reads the origin off `api->moduleName()`, so a wrapper built on a LogosAPI
// belonging to some OTHER module makes its calls under that module's identity
// and with its capabilities. A `bind_<iface>(...)` factory is where that would
// hide — it takes a name at runtime, and taking the WRONG one (the target's
// name reused as the origin, or a borrowed api) type-checks perfectly.
TEST(MakeUmbrellaTest, QtExplicitOriginStatesTheConsumersOwnNameEverywhere)
{
    const QString h = makeUmbrellaHeaderFromDeps(depsMixedForms(), {"some_iface"},
                                                 ApiStyle::Qt, "sample_module",
                                                 UmbrellaBinding::ExplicitOrigin);

    // Members: origin first, target baked into the wrapper itself.
    EXPECT_TRUE(h.contains("dep_b(QStringLiteral(\"sample_module\"))")) << h.toStdString();
    EXPECT_TRUE(h.contains("dep_c(QStringLiteral(\"sample_module\"))")) << h.toStdString();

    // Bind factories: origin is the CONSUMER (a literal), target is the
    // runtime argument. Both overloads, and in that order — swapping them would
    // make every bound call originate from the provider being bound to.
    EXPECT_TRUE(h.contains(
        "return SomeIface(QStringLiteral(\"sample_module\"), moduleName);"))
        << h.toStdString();
    EXPECT_TRUE(h.contains(
        "return SomeIface(QStringLiteral(\"sample_module\"), QString::fromStdString(moduleName));"))
        << h.toStdString();

    // Nothing anywhere passes an api, and nothing derives a name.
    EXPECT_FALSE(h.contains("(api")) << h.toStdString();
    EXPECT_FALSE(h.contains("moduleName()")) << h.toStdString();
}

// A module that cannot state its own name must not compile. Every origin would
// otherwise be the empty string, which is not "no identity" to the transport —
// it is a client authenticating as nobody, failing far from here and looking
// like a capability bug. The one thing the generator must never do is fill the
// gap by borrowing a name from somewhere.
TEST(MakeUmbrellaTest, QtExplicitOriginRefusesToGuessAnOrigin)
{
    const QString h = makeUmbrellaHeaderFromDeps(depsMixedForms(), {}, ApiStyle::Qt,
                                                 QString(), UmbrellaBinding::ExplicitOrigin);
    EXPECT_TRUE(h.contains("#error")) << h.toStdString();
    EXPECT_TRUE(h.contains("never derived or borrowed")) << h.toStdString();
}

// Additive, and asserted as such rather than assumed: the default binding IS
// the LogosAPI-threading umbrella, byte for byte. Every module in the tree
// compiles against that output today.
TEST(MakeUmbrellaTest, TheDefaultBindingIsTheLogosApiUmbrellaUnchanged)
{
    const QStringList ifaces{"some_iface"};
    const QString defaulted =
        makeUmbrellaHeaderFromDeps(depsMixedForms(), ifaces, ApiStyle::Qt, "sample_module");
    const QString explicitly =
        makeUmbrellaHeaderFromDeps(depsMixedForms(), ifaces, ApiStyle::Qt, "sample_module",
                                   UmbrellaBinding::FromApi);
    EXPECT_EQ(defaulted, explicitly);
    EXPECT_TRUE(defaulted.contains("explicit LogosModules(LogosAPI* api)")) << defaulted.toStdString();
    EXPECT_TRUE(defaulted.contains("return SomeIface(api, moduleName);")) << defaulted.toStdString();
}

// The Qt-free umbrella is origin-bound by construction, so the flag has nothing
// to say about it. Asserted rather than left implicit: an Lp branch that started
// reading `binding` would be a silent behaviour change for every universal and
// cdylib module in the tree.
TEST(MakeUmbrellaTest, LpIgnoresTheBindingFlag)
{
    const QString a = makeUmbrellaHeaderFromDeps(depsMixedForms(), {"some_iface"},
                                                 ApiStyle::Lp, "sample_module",
                                                 UmbrellaBinding::FromApi);
    const QString b = makeUmbrellaHeaderFromDeps(depsMixedForms(), {"some_iface"},
                                                 ApiStyle::Lp, "sample_module",
                                                 UmbrellaBinding::ExplicitOrigin);
    EXPECT_EQ(a, b);
    EXPECT_TRUE(a.contains("dep_a(\"sample_module\")")) << a.toStdString();
}
