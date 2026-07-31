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

// ---------------------------------------------------------------------------
// Optionality, header-first
//
// `std::optional<T>` used to fall through to the opaque `any` fallback with no
// diagnostic, so a header-first C++ provider could not express an optional at
// all: it declared one and published a contract that said something else.
// ---------------------------------------------------------------------------

TEST_F(ImplHeaderParserTest, StdOptionalBecomesOptional)
{
    auto r = parseImplHeader(
        fixturesDir() + "/optional_impl.h",
        "OptionalImpl",
        fixturesDir() + "/optional_metadata.json",
        err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    auto findType = [&](const char* n) -> const TypeDecl* {
        for (const auto& t : r.module.types)
            if (t.name == n) return &t;
        return nullptr;
    };
    auto findMethod = [&](const char* n) -> const MethodDecl* {
        for (const auto& m : r.module.methods)
            if (m.name == n) return &m;
        return nullptr;
    };

    const TypeDecl* profile = findType("Profile");
    ASSERT_NE(profile, nullptr);
    auto fieldNamed = [&](const char* n) -> const FieldDecl* {
        for (const auto& f : profile->fields)
            if (f.name == n) return &f;
        return nullptr;
    };

    // A plain member stays required.
    ASSERT_NE(fieldNamed("required"), nullptr);
    EXPECT_FALSE(fieldIsOptional(*fieldNamed("required")));

    // Optional members carry the value type, not `any`.
    ASSERT_NE(fieldNamed("nickname"), nullptr);
    EXPECT_TRUE(fieldIsOptional(*fieldNamed("nickname")));
    EXPECT_EQ(fieldValueType(*fieldNamed("nickname")).name, "tstr");
    EXPECT_EQ(fieldValueType(*fieldNamed("age")).name, "uint");
    EXPECT_EQ(fieldValueType(*fieldNamed("avatar")).name, "bstr");

    // Optional composes with a declared record — and `std::optional<Blob>` is
    // still a MENTION of Blob, so the record survives the
    // keep-only-referenced-records pass instead of being dropped as unused.
    ASSERT_NE(fieldNamed("blob"), nullptr);
    EXPECT_EQ(fieldValueType(*fieldNamed("blob")).kind, TypeExpr::Named);
    EXPECT_EQ(fieldValueType(*fieldNamed("blob")).name, "Blob");
    EXPECT_NE(findType("Blob"), nullptr);

    // Parameters and returns, and optional nested inside a container.
    const MethodDecl* echo = findMethod("echoOptional");
    ASSERT_NE(echo, nullptr);
    ASSERT_EQ(echo->params.size(), 1u);
    EXPECT_TRUE(paramIsOptional(echo->params[0]));
    EXPECT_EQ(paramValueType(echo->params[0]).name, "tstr");
    EXPECT_TRUE(typeIsOptional(echo->returnType));

    const MethodDecl* lst = findMethod("echoOptionalList");
    ASSERT_NE(lst, nullptr);
    ASSERT_EQ(lst->params.size(), 1u);
    EXPECT_EQ(lst->params[0].type.kind, TypeExpr::Array);
    ASSERT_EQ(lst->params[0].type.elements.size(), 1u);
    EXPECT_EQ(lst->params[0].type.elements[0].kind, TypeExpr::Optional);

    // A required method is untouched.
    const MethodDecl* req = findMethod("required");
    ASSERT_NE(req, nullptr);
    EXPECT_FALSE(paramIsOptional(req->params[0]));
    EXPECT_FALSE(typeIsOptional(req->returnType));

    // The event parameter, likewise.
    ASSERT_EQ(r.module.events.size(), 1u);
    ASSERT_EQ(r.module.events[0].params.size(), 2u);
    EXPECT_FALSE(paramIsOptional(r.module.events[0].params[0]));
    EXPECT_TRUE(paramIsOptional(r.module.events[0].params[1]));
}

// std::optional<std::optional<T>> has NO LIDL type: three C++ states over a
// two-state wire. It maps down to `?T` — which is what makes the author's own
// declaration stop compiling against the generated codec, deliberately — and
// says so here, rather than leaving a conversion error in generated code.
TEST_F(ImplHeaderParserTest, NestedOptionalCollapsesAndIsReported)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString hp = dir.filePath("nested_impl.h");
    {
        QFile f(hp);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(
            "#pragma once\n"
            "#include <optional>\n"
            "#include <string>\n"
            "struct Rec {\n"
            "    std::optional<std::optional<std::string>> collapsed;\n"
            "};\n"
            "class NestedImpl {\n"
            "public:\n"
            "    Rec echo(const Rec& v);\n"
            "};\n");
    }
    auto r = parseImplHeader(hp, "NestedImpl",
                             fixturesDir() + "/sample_metadata.json", err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    ASSERT_EQ(r.module.types.size(), 1u);
    ASSERT_EQ(r.module.types[0].fields.size(), 1u);
    const FieldDecl& f = r.module.types[0].fields[0];
    EXPECT_TRUE(fieldIsOptional(f));
    // Collapsed to ONE layer — the contract may not carry a third state.
    EXPECT_EQ(fieldValueType(f).kind, TypeExpr::Primitive);
    EXPECT_EQ(fieldValueType(f).name, "tstr");

    err.flush();
    EXPECT_TRUE(errOutput.contains("std::optional<std::optional<std::string>>"))
        << errOutput.toStdString();
    EXPECT_TRUE(errOutput.contains("no LIDL type")) << errOutput.toStdString();
}

// A one-class header written to a temp dir and parsed, for the cases that are
// about a single declaration rather than a whole fixture module.
namespace {

QString probeHeader(QTemporaryDir& dir, const QString& body)
{
    const QString hp = dir.filePath("probe_impl.h");
    QFile f(hp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    f.write(("#pragma once\n"
             "#include <logos_json.h>\n"
             "#include <cstdint>\n"
             "#include <map>\n"
             "#include <optional>\n"
             "#include <set>\n"
             "#include <string>\n"
             "#include <unordered_map>\n"
             "#include <utility>\n"
             "#include <vector>\n"
             + body).toUtf8());
    return hp;
}

} // namespace

// `nlohmann::json` reaches `any` only through the fallback, and `any` is the
// RIGHT answer for it — test_fullapi_cpp's echoAny / fireAnyEvent / anyEvent
// are the conformance matrix's `any` cells. Naming it is what lets the fallback
// become an error without taking them out.
TEST_F(ImplHeaderParserTest, NlohmannJsonIsAnyByName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString hp = probeHeader(dir,
        "class ProbeImpl {\n"
        "public:\n"
        "    nlohmann::json echoAny(const nlohmann::json& v);\n"
        "    bool fireAnyEvent(const nlohmann::json& v);\n"
        "logos_events:\n"
        "    void anyEvent(const nlohmann::json& v);\n"
        "};\n");
    auto r = parseImplHeader(hp, "ProbeImpl",
                             fixturesDir() + "/sample_metadata.json", err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();

    ASSERT_EQ(r.module.methods.size(), 2u);
    EXPECT_EQ(r.module.methods[0].returnType.name, "any");
    EXPECT_EQ(r.module.methods[0].params[0].type.name, "any");
    EXPECT_EQ(r.module.methods[1].params[0].type.name, "any");
    ASSERT_EQ(r.module.events.size(), 1u);
    EXPECT_EQ(r.module.events[0].params[0].type.name, "any");
}

// ---------------------------------------------------------------------------
// A C++ spelling with no LIDL type is a BUILD ERROR, not a silent `any`.
//
// cppTypeToLidl used to end with `// Fallback: treat as opaque` -> `any`, and
// `any` is admitted by every backend gate. So an unrecognised spelling was
// accepted in silence, published as `any`, and dispatched as a raw
// `lidlImpl().f(args.at(0))` with no decode and no check.
// ---------------------------------------------------------------------------

TEST_F(ImplHeaderParserTest, NarrowNumericIsRejectedWithTheWidening)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString hp = probeHeader(dir,
        "class ProbeImpl {\n"
        "public:\n"
        "    int64_t f(uint32_t depth);\n"
        "};\n");
    ASSERT_FALSE(hp.isEmpty());

    auto r = parseImplHeader(hp, "ProbeImpl",
                             fixturesDir() + "/sample_metadata.json", err);
    ASSERT_TRUE(r.hasError()) << "uint32_t was admitted as `any`";
    // Names the declaration, the offending type, and what to write instead.
    EXPECT_TRUE(r.error.contains("method 'f': parameter 'depth'")) << r.error.toStdString();
    EXPECT_TRUE(r.error.contains("`uint32_t`")) << r.error.toStdString();
    EXPECT_TRUE(r.error.contains("`uint64_t`")) << r.error.toStdString();
}

// The offender may be nested. Report BOTH: the element with no LIDL type, and
// the declaration that carries it.
TEST_F(ImplHeaderParserTest, NestedOffenderNamesTheDeclarationToo)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString hp = probeHeader(dir,
        "class ProbeImpl {\n"
        "public:\n"
        "    int64_t f(const std::vector<uint32_t>& instruction);\n"
        "};\n");
    auto r = parseImplHeader(hp, "ProbeImpl",
                             fixturesDir() + "/sample_metadata.json", err);
    ASSERT_TRUE(r.hasError());
    EXPECT_TRUE(r.error.contains("const std::vector<uint32_t>&")) << r.error.toStdString();
    EXPECT_TRUE(r.error.contains("`uint32_t`")) << r.error.toStdString();
}

// Each family gets a hint that names a replacement. A diagnostic without one
// just moves the guesswork.
TEST_F(ImplHeaderParserTest, EachUnsupportedFamilyNamesItsReplacement)
{
    struct Case { const char* decl; const char* mentions; };
    const Case cases[] = {
        {"int64_t f(float v);",                                      "`double`"},
        {"int64_t f(uint8_t v);",                                    "std::vector<uint8_t>"},
        {"int64_t f(size_t v);",                                     "`uint64_t`"},
        {"int64_t f(const std::set<std::string>& v);",               "std::vector<T>"},
        {"int64_t f(const std::pair<std::string, std::string>& v);", "struct"},
        {"int64_t f(const std::map<int64_t, std::string>& v);",      "`tstr`"},
    };
    for (const Case& c : cases) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString hp = probeHeader(dir,
            QString("class ProbeImpl {\npublic:\n    %1\n};\n").arg(c.decl));
        QString e;
        QTextStream es(&e);
        auto r = parseImplHeader(hp, "ProbeImpl",
                                 fixturesDir() + "/sample_metadata.json", es);
        ASSERT_TRUE(r.hasError()) << c.decl;
        EXPECT_TRUE(r.error.contains(c.mentions))
            << c.decl << "\n" << r.error.toStdString();
    }
}

