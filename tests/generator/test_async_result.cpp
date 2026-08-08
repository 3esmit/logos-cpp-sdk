// The two complementary gaps this suite pins down:
//
//   sync  had an error channel (logos::CallError*) but NO timeout;
//   async had a timeout but NO error channel.
//
// After the change the sync wrapper takes both, and a distinctly-named
// `<name>AsyncResult` delivers logos::AsyncResult<T> {value, error}. Every
// assertion here fails on the pre-change generator.
#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include "generator_lib.h"

namespace {

QJsonObject method(const QString& name, const QString& ret, const QStringList& paramTypes = {})
{
    QJsonObject m;
    m["name"] = name;
    m["returnType"] = ret;
    m["isInvokable"] = true;
    QJsonArray params;
    for (int i = 0; i < paramTypes.size(); ++i) {
        QJsonObject p;
        p["type"] = paramTypes.at(i);
        p["name"] = QString("p%1").arg(i);
        params.append(p);
    }
    m["parameters"] = params;
    return m;
}

QJsonArray sampleMethods()
{
    QJsonArray a;
    a.append(method("add", "int", {"int", "int"}));
    a.append(method("reset", "void"));
    a.append(method("name", "QString"));
    return a;
}

QString qtHeader()  { return makeHeader("mod", "Mod", sampleMethods(), ApiStyle::Qt); }
QString qtSource()  { return makeSource("mod", "Mod", "mod.h", sampleMethods(), ApiStyle::Qt); }
QString lpHeader()  { return makeHeader("mod", "Mod", sampleMethods(), ApiStyle::Lp); }
QString lpSource()  { return makeSource("mod", "Mod", "mod.h", sampleMethods(), ApiStyle::Lp); }

}  // namespace

// ─── 1. Sync gains a timeout, appended AFTER the error out-param ────────────

TEST(SyncTimeout, QtDeclarationTakesBothErrorAndTimeout)
{
    const QString h = qtHeader();
    // Order matters: err first, timeout second, both defaulted — so a call site
    // that already passes `&err` positionally is unaffected.
    EXPECT_TRUE(h.contains("int add(int p0, int p1, logos::CallError* err = nullptr, Timeout timeout = Timeout());"));
    EXPECT_TRUE(h.contains("void reset(logos::CallError* err = nullptr, Timeout timeout = Timeout());"));
    EXPECT_TRUE(h.contains("QString name(logos::CallError* err = nullptr, Timeout timeout = Timeout());"));
}

TEST(SyncTimeout, QtBodyForwardsTheCallersTimeout)
{
    const QString src = qtSource();
    EXPECT_TRUE(src.contains("Mod::add(int p0, int p1, logos::CallError* err, Timeout timeout)"));
    // Routes to the transport overload that takes BOTH — logos_api_client.h's
    // invokeRemoteMethod(obj, method, args, Timeout, logos::CallError*).
    EXPECT_TRUE(src.contains("}, timeout, &_err);"));
    // ...and no longer hard-codes a fresh default.
    EXPECT_FALSE(src.contains("}, Timeout(), &_err);"));
}

TEST(SyncTimeout, LpDeclarationTakesBothErrorAndTimeoutMs)
{
    // `Timeout` lives in logos_mode.h, which includes <QDebug>; the Qt-free
    // surface spells its deadline the way LpClient/the C ABI do.
    const QString h = lpHeader();
    EXPECT_TRUE(h.contains("int64_t add(int64_t p0, int64_t p1, logos::CallError* err = nullptr, int timeout_ms = 0);"));
    EXPECT_TRUE(h.contains("void reset(logos::CallError* err = nullptr, int timeout_ms = 0);"));
    EXPECT_FALSE(h.contains("Timeout timeout"));
}

TEST(SyncTimeout, LpBodyForwardsTimeoutMs)
{
    const QString src = lpSource();
    EXPECT_TRUE(src.contains("Mod::add(int64_t p0, int64_t p1, logos::CallError* err, int timeout_ms)"));
    EXPECT_TRUE(src.contains("m_client.invoke(\"add\", _args, err, timeout_ms);"));
    EXPECT_TRUE(src.contains("m_client.invoke(\"reset\", _args, err, timeout_ms);"));
    EXPECT_FALSE(src.contains("_args, err);"));
}

// ─── 2. Async gains a result-carrying entry point ───────────────────────────

TEST(AsyncResult, QtHeaderDeclaresTheDistinctlyNamedEntryPoint)
{
    const QString h = qtHeader();
    EXPECT_TRUE(h.contains("void addAsyncResult(int p0, int p1, "
                           "std::function<void(logos::AsyncResult<int>)> callback, "
                           "Timeout timeout = Timeout());"));
    EXPECT_TRUE(h.contains("void nameAsyncResult(std::function<void(logos::AsyncResult<QString>)> callback, "
                           "Timeout timeout = Timeout());"));
}

TEST(AsyncResult, VoidReturnUsesTheVoidSpecializationNotABespokeCallback)
{
    // Uniform shape: std::function<void(AsyncResult<R>)> for every R, including
    // void, so generated forwarding code never special-cases the return type.
    const QString h = qtHeader();
    EXPECT_TRUE(h.contains("void resetAsyncResult(std::function<void(logos::AsyncResult<void>)> callback, "
                           "Timeout timeout = Timeout());"));
}

