#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <algorithm>
#include "impl_header_parser.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

// Helper: find the fixtures directory.
// 1. FIXTURES_DIR env var — set by CI to point to installed fixtures
// 2. FIXTURES_DIR compile define — set by CMake, works during ctest in nix sandbox
// 3. ../fixtures relative to binary — nix install layout ($out/bin/ + $out/fixtures/)
static QString fixturesDir()
{
    // Environment variable takes priority (set by CI or user)
    QByteArray envDir = qgetenv("FIXTURES_DIR");
    if (!envDir.isEmpty() && QDir(envDir).exists())
        return QString::fromUtf8(envDir);

#ifdef FIXTURES_DIR
    if (QDir(FIXTURES_DIR).exists())
        return QString(FIXTURES_DIR);
#endif

    // Installed layout: $out/bin/experimental_tests + $out/fixtures/
    QString binDir = QCoreApplication::applicationDirPath();
    if (!binDir.isEmpty()) {
        QString installed = QDir::cleanPath(binDir + "/../fixtures");
        if (QDir(installed).exists())
            return installed;
    }
    return QDir::currentPath() + "/fixtures";
}

class ImplHeaderParserTest : public ::testing::Test {
protected:
    QString errOutput;
    QTextStream err{&errOutput};
};

// ---------------------------------------------------------------------------
// Basic parsing
// ---------------------------------------------------------------------------

