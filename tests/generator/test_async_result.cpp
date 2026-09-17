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
    // Into a LOCAL `_err`, then copied out: `err` is optional on this surface,
    // and the dispatch-rejection fold needs somewhere to write either way.
    EXPECT_TRUE(src.contains("m_client.invoke(\"add\", _args, &_err, timeout_ms);"));
    EXPECT_TRUE(src.contains("m_client.invoke(\"reset\", _args, &_err, timeout_ms);"));
    EXPECT_TRUE(src.contains("if (err) *err = _err;"));
    // The deadline is still forwarded, never dropped for a fresh default.
    EXPECT_FALSE(src.contains("_args, &_err);"));
}

TEST(SyncTimeout, LpSyncFoldsAProviderRejectionIntoTheErrorChannel)
{
    // A provider that RAN and refused answers {"code":"dispatch_failed", …} as
    // its RESULT, so LpClient::invoke reports ok() and the decode erases it.
    // The Qt sync path has folded this for a while; this surface now does too.
    const QString src = lpSource();
    const QString fold = "if (_err.ok()) logosDispatchRejectionJson(_r, _err);";
    ASSERT_TRUE(src.contains(fold));
    // Folded BEFORE the value is decoded and before `err` is written out, so a
    // caller never reads an ok() error next to a default-decoded rejection.
    const int f = src.indexOf(fold);
    const int copy = src.indexOf("if (err) *err = _err;");
    const int ret = src.indexOf("    return (_r.is_number_integer()");
    ASSERT_NE(copy, -1);
    ASSERT_NE(ret, -1);
    EXPECT_LT(f, copy);
    EXPECT_LT(copy, ret);
}

