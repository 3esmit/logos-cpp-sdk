#pragma once

// Qt-FREE typed consumer client over the logos-protocol C ABI (lp_*).
//
// This is the std/C++ analog of rust-sdk's PluginProxy: it lets a module's
// generated typed wrappers call other modules and subscribe to their events
// WITHOUT touching Qt. The only dependency is logos-protocol's `extern "C"`
// surface (logos_protocol.h) — Qt stays confined to the QRO transport inside
// logos-protocol and to the generated Qt-plugin glue, never the module's own
// translation units.
//
// The generated `<Dep>` wrappers (ApiStyle::Lp) hold a `logos::LpClient` and
// marshal std args -> nlohmann JSON -> lp_invoke -> JSON -> std return. Event
// subscriptions go through lp_subscribe and are owned by an RAII
// `LpSubscription` (mirrors rust-sdk's EventSubscription: unsubscribes on
// destruction so the callback never fires after the owner is gone).

#include <atomic>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <vector>

#include "logos_protocol.h"     // lp_* C ABI
#include "logos_call_error.h"   // logos::CallError
#include "logos_json.h"         // LogosMap / LogosList aliases
#include "logos_codec.h"        // logos::bytesToJson, b64UrlDecode, isTaggedBytes
#include "logos_result.h"       // StdLogosResult

// DOES THIS PROTOCOL HAVE THE PER-TARGET SUBSCRIPTION SURFACE?
//
// Not a version question, which is why this is not a version guard. MINOR 0.9
// was revised in place: the first cut (48afc01) and the revision (47d287c) BOTH
// report MINOR 9 and export disjoint sets, so `MINOR >= 9` is true of a
// protocol that has none of these symbols. That guard shipped once and the
// doctests caught it, because they build downstream modules from those modules'
// own older locks.
//
// Two spellings because the transition needs both. LP_SUB_HELD is what
// discriminates TODAY — it arrived with the client-scoped surface and is absent
// from every protocol without it, including the first cut of 0.9.
// LOGOS_PROTOCOL_HAS_CLIENT_SUBSCRIPTION_STATE is the named macro protocol
// defines going forward, which keeps this working if the LP_SUB_* codes are
// ever reorganised.
#if defined(LOGOS_PROTOCOL_HAS_CLIENT_SUBSCRIPTION_STATE) || defined(LP_SUB_HELD)
#  define LOGOS_LP_HAS_CLIENT_SUBSCRIPTION_STATE 1
#endif