TEST_F(ImplHeaderParserTest, ParsesSampleImpl)
{
    auto r = parseImplHeader(
        fixturesDir() + "/sample_impl.h",
        "SampleModuleImpl",
        fixturesDir() + "/sample_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    // Metadata from JSON
    EXPECT_EQ(r.module.name, "sample_module");
    EXPECT_EQ(r.module.version, "1.2.3");
    EXPECT_EQ(r.module.description, "A sample module for testing");
    EXPECT_EQ(r.module.category, "testing");
    ASSERT_EQ(r.module.depends.size(), 2);
    EXPECT_EQ(r.module.depends[0], "dep_a");

    // Methods — should find all public methods, skip ctor/dtor/private
    EXPECT_GE(r.module.methods.size(), 10);
}

// A dependency entry may carry the constraints an installer resolves it by,
// and generation still needs the name. Read as a plain string, an object entry
// came back empty and the module it names vanished from the generated
// LogosModules aggregate, so every call through it failed to compile.
TEST_F(ImplHeaderParserTest, ReadsDependenciesDeclaredInObjectForm)
{
    auto r = parseImplHeader(
        fixturesDir() + "/sample_impl.h",
        "SampleModuleImpl",
        fixturesDir() + "/object_deps_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    ASSERT_EQ(r.module.depends.size(), 3);
    EXPECT_EQ(r.module.depends[0], "dep_a");
    EXPECT_EQ(r.module.depends[1], "dep_b");
    EXPECT_EQ(r.module.depends[2], "dep_c");
}

TEST_F(ImplHeaderParserTest, MethodTypes)
{
    auto r = parseImplHeader(
        fixturesDir() + "/sample_impl.h",
        "SampleModuleImpl",
        fixturesDir() + "/sample_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    // Find specific methods and check their types
    auto findMethod = [&](const std::string& name) -> const MethodDecl* {
        for (const auto& m : r.module.methods)
            if (m.name == name) return &m;
        return nullptr;
    };

    // std::string greet(const std::string& name) → tstr
    auto greet = findMethod("greet");
    ASSERT_NE(greet, nullptr);
    EXPECT_EQ(greet->returnType.name, "tstr");
    ASSERT_EQ(greet->params.size(), 1);
    EXPECT_EQ(greet->params[0].name, "name");
    EXPECT_EQ(greet->params[0].type.name, "tstr");

    // bool isValid(const std::string& input) → bool
    auto isValid = findMethod("isValid");
    ASSERT_NE(isValid, nullptr);
    EXPECT_EQ(isValid->returnType.name, "bool");

    // int64_t getCount() → int
    auto getCount = findMethod("getCount");
    ASSERT_NE(getCount, nullptr);
    EXPECT_EQ(getCount->returnType.name, "int");
    EXPECT_TRUE(getCount->params.empty());

    // uint64_t getSize() → uint
    auto getSize = findMethod("getSize");
    ASSERT_NE(getSize, nullptr);
    EXPECT_EQ(getSize->returnType.name, "uint");

    // double getScore() → float64
    auto getScore = findMethod("getScore");
    ASSERT_NE(getScore, nullptr);
    EXPECT_EQ(getScore->returnType.name, "float64");

    // void doNothing() → void
    auto doNothing = findMethod("doNothing");
    ASSERT_NE(doNothing, nullptr);
    EXPECT_EQ(doNothing->returnType.name, "void");

    // std::vector<std::string> getNames() → [tstr]
    auto getNames = findMethod("getNames");
    ASSERT_NE(getNames, nullptr);
    EXPECT_EQ(getNames->returnType.kind, TypeExpr::Array);
    EXPECT_EQ(getNames->returnType.elements[0].name, "tstr");

    // std::vector<uint8_t> getData() → bstr
    auto getData = findMethod("getData");
    ASSERT_NE(getData, nullptr);
    EXPECT_EQ(getData->returnType.name, "bstr");

    // std::vector<int64_t> getIds() → [int]
    auto getIds = findMethod("getIds");
    ASSERT_NE(getIds, nullptr);
    EXPECT_EQ(getIds->returnType.kind, TypeExpr::Array);
    EXPECT_EQ(getIds->returnType.elements[0].name, "int");

    // std::string combine(const std::string& a, const std::string& b, int64_t count)
    auto combine = findMethod("combine");
    ASSERT_NE(combine, nullptr);
    EXPECT_EQ(combine->returnType.name, "tstr");
    ASSERT_EQ(combine->params.size(), 3);
    EXPECT_EQ(combine->params[0].name, "a");
    EXPECT_EQ(combine->params[1].name, "b");
    EXPECT_EQ(combine->params[2].name, "count");
    EXPECT_EQ(combine->params[2].type.name, "int");
}

TEST_F(ImplHeaderParserTest, SkipsPrivateMethods)
{
    auto r = parseImplHeader(
        fixturesDir() + "/sample_impl.h",
        "SampleModuleImpl",
        fixturesDir() + "/sample_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    for (const auto& m : r.module.methods) {
        EXPECT_NE(m.name, "internalHelper") << "Private method should not be parsed";
    }
}

// ---------------------------------------------------------------------------
// Empty class
// ---------------------------------------------------------------------------

TEST_F(ImplHeaderParserTest, EmptyClass)
{
    auto r = parseImplHeader(
        fixturesDir() + "/empty_class_impl.h",
        "EmptyClassImpl",
        fixturesDir() + "/empty_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();
    EXPECT_TRUE(r.module.methods.empty());
    // Should have a warning in err output
    EXPECT_TRUE(errOutput.contains("Warning"));
}

// ---------------------------------------------------------------------------
// Complex class with access specifier changes
// ---------------------------------------------------------------------------

TEST_F(ImplHeaderParserTest, ComplexAccessSpecifiers)
{
    auto r = parseImplHeader(
        fixturesDir() + "/complex_impl.h",
        "ComplexModuleImpl",
        fixturesDir() + "/empty_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    auto findMethod = [&](const std::string& name) -> const MethodDecl* {
        for (const auto& m : r.module.methods)
            if (m.name == name) return &m;
        return nullptr;
    };

    // First public section
    EXPECT_NE(findMethod("firstMethod"), nullptr);
    // Second public section (after protected)
    EXPECT_NE(findMethod("secondMethod"), nullptr);
    EXPECT_NE(findMethod("thirdMethod"), nullptr);
    // Protected method should be skipped
    EXPECT_EQ(findMethod("protectedHelper"), nullptr);
    // Private method should be skipped
    EXPECT_EQ(findMethod("privateHelper"), nullptr);
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

// A struct in an impl header becomes a contract `type` — but ONLY if the API
// mentions it. A header routinely declares private helpers, and publishing
// those would change the module's interface as a side effect of an internal
// refactor. Verified against two real modules: openmetrics' `ModuleSource` and
// the package manager's in-class `PendingAction` were both being published.
TEST_F(ImplHeaderParserTest, OnlyApiReferencedStructsBecomeRecords)
{
    auto r = parseImplHeader(
        fixturesDir() + "/records_impl.h",
        "RecordsImpl",
        fixturesDir() + "/records_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    std::vector<std::string> names;
    for (const auto& t : r.module.types) names.push_back(t.name);
    std::sort(names.begin(), names.end());

    // Blob is named directly; Wrapper too. Internal and Helper are not.
    ASSERT_EQ(names.size(), 2u) << "published: " << [&]{
        std::string j; for (const auto& n : names) j += n + " "; return j; }();
    EXPECT_EQ(names[0], "Blob");
    EXPECT_EQ(names[1], "Wrapper");

    // A field with a trailing comment must NOT be silently dropped: a record
    // published with a partial field list looks like a contract and is not one.
    for (const auto& t : r.module.types) {
        if (t.name != "Blob") continue;
        ASSERT_EQ(t.fields.size(), 3u);
        EXPECT_EQ(t.fields[2].name, "payload");
        EXPECT_EQ(t.fields[2].type.name, "bstr");
    }
}

// The closure is transitive: a record reaches the contract because something
// the API names refers to it, however indirectly.
TEST_F(ImplHeaderParserTest, RecordsReachableOnlyThroughAnotherRecordAreKept)
{
    auto r = parseImplHeader(
        fixturesDir() + "/records_impl.h",
        "RecordsImpl",
        fixturesDir() + "/records_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    // Wrapper's own fields name Blob; both survive even though a signature
    // could have named only one of them.
    bool sawWrapper = false;
    for (const auto& t : r.module.types) {
        if (t.name != "Wrapper") continue;
        sawWrapper = true;
        ASSERT_EQ(t.fields.size(), 2u);
        EXPECT_EQ(t.fields[0].type.name, "Blob");
        EXPECT_EQ(t.fields[1].type.elements.at(0).name, "Blob");
    }
    EXPECT_TRUE(sawWrapper);
}

TEST_F(ImplHeaderParserTest, MissingHeaderFile)
{
    auto r = parseImplHeader(
        "/nonexistent/path.h",
        "Foo",
        fixturesDir() + "/sample_metadata.json",
        err);
    EXPECT_TRUE(r.hasError());
    EXPECT_TRUE(r.error.contains("Failed to open header"));
}

TEST_F(ImplHeaderParserTest, MissingMetadataFile)
{
    auto r = parseImplHeader(
        fixturesDir() + "/sample_impl.h",
        "SampleModuleImpl",
        "/nonexistent/metadata.json",
        err);
    EXPECT_TRUE(r.hasError());
    EXPECT_TRUE(r.error.contains("Failed to open metadata"));
}

TEST_F(ImplHeaderParserTest, WrongClassName)
{
    auto r = parseImplHeader(
        fixturesDir() + "/sample_impl.h",
        "NonExistentClass",
        fixturesDir() + "/sample_metadata.json",
        err);
    // Not an error per se, but should find zero methods and warn
    ASSERT_FALSE(r.hasError());
    EXPECT_TRUE(r.module.methods.empty());
    EXPECT_TRUE(errOutput.contains("Warning"));
}

// ---------------------------------------------------------------------------
// LogosMap / LogosList, Qt collections, metadata events, emitEvent detection
// ---------------------------------------------------------------------------

TEST_F(ImplHeaderParserTest, UniversalTypesAndMetadataEvents)
{
    auto r = parseImplHeader(
        fixturesDir() + "/universal_impl.h",
        "UniversalImpl",
        fixturesDir() + "/universal_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    EXPECT_EQ(r.module.name, "universal_mod");
    EXPECT_EQ(r.module.version, "2.0.0");

    ASSERT_EQ(r.module.events.size(), 1);
    EXPECT_EQ(r.module.events[0].name, "onReady");
    // Optional per-event description carried from metadata.json events[].
    EXPECT_EQ(r.module.events[0].description, "Fired once the module is ready.");
    ASSERT_EQ(r.module.events[0].params.size(), 1);
    EXPECT_EQ(r.module.events[0].params[0].name, "info");
    EXPECT_EQ(r.module.events[0].params[0].type.name, "tstr");

    auto findMethod = [&](const std::string& name) -> const MethodDecl* {
        for (const auto& m : r.module.methods)
            if (m.name == name) return &m;
        return nullptr;
    };

    // The fixture declares a `std::function<…> emitEvent` member. The old
    // legacy hook treated it specially; now such members are simply skipped
    // and never mistaken for a callable method.
    EXPECT_EQ(findMethod("emitEvent"), nullptr);

    auto fetchMap = findMethod("fetchMap");
    ASSERT_NE(fetchMap, nullptr);
    EXPECT_EQ(fetchMap->returnType.kind, TypeExpr::Map);
    EXPECT_TRUE(fetchMap->jsonReturn);

    auto fetchList = findMethod("fetchList");
    ASSERT_NE(fetchList, nullptr);
    EXPECT_EQ(fetchList->returnType.kind, TypeExpr::Array);
    EXPECT_EQ(fetchList->returnType.elements[0].name, "any");
    EXPECT_TRUE(fetchList->jsonReturn);

    auto asVariantMap = findMethod("asVariantMap");
    ASSERT_NE(asVariantMap, nullptr);
    EXPECT_EQ(asVariantMap->returnType.kind, TypeExpr::Map);
    EXPECT_FALSE(asVariantMap->jsonReturn);

    auto listNames = findMethod("listNames");
    ASSERT_NE(listNames, nullptr);
    EXPECT_EQ(listNames->returnType.kind, TypeExpr::Array);
    EXPECT_EQ(listNames->returnType.elements[0].name, "tstr");
    EXPECT_FALSE(listNames->jsonReturn);

    auto anyList = findMethod("anyList");
    ASSERT_NE(anyList, nullptr);
    EXPECT_EQ(anyList->returnType.kind, TypeExpr::Array);
    EXPECT_EQ(anyList->returnType.elements[0].name, "any");
    EXPECT_FALSE(anyList->jsonReturn);

    auto fetchResult = findMethod("fetchResult");
    ASSERT_NE(fetchResult, nullptr);
    EXPECT_EQ(fetchResult->returnType.kind, TypeExpr::Primitive);
    EXPECT_EQ(fetchResult->returnType.name, "result");
    EXPECT_FALSE(fetchResult->jsonReturn);
    EXPECT_TRUE(fetchResult->resultReturn);

    auto fetchResultNodiscard = findMethod("fetchResultNodiscard");
    ASSERT_NE(fetchResultNodiscard, nullptr);
    EXPECT_EQ(fetchResultNodiscard->returnType.name, "result");
    EXPECT_TRUE(fetchResultNodiscard->resultReturn);

    auto fetchResultStatic = findMethod("fetchResultStatic");
    ASSERT_NE(fetchResultStatic, nullptr);
    EXPECT_EQ(fetchResultStatic->returnType.name, "result");
    EXPECT_TRUE(fetchResultStatic->resultReturn);

    auto fetchResultNodiscardStatic = findMethod("fetchResultNodiscardStatic");
    ASSERT_NE(fetchResultNodiscardStatic, nullptr);
    EXPECT_EQ(fetchResultNodiscardStatic->returnType.name, "result");
    EXPECT_TRUE(fetchResultNodiscardStatic->resultReturn);

    auto fetchResultStaticNodiscard = findMethod("fetchResultStaticNodiscard");
    ASSERT_NE(fetchResultStaticNodiscard, nullptr);
    EXPECT_EQ(fetchResultStaticNodiscard->returnType.name, "result");
    EXPECT_TRUE(fetchResultStaticNodiscard->resultReturn);

    auto fetchResultMultiAttr = findMethod("fetchResultMultiAttr");
    ASSERT_NE(fetchResultMultiAttr, nullptr);
    EXPECT_EQ(fetchResultMultiAttr->returnType.name, "result");
    EXPECT_TRUE(fetchResultMultiAttr->resultReturn);

    auto fetchResultInlineStatic = findMethod("fetchResultInlineStatic");
    ASSERT_NE(fetchResultInlineStatic, nullptr);
    EXPECT_EQ(fetchResultInlineStatic->returnType.name, "result");
    EXPECT_TRUE(fetchResultInlineStatic->resultReturn);

    auto fetchResultConsteval = findMethod("fetchResultConsteval");
    ASSERT_NE(fetchResultConsteval, nullptr);
    EXPECT_EQ(fetchResultConsteval->returnType.name, "result");
    EXPECT_TRUE(fetchResultConsteval->resultReturn);

    for (const auto& m : r.module.methods) {
        EXPECT_NE(m.name, "void") << "Keyword should not appear as method name";
    }
}

// ---------------------------------------------------------------------------
// Event doc comments: `///` above a `logos_events:` declaration becomes the
// event's description (same capture rules as methods: doc-comments only,
// adjacent-only, multi-line joined with \n).
// ---------------------------------------------------------------------------

TEST_F(ImplHeaderParserTest, EventDocCommentsFromHeader)
{
    auto r = parseImplHeader(
        fixturesDir() + "/documented_events_impl.h",
        "DocumentedEventsImpl",
        fixturesDir() + "/documented_events_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    ASSERT_EQ(r.module.events.size(), 3);

    // Multi-line `///` doc comment: the two lines are joined with a newline.
    EXPECT_EQ(r.module.events[0].name, "userLoggedIn");
    EXPECT_EQ(r.module.events[0].description,
              "Fired once the user has authenticated.\n"
              "Carries the freshly issued session token.");
    ASSERT_EQ(r.module.events[0].params.size(), 2);
    EXPECT_EQ(r.module.events[0].params[0].name, "userId");
    EXPECT_EQ(r.module.events[0].params[1].name, "token");

    // A plain `//` comment is not a doc comment → no description captured.
    EXPECT_EQ(r.module.events[1].name, "heartbeat");
    EXPECT_TRUE(r.module.events[1].description.empty());

    // Single-line `///` doc comment.
    EXPECT_EQ(r.module.events[2].name, "shutdown");
    EXPECT_EQ(r.module.events[2].description, "Single-line documented event.");
}

// ---------------------------------------------------------------------------
// Issue #76: a section specifier and a declaration on the *same* physical
// line (`logos_events : void foo();`, as clang-format / prettier produce)
// must be parsed identically to the newline-separated form. The code after
// the colon must not be discarded.
// ---------------------------------------------------------------------------

TEST_F(ImplHeaderParserTest, SameLineSectionSpecifiers)
{
    auto r = parseImplHeader(
        fixturesDir() + "/same_line_events_impl.h",
        "SameLineEventsImpl",
        fixturesDir() + "/same_line_events_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    auto findEvent = [&](const std::string& name) -> const EventDecl* {
        for (const auto& e : r.module.events)
            if (e.name == name) return &e;
        return nullptr;
    };
    auto findMethod = [&](const std::string& name) -> const MethodDecl* {
        for (const auto& m : r.module.methods)
            if (m.name == name) return &m;
        return nullptr;
    };

    // The exact prettier form from the issue:
    //     logos_events : void versionReady(const std::string &version);
    // Previously the prototype after the colon was discarded entirely.
    const EventDecl* versionReady = findEvent("versionReady");
    ASSERT_NE(versionReady, nullptr)
        << "Same-line `logos_events :` prototype must still be parsed";
    ASSERT_EQ(versionReady->params.size(), 1);
    EXPECT_EQ(versionReady->params[0].name, "version");
    EXPECT_EQ(versionReady->params[0].type.name, "tstr");
    // The `///` doc comment above the collapsed line must attach: in the
    // same-line form there is nowhere else for it to go, so documentation
    // must not be formatting-dependent either.
    EXPECT_EQ(versionReady->description, "Fired once the latest version is known.");

    // An event declared after the section is already open, also same-line.
    const EventDecl* downloadProgress = findEvent("downloadProgress");
    ASSERT_NE(downloadProgress, nullptr);
    ASSERT_EQ(downloadProgress->params.size(), 2);
    EXPECT_EQ(downloadProgress->params[0].name, "id");
    EXPECT_EQ(downloadProgress->params[0].type.name, "tstr");
    EXPECT_EQ(downloadProgress->params[1].name, "percent");
    EXPECT_EQ(downloadProgress->params[1].type.name, "int");

    // The newline-separated form keeps working alongside the collapsed form.
    EXPECT_NE(findEvent("shutdown"), nullptr);

    // Exactly the three events above — no phantom or dropped entries.
    EXPECT_EQ(r.module.events.size(), 3);

    // The symmetric case: `public : <decl>` on one line must surface the
    // method too (the access specifier no longer swallows the declaration).
    const MethodDecl* greet = findMethod("greet");
    ASSERT_NE(greet, nullptr)
        << "Same-line `public:` declaration must still be parsed";
    EXPECT_EQ(greet->returnType.name, "tstr");
    ASSERT_EQ(greet->params.size(), 1);
    EXPECT_EQ(greet->params[0].name, "name");
    EXPECT_EQ(greet->params[0].type.name, "tstr");

    // The same-line events must land in events[], never leak into methods[].
    EXPECT_EQ(findMethod("versionReady"), nullptr);
    EXPECT_EQ(findMethod("downloadProgress"), nullptr);
}


TEST_F(ImplHeaderParserTest, ParsesMultiLineSignature)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString hp = dir.filePath("ml_impl.h");
    {
        QFile f(hp);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(
            "#pragma once\n"
            "#include <string>\n"
            "class MlImpl {\n"
            "public:\n"
            "    std::string single(const std::string& a);\n"
            "    std::string wrapped(const std::string& first,\n"
            "                        const std::string& second);\n"
            "};\n");
    }
    auto r = parseImplHeader(hp, "MlImpl",
                             fixturesDir() + "/sample_metadata.json", err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    QStringList names;
    for (const auto& m : r.module.methods) names << QString::fromStdString(m.name);
    EXPECT_TRUE(names.contains("single"));
    EXPECT_TRUE(names.contains("wrapped"))
        << "got: " << names.join(",").toStdString();
}
