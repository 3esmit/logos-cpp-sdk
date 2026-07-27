#include <gtest/gtest.h>
#include "lidl_gen_client.h"

static ModuleDecl makeTestModule()
{
    ModuleDecl m;
    m.name = "wallet_module";
    m.version = "1.0.0";
    m.description = "Wallet";
    m.category = "finance";
    m.depends.push_back("crypto");

    {
        MethodDecl md;
        md.name = "createAccount";
        md.returnType = { TypeExpr::Primitive, "tstr", {} };
        ParamDecl p; p.name = "passphrase"; p.type = { TypeExpr::Primitive, "tstr", {} };
        md.params.push_back(p);
        m.methods.push_back(md);
    }
    {
        MethodDecl md;
        md.name = "getBalance";
        md.returnType = { TypeExpr::Primitive, "uint", {} };
        ParamDecl p; p.name = "address"; p.type = { TypeExpr::Primitive, "tstr", {} };
        md.params.push_back(p);
        m.methods.push_back(md);
    }
    {
        MethodDecl md;
        md.name = "listAccounts";
        TypeExpr elem = { TypeExpr::Primitive, "tstr", {} };
        md.returnType = { TypeExpr::Array, "", { elem } };
        m.methods.push_back(md);
    }

    EventDecl ed;
    ed.name = "onTransfer";
    ParamDecl ep; ep.name = "hash"; ep.type = { TypeExpr::Primitive, "tstr", {} };
    ed.params.push_back(ep);
    m.events.push_back(ed);

    return m;
}

// ---------------------------------------------------------------------------
// Header generation
// ---------------------------------------------------------------------------

TEST(LidlGenClient, HeaderHasClassName)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("class WalletModule {"));
}

TEST(LidlGenClient, HeaderHasConstructor)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("explicit WalletModule(LogosAPI* api)"));
}

TEST(LidlGenClient, HeaderHasSyncMethods)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("QString createAccount("));
    EXPECT_TRUE(h.contains("qulonglong getBalance("));
    EXPECT_TRUE(h.contains("QStringList listAccounts("));
}

TEST(LidlGenClient, HeaderHasAsyncMethods)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("void createAccountAsync("));
    EXPECT_TRUE(h.contains("void getBalanceAsync("));
    EXPECT_TRUE(h.contains("void listAccountsAsync("));
}

TEST(LidlGenClient, HeaderHasEventMethods)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("bool on(const QString& eventName"));
    EXPECT_TRUE(h.contains("void trigger(const QString& eventName"));
    EXPECT_TRUE(h.contains("void setEventSource(LogosObject* source)"));
}

TEST(LidlGenClient, HeaderHasIncludes)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("#include \"logos_api.h\""));
    EXPECT_TRUE(h.contains("#include \"logos_api_client.h\""));
    EXPECT_TRUE(h.contains("#include \"logos_types.h\""));
}

TEST(LidlGenClient, HeaderHasPrivateMembers)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("LogosAPI* m_api"));
    EXPECT_TRUE(h.contains("LogosAPIClient* m_client"));
    EXPECT_TRUE(h.contains("QString m_moduleName"));
}

// ---------------------------------------------------------------------------
// Source generation
// ---------------------------------------------------------------------------

TEST(LidlGenClient, SourceHasConstructor)
{
    auto m = makeTestModule();
    QString s = lidlMakeSource(m);
    EXPECT_TRUE(s.contains("WalletModule::WalletModule(LogosAPI* api)"));
    EXPECT_TRUE(s.contains("getClient(\"wallet_module\")"));
}

TEST(LidlGenClient, SourceHasSyncImplementations)
{
    auto m = makeTestModule();
    QString s = lidlMakeSource(m);
    EXPECT_TRUE(s.contains("WalletModule::createAccount("));
    EXPECT_TRUE(s.contains("invokeRemoteMethod(\"wallet_module\", \"createAccount\""));
}

TEST(LidlGenClient, SourceHasAsyncImplementations)
{
    auto m = makeTestModule();
    QString s = lidlMakeSource(m);
    EXPECT_TRUE(s.contains("WalletModule::createAccountAsync("));
    EXPECT_TRUE(s.contains("invokeRemoteMethodAsync(\"wallet_module\", \"createAccount\""));
}

TEST(LidlGenClient, SourceHasEventBoilerplate)
{
    auto m = makeTestModule();
    QString s = lidlMakeSource(m);
    EXPECT_TRUE(s.contains("WalletModule::on(const QString& eventName"));
    EXPECT_TRUE(s.contains("WalletModule::trigger(const QString& eventName"));
    EXPECT_TRUE(s.contains("ensureReplica()"));
}