namespace logos {

// JSON -> std helpers used by the generated ApiStyle::Lp wrappers to decode
// return values and event payloads. Lenient: a type mismatch yields the
// default-constructed value (mirrors the Qt path's default-on-failure).
inline std::vector<std::string> jsonToStringVec(const nlohmann::json& j) {
    std::vector<std::string> out;
    if (j.is_array())
        for (const auto& e : j)
            if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}

// Binary payloads travel in the canonical tagged form
// {"_bytes": "<base64url, unpadded>"}. Encoding is logos::bytesToJson from
// logos_codec.h — the one canonical definition. Decoding on this path is NOT
// the codec's: bytesFromJson throws and bytesFromJsonLenient also accepts a
// plain string, a number and an int array, whereas every `lp` decoder here is
// documented to yield the default-constructed value on a mismatch. So the lp
// decode keeps its own deliberately-narrow spelling, next to jsonToStringVec
// which has exactly the same contract.
inline std::vector<uint8_t> jsonToBytes(const nlohmann::json& j) {
    if (!isTaggedBytes(j)) return {};
    return b64UrlDecode(j["_bytes"].get<std::string>());
}

// A reply that is NOT a result object is a decode failure, and must not be
// spelled as a default-constructed StdLogosResult.
//
// Every other lenient decode in this file bottoms out at a value no provider
// means as an answer -- "" for a string, 0 for a number, {} for a map. This one
// does not: `success = false` with an empty error is exactly what a provider
// sends when it REFUSES a call, so returning the default made "I could not read
// this reply" indistinguishable from "you were rejected". Measured on the Qt
// twin of this function (logos::jsonToLogosResult, same shape, same opening
// `if (!is_object)`), where a wrapper bound to a provider with a different
// return shape answered a refusal to every input including the well-formed
// ones.
//
// The leniency of the BARE SCALARS above is untouched and deliberate --
// logos-qt-sdk's tests/qt-generator bare_scalar_slots_stay_lenient pins it on
// the other surface, and tightening that is a decision about every scalar
// return of every module. This is the one type whose default is a message.
inline StdLogosResult jsonToStdResult(const nlohmann::json& j) {
    StdLogosResult r;
    if (!j.is_object()) {
        r.success = false;
        r.error = std::string("expected a result object, got ") + j.type_name();
        return r;
    }
    if (j.contains("success") && j["success"].is_boolean()) r.success = j["success"].get<bool>();
    if (j.contains("value"))                                r.value = j["value"];
    if (j.contains("error") && j["error"].is_string())      r.error = j["error"].get<std::string>();
    return r;
}

// The subscription-status codes as a type, so author code never names a macro
// and a switch over them can be exhaustive.
enum class SubStatus {
    Armed     = 1,
    Lost      = 2,
    Abandoned = 3,
    Held      = 4,
};

// What the registry does when an ARMED subscription loses its provider.
// Automatic is what every subscription has always done. Manual means "do not
// RE-arm after a loss" and never "do not arm the first time" — a subscription
// taken during init(), before its provider has called listen(), is deferred and
// armed under either policy.
enum class RestartPolicy { Automatic = 0, Manual = 1 };

// The one place a lp_subscription* is freed.
//
// lp_unsubscribe DELETES the handle, and from 0.10 there are two things that
// can ask for it: the owning LpSubscription going out of scope, and a
// SubHandle::cancel() from inside a status callback. Both go through this cell,
// and the exchange decides: whoever swaps the pointer out non-null owns the
// free, the loser sees nullptr and does nothing. Without it the two paths are a
// double free, and the window is not theoretical — cancelling from the status
// callback while the owner unwinds is the obvious way to use both features
// together.
using LpSubCell = std::shared_ptr<std::atomic<lp_subscription*>>;

inline bool lpSubCellRelease(const LpSubCell& cell) {
    if (!cell) return false;
    lp_subscription* s = cell->exchange(nullptr, std::memory_order_acq_rel);
    if (!s) return false;
    lp_unsubscribe(s);
    return true;
}

// A non-owning ticket for ONE subscription, which is what a generated
// `on<Event>()` returns. Copyable, and safe to keep: it holds a weak reference
// to the cell, so every operation answers false once the owning LpSubscription
// is gone. That weakness is the point — a ticket that kept the subscription
// alive would turn "I watched it" into "I own it".
//
// It exists because the generated wrappers store the owning handle in a private
// vector, so without a ticket a module author has no way to unsubscribe from
// something they subscribed to.
//
// STATE IS NOT HERE. Arming, loss and the generation belong to the TARGET
// MODULE, not to one subscription of it — every subscription to a module shares
// its single handle and they rise and fall together — so they are read and
// controlled on the client (onSubscriptionStatus, subscriptionGeneration,
// setRestartPolicy, rearmSubscriptions). What is genuinely per-subscription is
// cancelling it, and that is what this carries.
class SubHandle {
public:
    SubHandle() = default;

    // Unsubscribe for good. Idempotent, and safe to race the owner's
    // destructor — the cell's exchange picks exactly one winner.
    bool cancel() const { return lpSubCellRelease(m_cell.lock()); }

    bool expired() const {
        auto cell = m_cell.lock();
        return !cell || cell->load(std::memory_order_acquire) == nullptr;
    }

    // NON-EXPLICIT, which is a deliberate exception to the usual rule. This is
    // what a generated `on<Event>()` returns, and that accessor used to return
    // bool: every existing `if (dep.onFoo(cb))` and `bool ok = dep.onFoo(cb);`
    // has to keep compiling and keep meaning the same thing. An explicit
    // conversion breaks the second form silently enough to be worth avoiding —
    // and the truthiness here is unambiguous, since a handle is either a live
    // subscription or nothing.
    operator bool() const { return !expired(); }

