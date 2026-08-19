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
}

// setEventSource / eventSource / trigger used to be emitted here: an
// author-facing way to SOURCE events through a CONSUMER wrapper. They had zero
// callers anywhere in the workspace, including the vendored SDK copies, and the
// generated code never used them either — m_eventSource was written only by its
// own setter and read only by trigger, so a trigger() call without a prior
// setEventSource() just warned and returned.
//
// Removing them also removes the reason a Qt wrapper had to keep a
// LogosAPIClient alongside the lp client: onEventResponse was the only lp-less
// call left. Pinned so the surface does not quietly reappear.
TEST(LidlGenClient, NoDeadEventSourceSurface)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_FALSE(h.contains("setEventSource")) << h.toStdString();
    EXPECT_FALSE(h.contains("m_eventSource")) << h.toStdString();
    EXPECT_FALSE(h.contains("trigger(")) << h.toStdString();
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
    // Deferred, not a synchronous acquire -- see MakeSourceTest.
    EXPECT_TRUE(s.contains("onEventWhenAvailable(m_moduleName, eventName, callback)"));
    EXPECT_FALSE(s.contains("ensureReplica"));
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

// Records: a `type` decl becomes a real C++ struct in the generated header, so a
// Qt consumer says `Status s = client.makeStatus();` rather than digging fields
// out of a QVariantMap. Additive — nothing generated records before this.
static ModuleDecl makeRecordModule()
{
    ModuleDecl m;
    m.name = "info_module";
    m.version = "1.0.0";

    TypeDecl rec;
    rec.name = "Status";
    FieldDecl a; a.name = "port"; a.type = { TypeExpr::Primitive, "uint", {} };
    FieldDecl b; b.name = "blob"; b.type = { TypeExpr::Primitive, "bstr", {} };
    rec.fields = {a, b};
    m.types.push_back(rec);

    {
        MethodDecl md;
        md.name = "describeStatus";
        md.returnType = { TypeExpr::Primitive, "tstr", {} };
        ParamDecl p; p.name = "s"; p.type = { TypeExpr::Named, "Status", {} };
        md.params.push_back(p);
        m.methods.push_back(md);
    }
    {
        MethodDecl md;
        md.name = "makeStatuses";
        TypeExpr elem = { TypeExpr::Named, "Status", {} };
        md.returnType = { TypeExpr::Array, "", { elem } };
        m.methods.push_back(md);
    }
    return m;
}

// `isTaggedBytes()` is checked BEFORE `is_object()` in both the codec and the
// QVariant bridge, so a record whose only field is a tstr named `_bytes` is
// wire-identical to a tagged byte string and decodes as bytes — the struct
// silently disappears. The ambiguity is inherent to the tagged form; refusing
// to emit the one shape guaranteed to misdecode is what a generator can do
// about it.
TEST(LidlGenClient, RecordThatCollidesWithTheBytesTagIsRefused)
{
    ModuleDecl m;
    m.name = "c_module";
    TypeDecl bad;
    bad.name = "Sneaky";
    FieldDecl f; f.name = "_bytes"; f.type = { TypeExpr::Primitive, "tstr", {} };
    bad.fields = {f};
    m.types.push_back(bad);

    QString err;
    EXPECT_FALSE(lidlCheckRecords(m, &err));
    EXPECT_TRUE(err.contains("Sneaky")) << err.toStdString();
    EXPECT_TRUE(err.contains("_bytes")) << err.toStdString();

    // A SECOND field disambiguates it — isTaggedBytes requires exactly one key,
    // so this shape round-trips and must still be allowed.
    FieldDecl g; g.name = "other"; g.type = { TypeExpr::Primitive, "int", {} };
    m.types[0].fields.push_back(g);
    EXPECT_TRUE(lidlCheckRecords(m, nullptr));

    // A `_bytes` field that is not the only one, and a differently-named sole
    // field, are both fine.
    ModuleDecl ok;
    ok.name = "ok_module";
    TypeDecl t; t.name = "Fine";
    FieldDecl h; h.name = "payload"; h.type = { TypeExpr::Primitive, "tstr", {} };
    t.fields = {h};
    ok.types.push_back(t);
    EXPECT_TRUE(lidlCheckRecords(ok, nullptr));
}

// The ASYNC overload must decode a record the same way the sync one does.
//
// `qvariant_cast<Status>(v)` does not fail on the wire's QVariantMap: no
// Q_DECLARE_METATYPE is emitted for the struct, so the cast silently yields a
// DEFAULT-CONSTRUCTED Status and the caller sees empty fields with no
// diagnostic. The sync path was already correct, which makes it worse — the
// same call would be right or wrong depending only on which overload the
// caller reached for.
TEST(LidlGenClient, AsyncRecordReturnsDecodeFieldByField)
{
    const QString c = lidlMakeSource(makeRecordModule(), BindMode::Bound);

    // A [Record] return, in the async callback.
    EXPECT_TRUE(c.contains("StatusFromVariant")) << c.toStdString();
    EXPECT_FALSE(c.contains("qvariant_cast<QList<Status>>")) << c.toStdString();
    EXPECT_FALSE(c.contains("qvariant_cast<Status>")) << c.toStdString();
}

