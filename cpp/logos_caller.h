#pragma once
// ---------------------------------------------------------------------------
// WHO CALLED THIS HANDLER — the module-side half of logos_module_set_call_caller().
//
// A handler running inside a dispatch can ask `logos::currentCaller()` who is
// calling it. The value is AMBIENT: it is not a parameter, never appears in a
// .lidl, and no method opts in. logos-protocol's cpp/logos_module_impl.h holds
// the normative definition of the document parsed here; this header is the C++
// reader for it, exactly as logos-rust-sdk holds the Rust one.
//
// WHY THE IDENTITY HAS TO BE PUSHED ACROSS THE IMAGE BOUNDARY, which is the
// only reason any of this exists rather than a plain `thread_local` set by the
// host. The host binary and the module plugin each link their OWN copy of
// logos-protocol.
//
// Measured with nm on built binaries, not assumed — liblogos_core against a
// module plugin, on both object formats:
//
//   * BOTH images DEFINE TokenManager::instance() and both overloads of
//     ModuleProxy::callRemoteMethod. Not one defining and one importing: two
//     definitions.
//   * The function-local static behind them, TokenManager::instance()::instance,
//     is a LOCAL bss symbol (nm `b`) in each image, at a different address in
//     each. Being local, it is not a candidate for interposition on any
//     platform — there is simply one per image, always.
//   * NEITHER image holds a single undefined reference to the other's copy.
//   * The Mach-O plugin's header flags are MH_NOUNDEFS | MH_TWOLEVEL, so its
//     references are bound to a named library at link time and never resolved
//     against whatever the host happens to have loaded.
//
// So a `thread_local` the host sets is simply NOT the object a handler reads.
// The push is the mechanism; it is not a fallback for a better one that failed.
//
// The measurement turned up one asymmetry worth writing down, because it is
// the trap next to this one: on ELF the ACCESSOR functions are global (nm `T`)
// and therefore CAN be interposed between images, while on Mach-O and PE they
// cannot. That is exactly why the accessor below must not be exported — see
// the note on currentCaller().
//
// A PER-THREAD STACK, NOT A SINGLE SLOT. A handler that makes an outbound call
// spins a nested event loop, and a second inbound call can be delivered on that
// same thread inside it. With one slot, the inner call's clear would erase the
// outer call's caller and the outer handler would resume seeing Unknown — or,
// worse, seeing the inner caller. So a non-NULL push nests, a NULL pop removes
// the innermost, a pop with nothing pushed is a no-op, and every thread has its
// own stack.
//
// VALID ONLY DURING A DISPATCH, ON THE DISPATCHING THREAD. A worker the module
// spawned, a timer callback, a context-ready hook and an event emission all
// read Unknown — correctly, because none of them has a caller. A handler that
// needs the identity past its own frame copies it at the top.
//
// NOT MARKED FOR EXPORT, and deliberately so — see the note above
// `currentCaller()` for why that is right in both the static and the shared
// topology.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace logos {

// The arms of the caller document. Mirrors logos-rust-sdk's enum one-for-one;
// logos-protocol/cpp/logos_module_impl.h is the normative list for both.
//
// `Unknown` is not an error code. It is the honest answer whenever the identity
// is absent, unreadable, or expressed in a vocabulary this build predates.
enum class CallerKind {
    Unknown,
    Host,
    Module,
    Derived,
    Operator,
};

// A parsed caller identity.
//
// Field population by arm — anything not listed is empty:
//   Unknown   —
//   Host      —                       (rule 5: host carries no name, ever)
//   Module    name, instance (opt)
//   Derived   parent, leaf
//   Operator  name
struct LogosCaller {
    CallerKind kind = CallerKind::Unknown;
    std::string name;       // Module, Operator
    std::string instance;   // Module, optional (rule 6)
    std::string parent;     // Derived
    std::string leaf;       // Derived