    // The price of that implicit conversion: without these, `a == b` would
    // silently compare the two bools rather than the handles.
    friend bool operator==(const SubHandle&, const SubHandle&) = delete;
    friend bool operator!=(const SubHandle&, const SubHandle&) = delete;

private:
    friend class LpSubscription;
    explicit SubHandle(const LpSubCell& cell) : m_cell(cell) {}
    std::weak_ptr<std::atomic<lp_subscription*>> m_cell;
};

// RAII handle for an lp_subscription. Owns the subscription and the heap
// callback box; unsubscribes (after which no further callbacks fire) and frees
// the box on destruction. Move-only.
class LpSubscription {
public:
    LpSubscription() = default;
    LpSubscription(lp_subscription* sub, void* cbBox, void (*deleter)(void*))
        : m_cell(std::make_shared<std::atomic<lp_subscription*>>(sub)),
          m_cbBox(cbBox), m_deleter(deleter) {}

    LpSubscription(LpSubscription&& o) noexcept { moveFrom(o); }
    LpSubscription& operator=(LpSubscription&& o) noexcept {
        if (this != &o) { reset(); moveFrom(o); }
        return *this;
    }
    LpSubscription(const LpSubscription&) = delete;
    LpSubscription& operator=(const LpSubscription&) = delete;
    ~LpSubscription() { reset(); }

    bool valid() const {
        return m_cell && m_cell->load(std::memory_order_acquire) != nullptr;
    }


    // Unsubscribe now rather than at destruction. Idempotent, and safe to race
    // with the destructor — see LpSubCell.
    void cancel() { lpSubCellRelease(m_cell); }

    // A non-owning ticket for this subscription, for handing to code that must
    // be able to act on it without owning it — a status callback, or a
    // generated wrapper that keeps the handle privately.
    SubHandle handle() const { return SubHandle(m_cell); }

private:
    void moveFrom(LpSubscription& o) {
        m_cell = std::move(o.m_cell); m_cbBox = o.m_cbBox; m_deleter = o.m_deleter;
        o.m_cell.reset(); o.m_cbBox = nullptr; o.m_deleter = nullptr;
    }
    void reset() {
        lpSubCellRelease(m_cell);
        m_cell.reset();
        if (m_cbBox && m_deleter) { m_deleter(m_cbBox); m_cbBox = nullptr; }
    }
    LpSubCell m_cell;
    void* m_cbBox = nullptr;
    void (*m_deleter)(void*) = nullptr;
};

// Qt-free typed client for one target module. The lp_client is created lazily
// on first use, on behalf of `origin` (the calling module's name, baked by the
// generated umbrella), over the process-default transport with the automatic
// capability/token flow that logos-protocol provides.
class LpClient {
public:
    LpClient(std::string target, std::string origin)
        : m_target(std::move(target)), m_origin(std::move(origin)) {}
    ~LpClient() {
        if (lp_client* c = m_client.load(std::memory_order_acquire))
            lp_client_destroy(c);
    }
    LpClient(const LpClient&) = delete;
    LpClient& operator=(const LpClient&) = delete;

    // Blocking call. `args` is a JSON array. Returns the result JSON value
    // (null on failure); fills `err` when non-null.
    //
    // `timeout_ms <= 0` selects the protocol default (the C ABI's rule), which
    // is what every caller got before the parameter existed — so adding it
    // changes nothing for them. It exists because the Qt-typed consumer surface
    // takes a `Timeout` on every async overload and had nowhere to put it: a
    // wrapper delegating here silently dropped the caller's timeout.
    nlohmann::json invoke(const std::string& method,
                          const nlohmann::json& args,
                          CallError* err,
                          int timeout_ms = 0) {
        lp_client* c = ensure();
        if (!c) {
            if (err) { err->code = "object_unavailable";
                       err->message = "could not create client for " + m_target;
                       err->origin = m_target; }
            return nullptr;
        }
        const std::string argsStr = args.dump();
        char* outRes = nullptr;
        char* outErr = nullptr;
        const int rc = lp_invoke(c, method.c_str(), argsStr.c_str(), timeout_ms, &outRes, &outErr);
        nlohmann::json result;  // null
        if (rc == LP_OK) {
            if (err) err->clear();
            if (outRes) {
                auto parsed = nlohmann::json::parse(outRes, nullptr, /*allow_exceptions=*/false);
                if (!parsed.is_discarded()) result = std::move(parsed);
            }
        } else {
            fillErr(err, outErr, rc);
        }
        if (outRes) lp_string_free(outRes);
        if (outErr) lp_string_free(outErr);
        return result;
    }

