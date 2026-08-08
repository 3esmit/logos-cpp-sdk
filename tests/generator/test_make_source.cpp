#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include "generator_lib.h"

static QJsonObject makeMethod(const QString& name, const QString& retType, int paramCount)
{
    QJsonObject m;
    m["name"] = name;
    m["returnType"] = retType;
    m["isInvokable"] = true;
    QJsonArray params;
    for (int i = 0; i < paramCount; ++i) {
        QJsonObject p;
        p["type"] = "int";
        p["name"] = QString("p%1").arg(i);
        params.append(p);
    }
    m["parameters"] = params;
    return m;
}

TEST(MakeSourceTest, ContainsInclude)
{
    QString src = makeSource("mod", "Mod", "mod_api.h", QJsonArray());
    EXPECT_TRUE(src.contains("#include \"mod_api.h\""));
}

TEST(MakeSourceTest, ConstructorInitializesClient)
{
    QString src = makeSource("my_mod", "MyMod", "my_mod_api.h", QJsonArray());
    EXPECT_TRUE(src.contains("MyMod::MyMod(LogosAPI* api)"));
    EXPECT_TRUE(src.contains("api->getClient(\"my_mod\")"));
}

TEST(MakeSourceTest, EnsureReplicaMethod)
{
    QString src = makeSource("mod", "Mod", "mod.h", QJsonArray());
    EXPECT_TRUE(src.contains("LogosObject* Mod::ensureReplica()"));
}

TEST(MakeSourceTest, ZeroParams)
{
    QJsonArray methods;
    methods.append(makeMethod("doStuff", "int", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("m_client->invokeRemoteMethod(\"mod\", \"doStuff\", QVariantList{}, timeout, &_err)"));
    EXPECT_TRUE(src.contains("return _result.toInt()"));
}

TEST(MakeSourceTest, OneParam)
{
    QJsonArray methods;
    methods.append(makeMethod("fn", "bool", 1));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("m_client->invokeRemoteMethod(\"mod\", \"fn\", QVariantList{QVariant::fromValue(p0)}, timeout, &_err)"));
    EXPECT_TRUE(src.contains("return _result.toBool()"));
}

TEST(MakeSourceTest, TwoParams)
{
    QJsonArray methods;
    methods.append(makeMethod("fn", "void", 2));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("m_client->invokeRemoteMethod(\"mod\", \"fn\", QVariantList{QVariant::fromValue(p0), QVariant::fromValue(p1)}, timeout, &_err)"));
}

TEST(MakeSourceTest, ThreeParams)
{
    QJsonArray methods;
    methods.append(makeMethod("fn", "QString", 3));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("QVariant::fromValue(p0), QVariant::fromValue(p1), QVariant::fromValue(p2)"));
    EXPECT_TRUE(src.contains("return _result.toString()"));
}

TEST(MakeSourceTest, FourParams)
{
    QJsonArray methods;
    methods.append(makeMethod("fn", "double", 4));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("QVariant::fromValue(p0), QVariant::fromValue(p1), QVariant::fromValue(p2), QVariant::fromValue(p3)"));
    EXPECT_TRUE(src.contains("return _result.toDouble()"));
}

TEST(MakeSourceTest, FiveParams)
{
    QJsonArray methods;
    methods.append(makeMethod("fn", "float", 5));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("QVariant::fromValue(p0), QVariant::fromValue(p1), QVariant::fromValue(p2), QVariant::fromValue(p3), QVariant::fromValue(p4)"));
    EXPECT_TRUE(src.contains("return _result.toFloat()"));
}

TEST(MakeSourceTest, MoreThanFiveParamsUsesVariantList)
{
    QJsonArray methods;
    methods.append(makeMethod("fn", "QVariant", 6));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("QVariantList{QVariant::fromValue(p0), QVariant::fromValue(p1), QVariant::fromValue(p2), QVariant::fromValue(p3), QVariant::fromValue(p4), QVariant::fromValue(p5)}"));
    EXPECT_TRUE(src.contains("return _result"));
}

// Regression: a QVariantList-typed ([any]/[int]/...) argument must be wrapped as
// ONE element via QVariant::fromValue. A bare `QVariantList{v}` concatenates the
// list's elements into the args list, sending [1,2,3] as three positional args
// — the "typed arrays empty over the Qt path" bug.
TEST(MakeSourceTest, ListArgWrappedAsOneElement)
{
    QJsonObject m;
    m["name"] = "echoList";
    m["returnType"] = "QVariantList";
    m["isInvokable"] = true;
    QJsonObject p;
    p["type"] = "QVariantList";
    p["name"] = "v";
    QJsonArray params;
    params.append(p);
    m["parameters"] = params;
    QJsonArray methods;
    methods.append(m);

    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("invokeRemoteMethod(\"mod\", \"echoList\", QVariantList{QVariant::fromValue(v)}, timeout, &_err)"));
    EXPECT_TRUE(src.contains("invokeRemoteMethodAsync(\"mod\", \"echoList\", QVariantList{QVariant::fromValue(v)}"));
    // The bare (spreading) form must not appear.
    EXPECT_FALSE(src.contains("QVariantList{v}"));
}