// std::unordered_map<std::string, T> is the second C++ spelling of `{tstr: T}`.
// logos_codec.h has always specialized Codec for it; the parser had not, so it
// published `any`.
TEST_F(ImplHeaderParserTest, UnorderedMapIsAStringKeyedMap)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString hp = probeHeader(dir,
        "class ProbeImpl {\n"
        "public:\n"
        "    int64_t f(const std::unordered_map<std::string, std::string>& m);\n"
        "};\n");
    auto r = parseImplHeader(hp, "ProbeImpl",
                             fixturesDir() + "/sample_metadata.json", err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();
    ASSERT_EQ(r.module.methods.size(), 1u);
    const TypeExpr& t = r.module.methods[0].params[0].type;
    EXPECT_EQ(t.kind, TypeExpr::Map);
    ASSERT_EQ(t.elements.size(), 2u);
    EXPECT_EQ(t.elements[0].name, "tstr");
    EXPECT_EQ(t.elements[1].name, "tstr");
}

// ...except in the two slots whose C++ spelling the generator WRITES OUT — a
// record field's codec and an event's generated body. There it has to pick one
// container name, and picking the wrong one is a compile error in code the
// author never wrote. Say so at the declaration instead.
TEST_F(ImplHeaderParserTest, UnorderedMapIsRejectedWhereTheSpellingIsEmitted)
{
    for (const char* body : {
        "struct Rec {\n"
        "    std::unordered_map<std::string, std::string> m;\n"
        "};\n"
        "class ProbeImpl {\npublic:\n    Rec echo(const Rec& v);\n};\n",

        "class ProbeImpl {\n"
        "public:\n"
        "    bool fire();\n"
        "logos_events:\n"
        "    void changed(const std::unordered_map<std::string, std::string>& m);\n"
        "};\n"}) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString hp = probeHeader(dir, body);
        QString e;
        QTextStream es(&e);
        auto r = parseImplHeader(hp, "ProbeImpl",
                                 fixturesDir() + "/sample_metadata.json", es);
        ASSERT_TRUE(r.hasError()) << body;
        EXPECT_TRUE(r.error.contains("std::map<std::string, T>")) << r.error.toStdString();
    }
}