TEST(LidlGenClient, RecordsBecomeStructsWithConversions)
{
    const QString h = lidlMakeHeader(makeRecordModule(), BindMode::Bound);

    // The struct, at the 1-1 Qt spellings: 64-bit unsigned, QByteArray for bytes.
    EXPECT_TRUE(h.contains("struct Status {")) << h.toStdString();
    EXPECT_TRUE(h.contains("qulonglong port{};")) << h.toStdString();
    EXPECT_TRUE(h.contains("QByteArray blob{};")) << h.toStdString();

    // Conversions both ways.
    EXPECT_TRUE(h.contains("inline QVariant StatusToVariant(const Status& v)")) << h.toStdString();
    EXPECT_TRUE(h.contains("inline Status StatusFromVariant(const QVariant& value)")) << h.toStdString();
    // A bstr field is a QByteArray: logos-protocol's QVariant<->JSON conversion
    // already materialises the tagged {"_bytes":…} form as QByteArray, so binary
    // survives without record-specific bytes handling.
    EXPECT_TRUE(h.contains("__out.blob = __m.value(\"blob\").toByteArray();")) << h.toStdString();

    // Methods speak the record: by const& in, typed list out. A QVariantList
    // could not hold a Status without Q_DECLARE_METATYPE.
    EXPECT_TRUE(h.contains("describeStatus(const Status& s")) << h.toStdString();
    EXPECT_TRUE(h.contains("QList<Status> makeStatuses(")) << h.toStdString();
}


// ---------------------------------------------------------------------------
// Optionality
//
// This backend is not on any live build path (real Qt consumers come from the
// legacy interface-wrapper path), so the bar here is CONSISTENCY, not features:
// the two spellings of an optional field must not generate different structs
// from the same declaration.
// ---------------------------------------------------------------------------

static ModuleDecl makeOptionalRecordModule(bool useFlagSpelling)
{
    ModuleDecl m;
    m.name = "opt_module";
    m.version = "1.0.0";

    TypeDecl t;
    t.name = "Profile";
    {
        FieldDecl f; f.name = "required"; f.type = { TypeExpr::Primitive, "tstr", {} };
        t.fields.push_back(f);
    }
    {
        FieldDecl f;
        f.name = "nickname";
        if (useFlagSpelling) {                       // `? nickname: tstr`
            f.type = { TypeExpr::Primitive, "tstr", {} };
            f.optional = true;
        } else {                                     // `nickname: ?tstr`
            f.type = { TypeExpr::Optional, "", { { TypeExpr::Primitive, "tstr", {} } } };
        }
        t.fields.push_back(f);
    }
    m.types.push_back(t);

    MethodDecl md;
    md.name = "echoProfile";
    md.returnType = { TypeExpr::Named, "Profile", {} };
    ParamDecl p; p.name = "v"; p.type = { TypeExpr::Named, "Profile", {} };
    md.params.push_back(p);
    m.methods.push_back(md);
    return m;
}

TEST(LidlGenClient, BothOptionalSpellingsEmitIdenticalCode)
{
    const QString flagged = lidlMakeHeader(makeOptionalRecordModule(true), BindMode::Bound);
    const QString typed   = lidlMakeHeader(makeOptionalRecordModule(false), BindMode::Bound);
    // Reading `f.type` alone made the flag spelling emit a bare `QString` — a
    // type with no empty inhabitant at all — from the same declaration that the
    // type spelling turned into a QVariant.
    EXPECT_EQ(flagged, typed) << flagged.toStdString() << "\n---\n" << typed.toStdString();
}

TEST(LidlGenClient, OptionalRecordFieldIsTwoStateQVariant)
{
    const QString h = lidlMakeHeader(makeOptionalRecordModule(true), BindMode::Bound);

    // QVariant, because Qt has no optional and an invalid QVariant is its one
    // empty inhabitant.
    EXPECT_TRUE(h.contains("QVariant nickname{};")) << h.toStdString();
    EXPECT_TRUE(h.contains("QString required{};")) << h.toStdString();
    // A record field is a NAMED slot: empty omits the key rather than writing an
    // invalid QVariant into the map.
    EXPECT_TRUE(h.contains("if (v.nickname.isValid())")) << h.toStdString();
    // Absent and null both arrive as an invalid QVariant. Converting (the
    // `.toString()` a required tstr field gets) would have turned "empty" into
    // "", which is a VALUE.
    EXPECT_TRUE(h.contains("__out.nickname = __m.value(\"nickname\");")) << h.toStdString();
    EXPECT_FALSE(h.contains("__out.nickname = __m.value(\"nickname\").toString();"))
        << h.toStdString();
}