TEST(SyncTimeout, LpVoidSyncStillCapturesTheResultSoItCanSeeARejection)
{
    // A void method can be rejected too, and the rejection object is the only
    // place that says so — so the result is captured even where nothing is
    // returned. `_r` is not unused: the fold reads it.
    const QString src = lpSource();
    EXPECT_TRUE(src.contains("nlohmann::json _r = m_client.invoke(\"reset\", _args, &_err, timeout_ms);"));
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

// ─── 4. The Qt-free surface emits its own AsyncResult twin ──────────────────
//
// It was withheld for a long time, and for a reason that belonged to the
// transport rather than to this emitter: lp_invoke_async used to subscribe with
// the VALUE-ONLY overload and hard-code `cb(1, ...)`, so an AsyncResult built on
// it would have reported ok() for a call to a module that is not loaded.
// logos-protocol#40 fixed that (`cb(0, makeErrorJson(...))`) and
// logos::LpClient::invokeAsyncResult surfaces it in C++, so the twin is honest
// and is emitted.

TEST(AsyncResult, LpHeaderDeclaresTheDistinctlyNamedEntryPoint)
{
    const QString h = lpHeader();
    EXPECT_TRUE(h.contains("void addAsyncResult(int64_t p0, int64_t p1, "
                           "std::function<void(logos::AsyncResult<int64_t>)> callback, "
                           "int timeout_ms = 0);"));
    EXPECT_TRUE(h.contains("void nameAsyncResult(std::function<void(logos::AsyncResult<std::string>)> callback, "
                           "int timeout_ms = 0);"));
    // Same uniform shape the Qt surface uses for void: the error-only
    // specialisation, never a bespoke callback.
    EXPECT_TRUE(h.contains("void resetAsyncResult(std::function<void(logos::AsyncResult<void>)> callback, "
                           "int timeout_ms = 0);"));
    // `Timeout` lives in logos_mode.h, which includes <QDebug>. Naming it here
    // would drag Qt into a translation unit whose whole purpose is not to have
    // any, so this surface spells deadlines the way the C ABI does.
    EXPECT_FALSE(h.contains("Timeout timeout"));
}

TEST(AsyncResult, LpHeaderIncludesTheAsyncResultHeader)
{
    EXPECT_TRUE(lpHeader().contains("#include \"logos_async_result.h\""));
}

TEST(AsyncResult, LpBodyRoutesToTheErrorCarryingClientEntryPoint)
{
    const QString src = lpSource();
    // invokeAsyncResult, NOT invokeAsync: the latter collapses the C ABI's
    // failure form into a bare JSON null, which is also what a successful call
    // returning nothing delivers — indistinguishable, which is the whole defect.
    EXPECT_TRUE(src.contains("m_client.invokeAsyncResult(\"add\", _args,"));
    EXPECT_TRUE(src.contains("[callback](nlohmann::json _r, const logos::CallError& _err) {"));
    EXPECT_TRUE(src.contains("logos::AsyncResult<int64_t> _res;"));
    EXPECT_TRUE(src.contains("_res.error = _err;"));
    EXPECT_TRUE(src.contains("callback(_res);"));
    // The void form carries the error and nothing else.
    EXPECT_TRUE(src.contains("logos::AsyncResult<void> _res;"));
}

TEST(AsyncResult, LpValueDecodeIsSharedWithThePlainAsyncEntryPoint)
{
    // Same decode expression in both, so a failed call delivers exactly the
    // value `<name>Async` would have delivered — plus the error.
    const QString src = lpSource();
    const QString decode = "(_r.is_string() ? _r.get<std::string>() : std::string())";
    EXPECT_TRUE(src.contains("callback(" + decode + ");"));
    EXPECT_TRUE(src.contains("_res.value = " + decode + ";"));
}

TEST(AsyncResult, LpRejectionIsFoldedBeforeTheValueIsDecoded)
{
    // Same rule as the sync path: fold before decoding, or this surface reports
    // success for a refused call — on the one async surface that has somewhere
    // to say otherwise.
    const QString src = lpSource();
    const QString fold = "if (_res.error.ok()) logosDispatchRejectionJson(_r, _res.error);";
    ASSERT_TRUE(src.contains(fold));
    const int f = src.indexOf(fold);
    const int decode = src.indexOf("_res.value = ");
    const int deliver = src.indexOf("callback(_res);");
    ASSERT_NE(decode, -1);
    ASSERT_NE(deliver, -1);
    EXPECT_LT(f, decode);
    EXPECT_LT(decode, deliver);
}

TEST(AsyncResult, LpDispatchRejectionDetectorIsEmittedOnceAndGuarded)
{
    // The umbrella (logos_sdk.cpp) textually #includes EVERY generated
    // <dep>_api.cpp, so a module with more than one dependency puts several of
    // these in ONE translation unit. Internal linkage handles the separate-TU
    // case; only the preprocessor handles this one.
    const QString src = lpSource();
    EXPECT_EQ(src.count("bool logosDispatchRejectionJson"), 1);
    EXPECT_TRUE(src.contains("#ifndef LOGOS_GENERATED_DISPATCH_REJECTION_JSON"));
    EXPECT_TRUE(src.contains("#define LOGOS_GENERATED_DISPATCH_REJECTION_JSON"));
}

// The two emitters in this repo (this nlohmann one and the QVariant one in
// test_make_source.cpp) are driven by ONE kRejectionCodes array in
// generator_lib.cpp, so they cannot drift. These assert the array reached the
// emitted text on this side too.
TEST(AsyncResult, LpDispatchRejectionDetectorMatchesTheClosedCodeSet)
{
    const QString src = lpSource();
    EXPECT_TRUE(src.contains(
        "    if (_code != \"dispatch_failed\"\n"
        "        && _code != \"invalid_args\"\n"
        "        && _code != \"unknown_method\") return false;\n"))
        << src.toStdString();
    // "invalid_args" is the code that was LIVE and undetected: the cdylib
    // dispatch (experimental/lidl_gen_cdylib.cpp) and logos-rust-sdk's
    // args::invalid_args both answer an arity error with it, and a consumer
    // decoded it as a three-key map. "unknown_method" is inert until providers
    // emit it; it is here so the detector is ready before they do.
}

TEST(AsyncResult, LpDispatchRejectionDetectorMatchesNoOtherCode)
{
    // The negatives, at the only level this text-emitting generator can pin
    // them: the guards that make an unrecognised code, a 2- or 4-key object and
    // a non-string value all stay DATA must still be there. Widening the code
    // literal into a set must not have loosened the SHAPE.
    const QString src = lpSource();
    const int begin = src.indexOf("bool logosDispatchRejectionJson(");
    ASSERT_NE(begin, -1);
    const QString body = src.mid(begin, src.indexOf("} // namespace", begin) - begin);
    EXPECT_TRUE(body.contains("if (!v.is_object() || v.size() != 3) return false;"));
    EXPECT_TRUE(body.contains(
        "if (code == v.end() || message == v.end() || origin == v.end()) return false;"));
    EXPECT_TRUE(body.contains(
        "if (!code->is_string() || !message->is_string() || !origin->is_string()) return false;"));
    // Exactly three comparisons, one per code — not a prefix test, not a
    // "has a code key" shortcut.
    EXPECT_EQ(body.count("_code != \""), 3) << body.toStdString();
    EXPECT_LT(body.indexOf("_code != \"unknown_method\""), body.indexOf("return true;"));
}

TEST(AsyncResult, LpContractWithNoInvokableMethodEmitsNoDetector)
{
    // The detector is only reachable from a method body; emitting it anyway is
    // an unused function in an anonymous namespace, i.e. -Wunused-function.
    const QString src = makeSource("mod", "Mod", "mod.h", QJsonArray{}, ApiStyle::Lp);
    EXPECT_FALSE(src.contains("logosDispatchRejectionJson"));
}

TEST(AsyncResult, LpPlainAsyncEntryPointIsUnchanged)
{
    // No timeout was retro-fitted onto it — it has existing callers, and the
    // twin is where the new capability goes.
    const QString h = lpHeader();
    EXPECT_TRUE(h.contains("void addAsync(int64_t p0, int64_t p1, std::function<void(int64_t)> callback);"));
    EXPECT_TRUE(h.contains("void resetAsync(std::function<void()> callback);"));
    EXPECT_TRUE(lpSource().contains("m_client.invokeAsync(\"add\", _args, [callback](nlohmann::json _r) {"));
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