TEST(LidlGenClient, SourceHasReturnConversion)
{
    auto m = makeTestModule();
    QString s = lidlMakeSource(m);
    // createAccount returns tstr → QString, should use .toString()
    EXPECT_TRUE(s.contains("_result.toString()"));
    // getBalance returns uint → qulonglong, so the accessor must be the 64-bit
    // unsigned one; toInt() truncated and re-signed it.
    EXPECT_TRUE(s.contains("_result.toULongLong()"));
    // listAccounts returns [tstr] → QStringList, should use .toStringList()
    EXPECT_TRUE(s.contains("_result.toStringList()"));
}

// ---------------------------------------------------------------------------
// Metadata JSON generation
// ---------------------------------------------------------------------------

TEST(LidlGenClient, MetadataJson)
{
    auto m = makeTestModule();
    QString json = lidlGenerateMetadataJson(m);
    EXPECT_TRUE(json.contains("\"name\": \"wallet_module\""));
    EXPECT_TRUE(json.contains("\"version\": \"1.0.0\""));
    EXPECT_TRUE(json.contains("\"category\": \"finance\""));
    EXPECT_TRUE(json.contains("\"crypto\""));
}

TEST(LidlGenClient, MetadataJsonDefaults)
{
    ModuleDecl m;
    m.name = "bare";
    QString json = lidlGenerateMetadataJson(m);
    EXPECT_TRUE(json.contains("\"version\": \"0.0.0\""));
    EXPECT_TRUE(json.contains("\"category\": \"general\""));
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(LidlGenClient, MethodWithManyParams)
{
    ModuleDecl m;
    m.name = "multi";
    MethodDecl md;
    md.name = "bigMethod";
    md.returnType = { TypeExpr::Primitive, "tstr", {} };
    for (int i = 0; i < 7; ++i) {
        ParamDecl p;
        p.name = QString("p%1").arg(i).toStdString();
        p.type = { TypeExpr::Primitive, "tstr", {} };
        md.params.push_back(p);
    }
    m.methods.push_back(md);

    QString s = lidlMakeSource(m);
    // Args are packed via packVariantList (one QVariant element per arg),
    // regardless of arity — never a braced/`<<` list that would spread a
    // QVariantList-typed arg.
    EXPECT_TRUE(s.contains("packVariantList(p0, p1, p2, p3, p4, p5, p6)"));
}

// Regression: a QVariantList-typed ([any]/[int]/...) argument must be packed as
// ONE element, not concatenated into the args list. `QVariantList{v}` and
// `QVariantList() << v` both spread a QVariantList; packVariantList wraps each
// arg with QVariant::fromValue, so a single list arg stays a single arg.
TEST(LidlGenClient, ListArgIsPackedAsOneElement)
{
    ModuleDecl m;
    m.name = "arrs";
    MethodDecl md;
    md.name = "echoList";
    TypeExpr elem = { TypeExpr::Primitive, "any", {} };
    md.returnType = { TypeExpr::Array, "", { elem } };
    ParamDecl p;
    p.name = "v";
    p.type = { TypeExpr::Array, "", { elem } };
    md.params.push_back(p);
    m.methods.push_back(md);

    QString s = lidlMakeSource(m);
    // Both sync and async pack the single list arg via packVariantList(v).
    EXPECT_TRUE(s.contains("invokeRemoteMethod(\"arrs\", \"echoList\", packVariantList(v)"));
    EXPECT_TRUE(s.contains("invokeRemoteMethodAsync(\"arrs\", \"echoList\", packVariantList(v)"));
    // Guard against the spreading forms regressing back in.
    EXPECT_FALSE(s.contains("QVariantList{v}"));
    EXPECT_FALSE(s.contains("QVariantList() << v"));
}

TEST(LidlGenClient, VoidReturnMethod)
{
    ModuleDecl m;
    m.name = "test";
    MethodDecl md;
    md.name = "doStuff";
    md.returnType = { TypeExpr::Primitive, "void", {} };
    m.methods.push_back(md);

    QString h = lidlMakeHeader(m);
    // void return should have async callback with void()
    EXPECT_TRUE(h.contains("std::function<void()>"));

    QString s = lidlMakeSource(m);
    // sync void method should not have "QVariant _result ="
    // The source should just call the method without capturing return
    EXPECT_FALSE(s.contains("QVariant _result = m_client->invokeRemoteMethod(\"test\", \"doStuff\""));
}