// Regression: in the Qt-free (lp) wrapper, an `any` (QVariant) return must pass
// the raw json value through, NOT force it to an object. `any` shares the
// LogosMap std type with the `{tstr:any}` map, and forcing `any` to an object
// collapsed every non-object value to `{}` (e.g. a proxy forwarding echoAny
// returned {} for the string "x"). The map keeps its object coercion.
TEST(MakeSourceTest, LpAnyReturnPassesThroughButMapForcesObject)
{
    QJsonObject any;
    any["name"] = "echoAny";
    any["returnType"] = "QVariant";
    any["isInvokable"] = true;
    {
        QJsonObject p; p["type"] = "QVariant"; p["name"] = "v";
        QJsonArray ps; ps.append(p); any["parameters"] = ps;
    }
    QJsonObject mp;
    mp["name"] = "echoMap";
    mp["returnType"] = "QVariantMap";
    mp["isInvokable"] = true;
    {
        QJsonObject p; p["type"] = "QVariantMap"; p["name"] = "v";
        QJsonArray ps; ps.append(p); mp["parameters"] = ps;
    }
    QJsonArray methods;
    methods.append(any);
    methods.append(mp);

    QString src = makeSourceLp("mod", "Mod", "mod.h", methods);
    // `any` return: raw passthrough (return _r;), no is_object coercion.
    EXPECT_TRUE(src.contains("return _r;"));
    // `{tstr:any}` map return: still forced to an object.
    EXPECT_TRUE(src.contains("_r.is_object() ? _r : LogosMap::object()"));
}