    // Async call. `cb` fires exactly once with the result JSON (null on
    // failure / parse error). Safe to call from any thread.
    void invokeAsync(const std::string& method,
                     const nlohmann::json& args,
                     std::function<void(nlohmann::json)> cb,
                     int timeout_ms = 0) {
        lp_client* c = ensure();
        if (!c) { if (cb) cb(nullptr); return; }
        auto* box = new std::function<void(nlohmann::json)>(std::move(cb));
        const std::string argsStr = args.dump();
        lp_invoke_async(c, method.c_str(), argsStr.c_str(), timeout_ms,
            &LpClient::resultTrampoline, box);
    }

    // Async call carrying the error — the async twin of invoke()'s `err`
    // out-parameter, and what the generated `<name>AsyncResult` wrappers are
    // built on. `cb` fires exactly once; on failure the JSON is null and the
    // CallError is populated from the C ABI's canonical {code, message, origin}
    // object.
    //
    // Why this exists next to invokeAsync rather than replacing it: invokeAsync
    // collapses the C ABI's failure form (`ok == 0` with `json` set to the error
    // object) into a bare JSON null, which is also what a successful call
    // returning nothing delivers. That is fine for a callback that only takes a
    // value and has nowhere to put an error, and useless for one that does.
    //
    // A DISTINCT NAME, not an overload of invokeAsync: two std::function
    // parameters differing only in arity are ambiguous for a generic lambda —
    // the same hazard that made the generator spell `<name>AsyncResult` as its
    // own name instead of an overload of `<name>Async`.
    //
    // Safe to call from any thread. logos-qt-sdk's LpBridge::invokeAsyncResult
    // is this function with a private second lp_client bolted on because this
    // one did not exist; it can now delegate here and drop that connection.
    void invokeAsyncResult(const std::string& method,
                           const nlohmann::json& args,
                           std::function<void(nlohmann::json, const CallError&)> cb,
                           int timeout_ms = 0) {
        if (!cb) return;
        lp_client* c = ensure();
        if (!c) {
            cb(nlohmann::json(),
               callErrorObjectUnavailable(m_target, "could not create client for " + m_target));
            return;
        }
        auto* box = new ResultErrBox(std::move(cb));
        const std::string argsStr = args.dump();
        const int rc = lp_invoke_async(c, method.c_str(), argsStr.c_str(), timeout_ms,
                                       &LpClient::resultErrorTrampoline, box);
        if (rc != LP_OK) {
            // A synchronous refusal does NOT call back (the C ABI's rule), so
            // the completion is this function's to make — `cb` still has to fire
            // exactly once, which is the whole contract a caller schedules on.
            ResultErrBox fn = std::move(*box);
            delete box;
            fn(nlohmann::json(),
               callErrorCallFailed(m_target, "lp_invoke_async refused the call (rc="
                                                 + std::to_string(rc) + ")"));
        }
    }

    // The target's method list, as the JSON the host reports. Empty on
    // failure. Invoke-without-introspect is what makes a by-name call an
    // escape hatch rather than an API: a caller that cannot ask what exists
    // can only guess, and a wrong guess fails at runtime like a typo.
    nlohmann::json getMethods() {
        lp_client* c = ensure();
        if (!c) return nlohmann::json();
        char* out = lp_get_methods(c);
        if (!out) return nlohmann::json();
        auto parsed = nlohmann::json::parse(out, nullptr, /*allow_exceptions=*/false);
        lp_string_free(out);
        return parsed.is_discarded() ? nlohmann::json() : parsed;
    }