TEST(AsyncResult, QtHeaderIncludesTheAsyncResultHeader)
{
    EXPECT_TRUE(qtHeader().contains("#include \"logos_async_result.h\""));
}

TEST(AsyncResult, QtBodyRoutesToTheCallErrorAwareTransportOverload)
{
    const QString src = qtSource();
    // A TWO-argument lambda: only LogosAPIClient::AsyncResultErrorCallback is
    // invocable with it, so this cannot silently bind to the value-only one.
    EXPECT_TRUE(src.contains("[callback](QVariant v, const logos::CallError& _err) {"));
    EXPECT_TRUE(src.contains("logos::AsyncResult<int> _r;"));
    EXPECT_TRUE(src.contains("_r.error = _err;"));
    EXPECT_TRUE(src.contains("_r.value = v.isValid() ? qvariant_cast<int>(v) : 0;"));
    EXPECT_TRUE(src.contains("callback(_r);"));
    // The void form carries the error and nothing else.
    EXPECT_TRUE(src.contains("logos::AsyncResult<void> _r;"));
}

TEST(AsyncResult, TheValueDecodeIsSharedWithThePlainAsyncEntryPoint)
{
    // Same decode expression in both, so a failed call delivers exactly the
    // value `<name>Async` would have delivered — plus the error.
    const QString src = qtSource();
    EXPECT_TRUE(src.contains("callback(v.isValid() ? qvariant_cast<int>(v) : 0);"));
    EXPECT_TRUE(src.contains("_r.value = v.isValid() ? qvariant_cast<int>(v) : 0;"));
}

// ─── 3. The existing async surface is untouched ─────────────────────────────

TEST(AsyncResult, ThePlainAsyncEntryPointIsUnchanged)
{
    const QString h = qtHeader();
    EXPECT_TRUE(h.contains("void addAsync(int p0, int p1, std::function<void(int)> callback, "
                           "Timeout timeout = Timeout());"));
    EXPECT_TRUE(h.contains("void resetAsync(std::function<void()> callback, Timeout timeout = Timeout());"));

    const QString src = qtSource();
    EXPECT_TRUE(src.contains("void Mod::addAsync(int p0, int p1, std::function<void(int)> callback, Timeout timeout)"));
    // Still a ONE-argument lambda -> still the value-only transport overload.
    EXPECT_TRUE(src.contains("[callback](QVariant v) {"));
}

// ─── 4. The Qt-free surface deliberately has no AsyncResult yet ─────────────

TEST(AsyncResult, LpSurfaceDoesNotEmitAsyncResultWhileTheCAbiCannotReportOne)
{
    // logos-protocol's lp_invoke_async hard-codes `cb(1, ...)`, so an
    // AsyncResult here would report ok() on a failed call. See makeHeaderLp.
    EXPECT_FALSE(lpHeader().contains("AsyncResult"));
    EXPECT_FALSE(lpSource().contains("AsyncResult"));
    // ...and the Lp async entry point keeps its exact shape.
    EXPECT_TRUE(lpHeader().contains("void addAsync(int64_t p0, int64_t p1, std::function<void(int64_t)> callback);"));
}

// ─── 5. A REJECTION reaches the surface that can report it ──────────────────
//
// A provider that refuses a call answers the canonical
// {"code":"dispatch_failed", …} object as its RESULT, not as a transport error.
// The sync path folds it into the caller's CallError. `<name>Async` can only
// warn — its callback takes the value alone. `<name>AsyncResult` is the first
// async surface with an error slot, so it must fold it too; reporting ok() for
// a rejected call there would reintroduce, on the new surface, exactly the
// defect the sync fold removed.

TEST(AsyncResult, RejectionIsFoldedIntoTheAsyncResultError)
{
    const QString src = qtSource();
    EXPECT_TRUE(src.contains("if (_r.error.ok()) logosDispatchRejection(v, _r.error);"));
    // Folded BEFORE the value is decoded and before the callback runs, so the
    // callback never sees an ok() AsyncResult for a rejected call.
    const int fold = src.indexOf("if (_r.error.ok()) logosDispatchRejection(v, _r.error);");
    const int decode = src.indexOf("_r.value = v.isValid() ? qvariant_cast<int>(v) : 0;");
    const int deliver = src.indexOf("callback(_r);");
    EXPECT_NE(fold, -1);
    EXPECT_NE(decode, -1);
    EXPECT_NE(deliver, -1);
    EXPECT_LT(fold, decode);
    EXPECT_LT(decode, deliver);
}

TEST(AsyncResult, ThePlainAsyncEntryPointStillOnlyWarns)
{
    // Unchanged public surface -> nowhere to put an error -> the log is all
    // there is. The warning names `<name>Async`, never `<name>AsyncResult`.
    const QString src = qtSource();
    EXPECT_TRUE(src.contains("{ logos::CallError _rej; if (logosDispatchRejection(v, _rej))"));
    EXPECT_TRUE(src.contains("Mod::addAsync: remote call failed:"));
    EXPECT_FALSE(src.contains("Mod::addAsyncResult: remote call failed:"));
}