    bool isUnknown() const { return kind == CallerKind::Unknown; }
    bool isHost() const { return kind == CallerKind::Host; }
    bool isDerived() const { return kind == CallerKind::Derived; }
    bool isOperator() const { return kind == CallerKind::Operator; }

    bool isModule() const { return kind == CallerKind::Module; }

    // Deliberately ignores `instance` (rule 6). A caller that must distinguish
    // one instance of a module from another compares the whole identity; this
    // predicate answers "is this chat_module", which is the question every call
    // site actually has, and it must keep answering it unchanged on the day
    // instance addressing starts being emitted.
    bool isModule(const std::string& moduleName) const
    {
        return kind == CallerKind::Module && name == moduleName;
    }
};

// Parse the normative caller document. NEVER throws and never reports failure
// out of band: every malformed, truncated, empty or unrecognised input is an
// Unknown caller.
//
// The degradation direction is the whole point and it is one-way. Adding an arm
// can only turn an old reader's isModule(x) from true to FALSE. A permissive
// reader — "kind I don't know, but there is a name, so call it a module" —
// would do the reverse and silently WIDEN a predicate that sits next to
// authorization decisions. So: no closest match, no partial values, no
// salvaging fields out of an arm whose required ones are missing.
// Hidden visibility, and it is load-bearing rather than hygiene.
//
// The comment on currentCaller() below argues that this state must NOT be
// unified across images. On ELF at DEFAULT visibility it is: a function-local
// static inside an inline function emits as STB_GNU_UNIQUE ("u" in .dynsym) and
// the dynamic linker deliberately collapses every image's copy into one, even
// under RTLD_LOCAL. Measured, two dlopen'd images: with default visibility a
// push in image A is READ BY image B; with hidden visibility B correctly reads
// Unknown. logos-module-builder sets no visibility anywhere, so real plugins are
// built the first way.
//
// So the property has to be asked for. It cannot be inherited from being inline
// — that was the claim, and it is false on the one platform where the bug this
// whole mechanism fixes is otherwise visible.
//
// An anonymous namespace would be strictly worse: vague linkage is load-bearing
// WITHIN an image, because the generated TU pushes and the author's TU reads,
// and they must agree on one object. Hidden keeps that and stops only the
// cross-image collapse. Not applied on MSVC, which has no equivalent and no
// STB_GNU_UNIQUE to defend against.
#if defined(__GNUC__) || defined(__clang__)
#  define LOGOS_CALLER_LOCAL __attribute__((visibility("hidden")))
#else
#  define LOGOS_CALLER_LOCAL
#endif

LOGOS_CALLER_LOCAL inline LogosCaller parseCaller(const std::string& json)
{
    LogosCaller caller;   // Unknown until proven otherwise.

    // allow_exceptions = false: a module handler must not be able to crash the
    // dispatch by being handed a bad document, and the host is not the only
    // thing that can produce one.
    const nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
    if (!doc.is_object())
        return caller;   // rule 1: unparseable, empty, or not an object

    const auto kindIt = doc.find("kind");
    if (kindIt == doc.end() || !kindIt->is_string())
        return caller;   // rule 1: "kind" is mandatory and must be a string

    // A required string field: present, a string, and non-empty. An empty name
    // is not an identity, so it is treated as absent rather than as a module
    // called "" that isModule("") would match.
    const auto required = [&doc](const char* key, std::string& out) {
        const auto it = doc.find(key);
        if (it == doc.end() || !it->is_string())
            return false;
        out = it->get<std::string>();
        return !out.empty();
    };

    const std::string kind = kindIt->get<std::string>();

    if (kind == "unknown") {
        return caller;
    }
    if (kind == "host") {
        // Rule 5: no name is read here even if one is present. "core" and
        // "capability_module" hold the same token VALUE under two keys by
        // construction, so a name on this arm would be a coin flip presented
        // as a fact. Any `name` in the document is an unrecognised field for
        // this arm and rule 3 says to ignore it.
        caller.kind = CallerKind::Host;
        return caller;
    }
    if (kind == "module") {
        if (!required("name", caller.name))
            return LogosCaller{};   // rule 4: missing required field ⇒ unknown
        // `instance` is optional. A non-string one is dropped rather than
        // failing the whole identity: it is not required, and isModule(name)
        // ignores it anyway, so degrading a usable name to Unknown over it
        // would lose more than it protects.
        const auto instIt = doc.find("instance");
        if (instIt != doc.end() && instIt->is_string())
            caller.instance = instIt->get<std::string>();
        caller.kind = CallerKind::Module;
        return caller;
    }
    if (kind == "derived") {
        if (!required("parent", caller.parent) || !required("leaf", caller.leaf))
            return LogosCaller{};   // rule 4
        caller.kind = CallerKind::Derived;
        return caller;
    }
    if (kind == "operator") {
        if (!required("name", caller.name))
            return LogosCaller{};   // rule 4
        caller.kind = CallerKind::Operator;
        return caller;
    }

    // Rule 2: an arm from a newer protocol. Unknown, not a guess.
    return LogosCaller{};
}