    // Subscribe to `event`. The payload is delivered as a JSON array. The
    // returned handle owns the subscription — keep it alive (the generated
    // wrapper stores it) for as long as you want the callback to fire.
    LpSubscription subscribe(const std::string& event,
                             std::function<void(nlohmann::json)> cb) {
        lp_client* c = ensure();
        if (!c) return {};
        auto* box = new std::function<void(nlohmann::json)>(std::move(cb));
        lp_subscription* sub = lp_subscribe(c, event.c_str(), &LpClient::eventTrampoline, box);
        if (!sub) { delete box; return {}; }
        return LpSubscription(sub, box, &LpClient::deleteBox);
    }

    // ── the target's subscription state ────────────────────────────────────
    //
    // All four are per TARGET MODULE, because that is the granularity the
    // phenomenon has: this client names one module, every subscribe() through
    // it attaches to that module's single handle, and a provider that dies
    // takes all of them down together. A per-subscription version of any of
    // these would report one event once per subscription and let a caller
    // believe the halves could differ.
    //
    // DECLARED UNCONDITIONALLY, with the version fan-out inside each body. That
    // placement is the point: the code generators do not receive a protocol
    // version, so an emitted wrapper can call these and stay identical across
    // every protocol the SDK supports, with this header absorbing the
    // difference. A guarded DECLARATION would push that arithmetic into every
    // emitter instead.
    //
    // Below 0.9 none of it can be honoured, and each warns ONCE rather than
    // degrading quietly — silently turning Manual into Automatic is the same
    // class of failure as the silent re-arm this whole surface exists to
    // remove.

    // Watch this module's transitions: Armed / Lost / Held / Abandoned, with
    // the establishment number. The pair that matters is Lost followed by Armed
    // with a higher generation — the provider restarted, so every subscription
    // is new and everything emitted in between is unrecoverable. subscribe()
    // alone cannot report that; the stream simply resumes with a hole in it.
    //
    // Installable before the first subscribe, and replays the current state, so
    // there is no order in which a caller can miss the arm.
    void onSubscriptionStatus(std::function<void(SubStatus, std::uint64_t generation)> cb) {
        lp_client* c = ensure();
        if (!c) return;
#if defined(LOGOS_LP_HAS_CLIENT_SUBSCRIPTION_STATE)
        {
            std::lock_guard<std::mutex> lk(m_statusMu);
            m_status = std::move(cb);
        }
        lp_client_set_subscription_status_cb(c, &LpClient::statusTrampoline, this);
#else
        if (cb) warnOnce(kNoStatusChannel, "onSubscriptionStatus");
#endif
    }

    // Which establishment this module is on: 0 = never armed, 1 = the first,
    // N+1 after each re-establishment. Readable with no callback at all, which
    // is what gives gap detection to a consumer that changes nothing else.
    std::uint64_t subscriptionGeneration() {
        lp_client* c = ensure();
        if (!c) return 0;
#if defined(LOGOS_LP_HAS_CLIENT_SUBSCRIPTION_STATE)
        return lp_client_subscription_generation(c);
#else
        return 0;
#endif
    }

    // What happens to this module's subscriptions when its provider goes away.
    // Manual means "do not RE-arm after a loss", never "do not arm the first
    // time" — a subscription taken during init(), before the provider has
    // called listen(), is deferred and armed under either policy.
    void setRestartPolicy(RestartPolicy policy) {
        lp_client* c = ensure();
        if (!c) return;
#if defined(LOGOS_LP_HAS_CLIENT_SUBSCRIPTION_STATE)
        lp_client_set_subscription_options(
            c, policy == RestartPolicy::Manual ? "{\"restart\":\"manual\"}"
                                               : "{\"restart\":\"automatic\"}");
#else
        if (policy == RestartPolicy::Manual) warnOnce(kNoRestartPolicy, "setRestartPolicy");
#endif
    }

    // Revive this module's held subscriptions. Safe from inside the status
    // callback: the ABI posts the revive rather than marshalling it
    // synchronously. Answers "accepted", not "re-armed" — watch for Armed with
    // a higher generation for that.
    bool rearmSubscriptions() {
        lp_client* c = ensure();
        if (!c) return false;
#if defined(LOGOS_LP_HAS_CLIENT_SUBSCRIPTION_STATE)
        return lp_client_rearm_subscriptions(c) != 0;
#else
        return false;
#endif
    }

private:
    using Box = std::function<void(nlohmann::json)>;
    using ResultErrBox = std::function<void(nlohmann::json, const CallError&)>;