TEST(MakeSourceTest, VoidReturnNoConversion)
{
    QJsonArray methods;
    methods.append(makeMethod("doIt", "void", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_FALSE(src.contains("return _result"));
}

TEST(MakeSourceTest, QStringListReturn)
{
    QJsonArray methods;
    methods.append(makeMethod("getNames", "QStringList", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("return _result.toStringList()"));
}

TEST(MakeSourceTest, QJsonArrayReturn)
{
    QJsonArray methods;
    methods.append(makeMethod("getData", "QJsonArray", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("qvariant_cast<QJsonArray>(_result)"));
}

TEST(MakeSourceTest, QVariantListReturn)
{
    QJsonArray methods;
    methods.append(makeMethod("getItems", "QVariantList", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("return _result.toList()"));
}

TEST(MakeSourceTest, QVariantMapReturn)
{
    QJsonArray methods;
    methods.append(makeMethod("getData", "QVariantMap", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("return _result.toMap()"));
}

TEST(MakeSourceTest, QVariantListAsync)
{
    QJsonArray methods;
    methods.append(makeMethod("getItems", "QVariantList", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("Mod::getItemsAsync("));
    EXPECT_TRUE(src.contains("std::function<void(QVariantList)> callback"));
    EXPECT_TRUE(src.contains("QVariantList()"));
}

TEST(MakeSourceTest, QVariantMapAsync)
{
    QJsonArray methods;
    methods.append(makeMethod("getData", "QVariantMap", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("Mod::getDataAsync("));
    EXPECT_TRUE(src.contains("std::function<void(QVariantMap)> callback"));
    EXPECT_TRUE(src.contains("QVariantMap()"));
}

TEST(MakeSourceTest, QVariantListConstRefParam)
{
    QJsonArray methods;
    {
        QJsonObject m;
        m["name"] = "process";
        m["returnType"] = "void";
        m["isInvokable"] = true;
        QJsonArray params;
        QJsonObject p;
        p["type"] = "QVariantList";
        p["name"] = "items";
        params.append(p);
        m["parameters"] = params;
        methods.append(m);
    }
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("const QVariantList& items"));
}

TEST(MakeSourceTest, QVariantMapConstRefParam)
{
    QJsonArray methods;
    {
        QJsonObject m;
        m["name"] = "update";
        m["returnType"] = "void";
        m["isInvokable"] = true;
        QJsonArray params;
        QJsonObject p;
        p["type"] = "QVariantMap";
        p["name"] = "data";
        params.append(p);
        m["parameters"] = params;
        methods.append(m);
    }
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("const QVariantMap& data"));
}

TEST(MakeSourceTest, LogosResultReturn)
{
    QJsonArray methods;
    methods.append(makeMethod("query", "LogosResult", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("_result.value<LogosResult>()"));
}

TEST(MakeSourceTest, AsyncImplementation)
{
    QJsonArray methods;
    methods.append(makeMethod("fn", "int", 1));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("Mod::fnAsync("));
    EXPECT_TRUE(src.contains("invokeRemoteMethodAsync"));
    EXPECT_TRUE(src.contains("callback"));
}

TEST(MakeSourceTest, NonInvokableSkipped)
{
    QJsonArray methods;
    QJsonObject m;
    m["name"] = "hidden";
    m["returnType"] = "void";
    m["isInvokable"] = false;
    m["parameters"] = QJsonArray();
    methods.append(m);

    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_FALSE(src.contains("hidden"));
}

// ─── The provider REJECTION envelope on the return path ─────────────────────
//
// A provider that refuses a call answers the canonical
// {"code":"dispatch_failed", "message":…, "origin":…} object as its RESULT. The
// Qt return table converts it like any other value, which ERASES it — a rejected
// `[uint]` call answered `[]`, indistinguishable from "the provider returned
// nothing". These pin the consumer folding it into the CallError out-channel the
// wrapper already uses for a failed call.

TEST(MakeSourceTest, QtEmitsRejectionDetector)
{
    QJsonArray methods;
    methods.append(makeMethod("fn", "QVariantList", 1));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("bool logosDispatchRejection(const QVariant& v, logos::CallError& out)"));
    // Exact match only: an `any` / map return carrying user data must not
    // false-match (same discipline as logos_rpc_status.h's sentinel).
    EXPECT_TRUE(src.contains("if (m.size() != 3) return false;"));
    EXPECT_TRUE(src.contains("if (code.toString() != QStringLiteral(\"dispatch_failed\")) return false;"));
}

TEST(MakeSourceTest, QtSyncFoldsRejectionIntoCallError)
{
    QJsonArray methods;
    methods.append(makeMethod("echoUintList", "QVariantList", 1));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    // Folded BEFORE the return table converts the value, and before *err is
    // written, so a caller passing err sees the rejection.
    const int fold = src.indexOf("if (_err.ok()) logosDispatchRejection(_result, _err);");
    const int assign = src.indexOf("if (err) *err = _err;");
    const int convert = src.indexOf("return _result.toList();");
    EXPECT_NE(fold, -1);
    EXPECT_NE(assign, -1);
    EXPECT_NE(convert, -1);
    EXPECT_LT(fold, assign);
    EXPECT_LT(assign, convert);
}

TEST(MakeSourceTest, QtVoidReturnStillCapturesResult)
{
    // A void method can be rejected too, and the rejection object is the only
    // place that says so — so the result has to be captured even when it is
    // never returned.
    QJsonArray methods;
    methods.append(makeMethod("doVoid", "void", 0));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("QVariant _result = m_client->invokeRemoteMethod(\"mod\", \"doVoid\""));
    EXPECT_TRUE(src.contains("if (_err.ok()) logosDispatchRejection(_result, _err);"));
}

TEST(MakeSourceTest, QtAsyncLogsRejection)
{
    // The async callback takes the value alone — there is no CallError to fill
    // without changing the generated public surface — so the rejection has to at
    // least reach the module log instead of vanishing into the conversion.
    QJsonArray methods;
    methods.append(makeMethod("fn", "QVariantList", 1));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("{ logos::CallError _rej; if (logosDispatchRejection(v, _rej))"));
    EXPECT_TRUE(src.contains("Mod::fnAsync: remote call failed:"));
}

TEST(MakeSourceTest, QtNoInvokableMethodsEmitsNoDetector)
{
    // Unreachable from any body: emitting it would be an unused function in an
    // anonymous namespace (-Wunused-function), and such a contract's wrapper
    // stays byte-identical to what it generated before.
    QJsonArray methods;
    QJsonObject m;
    m["name"] = "hidden";
    m["returnType"] = "void";
    m["isInvokable"] = false;
    m["parameters"] = QJsonArray();
    methods.append(m);
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_FALSE(src.contains("logosDispatchRejection"));
}

TEST(MakeSourceTest, LpSurfaceIsUntouched)
{
    // The fix is Qt-consumer-only; the lp wrapper must generate exactly as
    // before (byte-identical output is the negative control for the change).
    QJsonArray methods;
    methods.append(makeMethod("fn", "QVariantList", 1));
    QString src = makeSource("mod", "Mod", "mod.h", methods, ApiStyle::Lp);
    EXPECT_FALSE(src.contains("logosDispatchRejection"));
}

TEST(MakeSourceTest, QtRejectionDetectorIsPreprocessorGuarded)
{
    // The umbrella (logos_sdk.cpp) textually #includes EVERY generated
    // `<dep>_api.cpp`, so a module with more than one dependency puts several
    // copies in ONE translation unit — "redefinition of logosDispatchRejection".
    // Internal linkage does not help there; only the guard does.
    QJsonArray methods;
    methods.append(makeMethod("fn", "QVariantList", 1));
    QString src = makeSource("mod", "Mod", "mod.h", methods);
    EXPECT_TRUE(src.contains("#ifndef LOGOS_GENERATED_DISPATCH_REJECTION"));
    EXPECT_TRUE(src.contains("#define LOGOS_GENERATED_DISPATCH_REJECTION"));
    EXPECT_TRUE(src.contains("#endif  // LOGOS_GENERATED_DISPATCH_REJECTION"));
    // Concatenating two generated wrappers, as the umbrella does, must compile:
    // the second copy is preprocessed away.
    QString other = makeSource("dep", "Dep", "dep.h", methods);
    EXPECT_EQ(other.count("bool logosDispatchRejection"), 1);
    EXPECT_EQ((src + other).count("#ifndef LOGOS_GENERATED_DISPATCH_REJECTION"), 2);
}