// The `_bytes` collision check must read through an optional too. A PRESENT
// `? _bytes: tstr` still encodes to {"_bytes": "..."} — the shape that decodes
// as a byte string and loses the record — so both spellings have to be refused.
// Reading `f.type` refused only the flag one.
TEST(LidlGenClient, BytesTagCollisionIsRefusedThroughAnOptional)
{
    auto sneaky = [](bool useFlagSpelling) {
        ModuleDecl m;
        m.name = "sneaky_module";
        TypeDecl t;
        t.name = "Sneaky";
        FieldDecl f;
        f.name = "_bytes";
        if (useFlagSpelling) {
            f.type = { TypeExpr::Primitive, "tstr", {} };
            f.optional = true;
        } else {
            f.type = { TypeExpr::Optional, "", { { TypeExpr::Primitive, "tstr", {} } } };
        }
        t.fields.push_back(f);
        m.types.push_back(t);
        return m;
    };

    for (bool flag : {true, false}) {
        QString error;
        EXPECT_FALSE(lidlCheckRecords(sneaky(flag), &error)) << "flagSpelling=" << flag;
        EXPECT_TRUE(error.contains("Sneaky")) << error.toStdString();
    }
}

// ---------------------------------------------------------------------------
// Sync timeout + result-carrying async
//
// This emitter and cpp-generator/generator_lib.cpp produce the SAME consumer surface
// for the same contract — one is reached from a published `.lidl`, the other
// through the module builder — so the two must agree. tests/generator/
// test_async_result.cpp holds the legacy twin of these assertions.
// ---------------------------------------------------------------------------

TEST(LidlGenClient, SyncTakesBothErrorAndTimeout)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    // Trailing and defaulted, err first — `createAccount(p)` and
    // `createAccount(p, &err)` are both unaffected.
    EXPECT_TRUE(h.contains("QString createAccount(const QString& passphrase, "
                           "logos::CallError* err = nullptr, Timeout timeout = Timeout());"));
    EXPECT_TRUE(h.contains("QStringList listAccounts(logos::CallError* err = nullptr, "
                           "Timeout timeout = Timeout());"));
}

TEST(LidlGenClient, SyncBodyForwardsTheCallersTimeout)
{
    auto m = makeTestModule();
    QString s = lidlMakeSource(m);
    EXPECT_TRUE(s.contains("WalletModule::createAccount(const QString& passphrase, "
                           "logos::CallError* err, Timeout timeout)"));
    EXPECT_TRUE(s.contains("), timeout, &_err);"));
    EXPECT_FALSE(s.contains("), Timeout(), &_err);"));
}

TEST(LidlGenClient, HeaderDeclaresTheResultCarryingAsyncEntryPoint)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("#include \"logos_async_result.h\""));
    EXPECT_TRUE(h.contains("void createAccountAsyncResult(const QString& passphrase, "
                           "std::function<void(logos::AsyncResult<QString>)> callback, "
                           "Timeout timeout = Timeout());"));
    EXPECT_TRUE(h.contains("void listAccountsAsyncResult("
                           "std::function<void(logos::AsyncResult<QStringList>)> callback, "
                           "Timeout timeout = Timeout());"));
}

TEST(LidlGenClient, ResultCarryingAsyncRoutesToTheCallErrorAwareOverload)
{
    auto m = makeTestModule();
    QString s = lidlMakeSource(m);
    // Two-argument lambda: only AsyncResultErrorCallback is invocable with it.
    EXPECT_TRUE(s.contains("[callback](QVariant v, const logos::CallError& _err) {"));
    EXPECT_TRUE(s.contains("logos::AsyncResult<QString> _r;"));
    EXPECT_TRUE(s.contains("_r.error = _err;"));
    EXPECT_TRUE(s.contains("callback(_r);"));
}

TEST(LidlGenClient, ThePlainAsyncEntryPointIsUnchanged)
{
    auto m = makeTestModule();
    QString h = lidlMakeHeader(m);
    EXPECT_TRUE(h.contains("void createAccountAsync(const QString& passphrase, "
                           "std::function<void(QString)> callback, Timeout timeout = Timeout());"));
    QString s = lidlMakeSource(m);
    // Still a ONE-argument lambda -> still the value-only transport overload.
    EXPECT_TRUE(s.contains("[callback](QVariant v) {"));
}