    // Create-once, and never while holding a lock.
    //
    // Two threads reach a dep's FIRST call concurrently more often than the
    // lazy-init shape suggests: a concurrency:"multi" module runs its handlers
    // on concurrent QThreads, and any module with a worker of its own (an HTTP
    // handler, a chain-sync pump) races that worker against the dispatch
    // thread. The plain `if (!m_client) m_client = lp_client_create(...)` this
    // replaces was a data race on m_client, and leaked whichever client lost.
    //
    // A mutex around the whole body is the obvious fix and the WRONG one. For a
    // Qt-affine transport lp_client_create marshals construction onto the Qt
    // main thread and BLOCKS there (logos_protocol.cpp's runOnQtMainThread). A
    // worker holding the lock across that waits for the main thread — while the
    // main thread, reaching this same ensure() from an inbound call, waits for
    // the lock and so never returns to the event loop that would run the
    // construction. That trades a data race for a deadlock.
    //
    // So construct OUTSIDE any lock and publish with a CAS. Both racers may
    // build a client; exactly one is ever published, and the loser destroys its
    // own. That is safe and cheap: lp_client_destroy may be called from any
    // thread and defers the teardown to the owner thread (logos_protocol.h),
    // and construction has no effect at the target — the capability handshake
    // is lazy, inside invokeRemoteMethod — so a discarded client mints no token
    // and leaves no trace.
    //
    // A failed create is deliberately NOT latched: the next call retries, which
    // is what the pre-CAS version did.
    lp_client* ensure() {
        if (lp_client* c = m_client.load(std::memory_order_acquire))
            return c;
        lp_client* fresh = lp_client_create(m_target.c_str(), m_origin.c_str(), nullptr, nullptr);
        // Creation failed — report whatever is published (usually null, but a
        // racer may have succeeded meanwhile) rather than caching the failure.
        if (!fresh) return m_client.load(std::memory_order_acquire);
        lp_client* expected = nullptr;
        if (m_client.compare_exchange_strong(expected, fresh,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire))
            return fresh;
        lp_client_destroy(fresh);   // lost the publish race
        return expected;
    }

    static void resultTrampoline(int ok, const char* json, void* ud) {
        auto* fn = static_cast<Box*>(ud);
        nlohmann::json r;  // null
        if (ok && json) {
            auto parsed = nlohmann::json::parse(json, nullptr, false);
            if (!parsed.is_discarded()) r = std::move(parsed);
        }
        (*fn)(std::move(r));
        delete fn;  // result callback fires exactly once
    }

    // The error-aware twin of resultTrampoline. `ok == 0` means `json` is the
    // canonical error object rather than a value, so the value is dropped and
    // the error decoded; a malformed/absent one still yields a NON-ok
    // CallError, because reporting ok() for a call the ABI said failed is the
    // one outcome this trampoline exists to prevent.
    static void resultErrorTrampoline(int ok, const char* json, void* ud) {
        auto* fn = static_cast<ResultErrBox*>(ud);
        nlohmann::json parsed;  // null
        if (json) {
            auto p = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
            if (!p.is_discarded()) parsed = std::move(p);
        }
        CallError err;
        if (!ok) {
            err = callErrorCallFailed("", "lp_invoke_async failed");
            if (parsed.is_object()) {
                if (parsed.contains("code") && parsed["code"].is_string())
                    err.code = parsed["code"].get<std::string>();
                if (parsed.contains("message") && parsed["message"].is_string())
                    err.message = parsed["message"].get<std::string>();
                if (parsed.contains("origin") && parsed["origin"].is_string())
                    err.origin = parsed["origin"].get<std::string>();
            }
            parsed = nlohmann::json();
        }
        (*fn)(std::move(parsed), err);
        delete fn;  // result callback fires exactly once
    }

