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

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <vector>

#include "logos_protocol.h"     // lp_* C ABI
#include "logos_call_error.h"   // logos::CallError
#include "logos_json.h"         // LogosMap / LogosList aliases
#include "logos_codec.h"        // logos::bytesToJson, b64UrlDecode, isTaggedBytes
#include "logos_result.h"       // StdLogosResult

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

inline StdLogosResult jsonToStdResult(const nlohmann::json& j) {
    StdLogosResult r;
    if (j.is_object()) {
        if (j.contains("success") && j["success"].is_boolean()) r.success = j["success"].get<bool>();
        if (j.contains("value"))                                r.value = j["value"];
        if (j.contains("error") && j["error"].is_string())      r.error = j["error"].get<std::string>();
    }
    return r;
}

// RAII handle for an lp_subscription. Owns the subscription and the heap
// callback box; unsubscribes (after which no further callbacks fire) and
// frees the box on destruction. Move-only.
class LpSubscription {
public:
    LpSubscription() = default;
    LpSubscription(lp_subscription* sub, void* cbBox, void (*deleter)(void*))
        : m_sub(sub), m_cbBox(cbBox), m_deleter(deleter) {}

    LpSubscription(LpSubscription&& o) noexcept { moveFrom(o); }
    LpSubscription& operator=(LpSubscription&& o) noexcept {
        if (this != &o) { reset(); moveFrom(o); }
        return *this;
    }
    LpSubscription(const LpSubscription&) = delete;
    LpSubscription& operator=(const LpSubscription&) = delete;
    ~LpSubscription() { reset(); }

    bool valid() const { return m_sub != nullptr; }

private:
    void moveFrom(LpSubscription& o) {
        m_sub = o.m_sub; m_cbBox = o.m_cbBox; m_deleter = o.m_deleter;
        o.m_sub = nullptr; o.m_cbBox = nullptr; o.m_deleter = nullptr;
    }
    void reset() {
        if (m_sub) { lp_unsubscribe(m_sub); m_sub = nullptr; }
        if (m_cbBox && m_deleter) { m_deleter(m_cbBox); m_cbBox = nullptr; }
    }
    lp_subscription* m_sub = nullptr;
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
    ~LpClient() { if (m_client) lp_client_destroy(m_client); }
    LpClient(const LpClient&) = delete;
    LpClient& operator=(const LpClient&) = delete;

    // Blocking call. `args` is a JSON array. Returns the result JSON value
    // (null on failure); fills `err` when non-null.
    nlohmann::json invoke(const std::string& method,
                          const nlohmann::json& args,
                          CallError* err) {
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
        const int rc = lp_invoke(c, method.c_str(), argsStr.c_str(), 0, &outRes, &outErr);
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
                     std::function<void(nlohmann::json)> cb) {
        lp_client* c = ensure();
        if (!c) { if (cb) cb(nullptr); return; }
        auto* box = new std::function<void(nlohmann::json)>(std::move(cb));
        const std::string argsStr = args.dump();
        lp_invoke_async(c, method.c_str(), argsStr.c_str(), 0,
            &LpClient::resultTrampoline, box);
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

private:
    using Box = std::function<void(nlohmann::json)>;

    lp_client* ensure() {
        if (!m_client)
            m_client = lp_client_create(m_target.c_str(), m_origin.c_str(), nullptr, nullptr);
        return m_client;
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
    lp_client* m_client = nullptr;
};

}  // namespace logos
