#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// logos_host_services.h — the C++ veneer over the privileged host services.
//
// These are the operations an ordinary module must NOT have: enumerating the
// token store, and pushing an auth token to an arbitrary target. Exactly one
// module in a normal deployment needs them — capability_module, the trust root
// — which is why they are a declared, host-granted privilege rather than part
// of the ambient SDK surface.
//
// A module declares what it needs in metadata.json:
//
//     "host_services": ["token_registry", "token_delivery"]
//
// and the HOST decides whether to grant it, pushing the grant in over the
// module-impl C ABI (logos_module_grant_host_services). Until that happens
// every call here fails closed with LP_ERR_UNSUPPORTED — the declaration is
// advisory, the grant is authority.
//
// Qt-free by construction: this is a header-only wrapper over lp_* and
// nlohmann::json, so a universal/cdylib module can use it from translation
// units that never see Qt.
//
// ── Why these are free functions and not a LogosModuleContext seam ───────────
// The grant is process-global *per image* (see logos_protocol.h: the host
// binary and a module cdylib each link their own copy of logos-protocol, so
// each has its own grant state and its own TokenManager). The gate these
// functions check therefore lives in the caller's own image, and there is
// nothing per-instance to inject. Adding a context seam would imply the
// privilege is a property of one impl object, which it is not.
// ─────────────────────────────────────────────────────────────────────────────

#include "logos_protocol.h"  // lp_* C ABI

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace logos {
namespace host {

/// Outcome of a privileged call. `ok == false` with `code == LP_ERR_UNSUPPORTED`
/// is the ordinary, expected answer for a module that was never granted the
/// service — callers should treat it as "not permitted", not as a failure to
/// retry.
struct Status {
    bool ok = false;
    int code = 0;

    explicit operator bool() const { return ok; }

    static Status fromCode(int c) { return Status{c == LP_OK, c}; }
    /// True when the call was refused for want of a grant, as opposed to
    /// failing on its own terms.
    bool ungranted() const { return !ok && code == LP_ERR_UNSUPPORTED; }
};

namespace detail {

/// lp_* returns heap `char*` that the caller must release through
/// lp_string_free — never free() or delete. Owning it here keeps every exit
/// path (including the parse-failure ones) from leaking.
class OwnedString {
public:
    explicit OwnedString(char* p) : m_p(p) {}
    ~OwnedString() { if (m_p) lp_string_free(m_p); }
    OwnedString(const OwnedString&) = delete;
    OwnedString& operator=(const OwnedString&) = delete;
    OwnedString(OwnedString&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }

    const char* get() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }

private:
    char* m_p = nullptr;
};

} // namespace detail

/// The module names this image's token store holds.
///
/// Requires the "token_registry" service. Returns an empty vector both when the
/// service was not granted and when the store is genuinely empty; pass
/// `status` when the difference matters — it is the whole point of the gate.
inline std::vector<std::string> tokenKeys(Status* status = nullptr)
{
    detail::OwnedString raw(lp_token_keys());
    if (!raw) {
        // The C surface signals refusal by returning null rather than a code,
        // so map it onto the same "ungranted" answer the other call reports.
        if (status) *status = Status{false, LP_ERR_UNSUPPORTED};
        return {};
    }

    nlohmann::json parsed = nlohmann::json::parse(raw.get(), nullptr,
                                                  /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        if (status) *status = Status{false, LP_ERR_INVALID_ARG};
        return {};
    }

    std::vector<std::string> keys;
    keys.reserve(parsed.size());
    for (const nlohmann::json& e : parsed) {
        if (e.is_string()) keys.push_back(e.get<std::string>());
    }
    if (status) *status = Status{true, LP_OK};
    return keys;
}

/// The token this image holds for `moduleName`, or empty when there is none.
///
/// ⚠️ NOT gated. `lp_token_get` performs no host-service check
/// (logos_protocol.cpp), so ANY module in this image can look up ANY name it
/// can guess — "token_registry" gates ENUMERATION (lp_token_keys) only, which
/// is what stops a module discovering names it was never told. Do not read the
/// presence of this function as a privilege boundary; if lookup is meant to be
/// gated too, that gate belongs in lp_token_get, not here.
inline std::string tokenFor(const std::string& moduleName)
{
    detail::OwnedString raw(lp_token_get(moduleName.c_str()));
    return raw ? std::string(raw.get()) : std::string();
}

/// Deliver the token for `moduleName` TO `originModule`, over `client`.
///
/// Requires the "token_delivery" service. This is the PROVIDER half of the
/// exchange — the direction capability_module pushes — and is deliberately not
/// `lp_inform_module_token`, which always lands on capability_module whatever
/// the client's target is.
///
/// `authToken` is the caller's own token for the target, as usual.
/// `timeoutMs <= 0` selects the protocol default (20s), which bounds the
/// handshake-surface fallback and the call together.
inline Status informModuleTokenTo(lp_client* client,
                                  const std::string& authToken,
                                  const std::string& originModule,
                                  const std::string& moduleName,
                                  const std::string& token,
                                  int timeoutMs = 0)
{
    return Status::fromCode(
        lp_inform_module_token_to(client, authToken.c_str(), originModule.c_str(),
                                  moduleName.c_str(), token.c_str(), timeoutMs));
}

/// Constant-time string comparison, for validating a presented token against a
/// stored one.
///
/// Deliberately provided HERE rather than left to each caller: the natural
/// spelling (`a == b`) leaks the length of the matching prefix through timing,
/// and a trust root that compares tokens with `==` is the exact bug this file
/// exists to prevent. Compares length first — the lengths of these tokens are
/// not secret — then every remaining byte with no early exit.
inline bool constantTimeEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

} // namespace host
} // namespace logos