    static void eventTrampoline(const char* /*eventName*/, const char* dataJson, void* ud) {
        auto* fn = static_cast<Box*>(ud);
        nlohmann::json r = nlohmann::json::array();
        if (dataJson) {
            auto parsed = nlohmann::json::parse(dataJson, nullptr, false);
            if (!parsed.is_discarded()) r = std::move(parsed);
        }
        (*fn)(std::move(r));
    }

    static void deleteBox(void* p) { delete static_cast<Box*>(p); }

    // The status trampoline is per CLIENT, so its user_data is the client
    // itself rather than a per-subscription box — there is exactly one of these
    // per target and it outlives every subscription through it.
    //
    // The callback is copied out under the lock and invoked outside it: a
    // watcher's obvious move on Held is rearmSubscriptions(), and holding
    // m_statusMu across that would deadlock against a concurrent installer.
    static void statusTrampoline(int state, unsigned long long generation,
                                 const char* /*reason*/, void* ud) {
        auto* self = static_cast<LpClient*>(ud);
        std::function<void(SubStatus, std::uint64_t)> cb;
        {
            std::lock_guard<std::mutex> lk(self->m_statusMu);
            cb = self->m_status;
        }
        if (!cb) return;
#if defined(LOGOS_LP_HAS_CLIENT_SUBSCRIPTION_STATE)
        // The WHOLE switch is guarded, not just the Held case: none of the
        // LP_SUB_* codes exist below the revision — 0.8 has no LP_SUB_ARMED
        // either. LpClient is a plain class, so these bodies are checked at
        // class close whether or not anything calls them; leaving three of the
        // four labels outside the guard broke the 0.8 build with the other two
        // compiling out perfectly around them.
        //
        // An UNKNOWN code is dropped rather than coerced. A newer protocol can
        // introduce a status this build has no name for, and reporting it as
        // Armed -- the numerically-first value -- would tell a subscriber its
        // subscriptions are live at the one moment that might not be true.
        switch (state) {
            case LP_SUB_ARMED:     cb(SubStatus::Armed,     generation); break;
            case LP_SUB_LOST:      cb(SubStatus::Lost,      generation); break;
            case LP_SUB_ABANDONED: cb(SubStatus::Abandoned, generation); break;
            case LP_SUB_HELD:      cb(SubStatus::Held,      generation); break;
            default: break;
        }
#else
        (void)state; (void)generation; (void)cb;
#endif
    }

    // One line per missing capability per process, not per call: a module that
    // configures forty deps against an old runtime should say so once, not
    // forty times.
    static constexpr int kNoRestartPolicy = 0;
    static constexpr int kNoStatusChannel = 1;
    static void warnOnce(int which, const std::string& what) {
        static std::atomic<bool> said[2] = {{false}, {false}};
        if (said[which].exchange(true)) return;
        std::fprintf(stderr,
            "logos: %s: this runtime is logos-protocol %s, which has no "
            "per-module subscription %s (needs 0.9). Subscriptions are live, but "
            "that setting is NOT in effect.\n",
            what.c_str(), LOGOS_PROTOCOL_VERSION_STRING,
            which == kNoRestartPolicy ? "restart policy" : "status channel");
    }

    static void fillErr(CallError* err, const char* errJson, int rc) {
        if (!err) return;
        err->code = "call_failed";
        err->message = "lp_invoke failed (rc=" + std::to_string(rc) + ")";
        err->origin.clear();
        if (errJson) {
            auto j = nlohmann::json::parse(errJson, nullptr, false);
            if (!j.is_discarded() && j.is_object()) {
                if (j.contains("code") && j["code"].is_string())       err->code = j["code"].get<std::string>();
                if (j.contains("message") && j["message"].is_string()) err->message = j["message"].get<std::string>();
                if (j.contains("origin") && j["origin"].is_string())   err->origin = j["origin"].get<std::string>();
            }
        }
    }

    std::string m_target;
    std::string m_origin;
    // Published exactly once by ensure(); read from any thread.
    std::atomic<lp_client*> m_client{nullptr};
    // The target's status watcher. Guarded rather than atomic because a
    // std::function is not trivially copyable, and installed rarely — once at
    // construction for essentially every caller.
    std::mutex m_statusMu;
    std::function<void(SubStatus, std::uint64_t)> m_status;
};

}  // namespace logos