namespace detail {

// The per-thread stack. A function-local `thread_local` rather than a namespace
// -scope one so it is initialised on first use on every thread, including
// threads that existed before this image was dlopen'd.
//
// One stack PER IMAGE is the correct and intended scope: the generated
// logos_module_set_call_caller() that pushes and the handler that reads both
// live in the module image, so they share this object. The host has its own and
// never touches this one, which is precisely the separation the C ABI push
// exists to bridge.
LOGOS_CALLER_LOCAL inline std::vector<LogosCaller>& callerStack()
{
    static thread_local std::vector<LogosCaller> stack;
    return stack;
}

// The body of the generated logos_module_set_call_caller() export. Lives here,
// not in emitted text, so it is reachable by a unit test — generated source can
// only ever be asserted on as strings.
inline void setCallCaller(const char* callerJson)
{
    std::vector<LogosCaller>& stack = callerStack();
    if (callerJson) {
        stack.push_back(parseCaller(callerJson));
        return;
    }
    // A pop with nothing pushed is a no-op, not undefined behaviour: the host
    // and the module are separate processes' worth of independent lifetime, and
    // a clear that arrives without its push (a module loaded mid-dispatch, a
    // host that retries teardown) must not corrupt the stack or crash.
    if (!stack.empty())
        stack.pop_back();
}

} // namespace detail

// The caller of the dispatch currently running on THIS thread, or an Unknown
// caller outside a dispatch.
//
// NOT marked with any export macro, and that is deliberate in both topologies:
//
//   * Static / header-only, which is what logos-cpp-sdk is today — every target
//     in cpp/CMakeLists.txt is an INTERFACE library and no header in this repo
//     carries an export attribute. Marking this one dllexport would be a lie in
//     an image that is not a DLL, and MSVC rejects the same symbol later seen
//     as dllimport.
//
//   * Shared. This is the one symbol in the SDK that must NOT be unified across
//     images even if it could be. Give it default visibility out of a shared
//     runtime and ELF's flat namespace is entitled to interpose the host's copy
//     over the module's, so a handler would read the stack the host pushed to —
//     restoring, on Linux only, the exact bug the C ABI push was added to fix,
//     and restoring it in the one place where the other two platforms would
//     stay correct and hide it.
//
//     Being inline does NOT buy that separation — a function-local static in an
//     inline function emits STB_GNU_UNIQUE at default visibility and IS unified
//     across images. It is bought by LOGOS_CALLER_LOCAL above, which was
//     measured to restore per-image isolation. STB_GNU_UNIQUE additionally
//     pins an image against dlclose unmapping it, which the module teardown
//     path would rather not inherit.
LOGOS_CALLER_LOCAL inline const LogosCaller& currentCaller()
{
    const std::vector<LogosCaller>& stack = detail::callerStack();
    if (stack.empty()) {
        static const LogosCaller unknown;
        return unknown;
    }
    return stack.back();
}

} // namespace logos