// A helper struct the API never mentions is dropped from the contract, so an
// unsupported spelling INSIDE it promises nothing and must not fail the build.
// Publishing is what makes a declaration's type a promise.
TEST_F(ImplHeaderParserTest, UnreferencedHelperStructDoesNotFailTheBuild)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString hp = probeHeader(dir,
        "struct PendingAction {\n"
        "    uint32_t attempts;\n"
        "};\n"
        "class ProbeImpl {\n"
        "public:\n"
        "    int64_t f(int64_t n);\n"
        "};\n");
    auto r = parseImplHeader(hp, "ProbeImpl",
                             fixturesDir() + "/sample_metadata.json", err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();
    EXPECT_TRUE(r.module.types.empty());

    // ...and the same struct DOES fail once a method publishes it.
    QTemporaryDir dir2;
    ASSERT_TRUE(dir2.isValid());
    const QString hp2 = probeHeader(dir2,
        "struct PendingAction {\n"
        "    uint32_t attempts;\n"
        "};\n"
        "class ProbeImpl {\n"
        "public:\n"
        "    PendingAction f(int64_t n);\n"
        "};\n");
    QString e2;
    QTextStream es2(&e2);
    auto r2 = parseImplHeader(hp2, "ProbeImpl",
                              fixturesDir() + "/sample_metadata.json", es2);
    ASSERT_TRUE(r2.hasError());
    EXPECT_TRUE(r2.error.contains("type 'PendingAction': field 'attempts'"))
        << r2.error.toStdString();
}

// The reserved LogosModuleContext hooks are framework plumbing, not contract.
// They are dropped after parsing, so their spellings are not a contract defect.
TEST_F(ImplHeaderParserTest, ReservedHookSpellingsAreNotContractDefects)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString hp = probeHeader(dir,
        "class ProbeImpl {\n"
        "public:\n"
        "    void onContextReady(uint32_t generation);\n"
        "    int64_t f(int64_t n);\n"
        "};\n");
    auto r = parseImplHeader(hp, "ProbeImpl",
                             fixturesDir() + "/sample_metadata.json", err);
    ASSERT_FALSE(r.hasError()) << r.error.toStdString();
    ASSERT_EQ(r.module.methods.size(), 1u);
    EXPECT_EQ(r.module.methods[0].name, "f");
}
