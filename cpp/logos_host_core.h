#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// logos_host_core.h — the C++ veneer over liblogos' core-management C API.
//
// This is for HOST programs: the ones that stand up a Logos core and then load
// modules into it (logos-basecamp, logos-logoscore-cli, logos-standalone-app,
// logos-module-viewer). A module never needs it — a module is loaded BY a host
// and reaches its declared dependencies through the generated `LogosModules`
// aggregate instead.
//
// ── Why a wrapper at all ────────────────────────────────────────────────────
// Four hosts currently open-code the same `logos_core_*` calls, and the C API
// has three classes of hazard that a call site cannot see:
//
//   1. OWNERSHIP. Six entry points return `char**` / `char*` that the caller
//      must free — and liblogos allocates them with `new char[]` /
//      `new char*[]` (module_manager.cpp::toNullTerminatedArray,
//      logos_core.cpp:85), so `delete[]` is correct and `free()` is undefined
//      behaviour. Nothing in the signature says so.
//   2. ORDERING. Three setters must precede `logos_core_start()`, and
//      `set_module_transports` must precede the target module's LOAD. Today
//      those constraints exist only as comments in logos_core.h.
//   3. SHAPE. `logos_core_get_module_stats()` takes NO module name — it
//      returns one JSON array covering every loaded module. Every caller that
//      wants one module's stats has to parse and index it.
//
// This header turns (1) into RAII, (2) into constructor arguments (the illegal
// order stops being representable), and (3) into one parse.
//
// ── Why it is a plain object, with no codegen and no injection seam ─────────
// Contrast LogosModuleContext, which needs `_logosCoreSetContext_`, a `void*`
// round-trip in `modules()`, and SFINAE `maybeSet*` helpers. All of that exists
// because a module impl is USER-AUTHORED but FRAMEWORK-INSTANTIATED: the
// generated provider must inject state into an object it did not construct,
// belonging to a class that may or may not inherit the base.
//
// A host has none of those constraints. The host IS main(). It constructs this
// object itself, nothing injects into it, and no generator is involved because
// the host is not generated. So this is an ordinary RAII class.
//
// For the same reason there is deliberately NO `modules()` here. `LogosModules`
// is emitted per-build from the host's own metadata.json#dependencies; the host
// includes its own `logos_sdk.h` and can simply hold one. Only the SDK-side
// module context needs the `void*` indirection, because only it must name a
// type it cannot see.
//
// ── Why the C API is re-declared below rather than included ─────────────────
// logos-liblogos DEPENDS ON logos-cpp-sdk (logos-liblogos/flake.nix:7), so this
// header cannot include liblogos' `logos_core.h` without inverting the
// dependency graph. The declarations below are therefore a hand-maintained
// mirror — which is what every host does today anyway (see
// logos-basecamp/app/CoreModuleManager.cpp), except that it now exists ONCE
// instead of four times. The host links liblogos; this header only declares.
//
// Header-only, Qt-free, and it adds no link edge: `cpp/CMakeLists.txt` exports
// an INTERFACE library and this drops straight into it.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// ── liblogos' core-management C ABI ─────────────────────────────────────────
// Mirror of logos-liblogos/src/logos_core/logos_core.h. Kept minimal and in
// declaration order so a diff against that file is easy to eyeball.
extern "C" {
void   logos_core_init(int argc, char* argv[]);
void   logos_core_add_modules_dir(const char* modules_dir);
void   logos_core_start();
void   logos_core_cleanup();
char** logos_core_get_loaded_modules();
char** logos_core_get_known_modules();
int    logos_core_load_module(const char* module_name, bool with_dependencies);
int    logos_core_unload_module(const char* module_name, bool with_dependents);
char** logos_core_get_module_dependencies(const char* module_name, bool recursive);
char** logos_core_get_module_dependents(const char* module_name, bool recursive);
char*  logos_core_get_modules_info();
char*  logos_core_process_module(const char* module_path);
char*  logos_core_get_token(const char* key);
char*  logos_core_get_module_stats();
void   logos_core_set_persistence_base_path(const char* path);
void   logos_core_set_module_transports(const char* module_name,
                                        const char* transport_set_json);
void   logos_core_set_access_policy(const char* policy_json);
void   logos_core_refresh_modules();
}

namespace logos {
namespace host {

// NOTE: there is deliberately no "called out of order" exception type here.
// Every pre-start setting is a constructor argument, so applying one after
// start() is not something a caller can express — the ordering constraint is
// enforced by the shape of the type rather than by a runtime check.

// One loaded module's resource usage, indexed out of the single blob that
// logos_core_get_module_stats() returns for ALL modules.
struct ModuleStats {
    std::string name;
    double      cpuPercent = 0.0;
    long long   memoryBytes = 0;
    // The raw entry, so a host can read fields this struct does not model
    // without waiting for the SDK to grow them.
    nlohmann::json raw;
};

namespace detail {

// liblogos builds these arrays with `new char*[]` and each element with
// `new char[]` (module_manager.cpp::toNullTerminatedArray). `delete[]` is the
// correct deallocator for both; `free()` is undefined behaviour. This is the
// single most-copied piece of knowledge in the host repos, so it lives here.
inline std::vector<std::string> drainCStringArray(char** arr)
{
    std::vector<std::string> out;
    if (!arr) return out;
    for (char** p = arr; *p != nullptr; ++p) {
        out.emplace_back(*p);
        delete[] *p;
    }
    delete[] arr;
    return out;
}

// Same allocator, single string. Returns nullopt for a NULL return, which the
// C API uses to mean "no value / error" — distinct from an empty string.
inline std::optional<std::string> drainCString(char* s)
{
    if (!s) return std::nullopt;
    std::optional<std::string> out(std::string{s});
    delete[] s;
    return out;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// LogosCore — owns the process-wide Logos core.
//
// Construct exactly ONE, in main(), and keep it alive for the process. The
// underlying C API is process-global, so this type is neither copyable nor
// movable: two instances would mean two owners of one core, and the second
// destructor would call logos_core_cleanup() on an already-cleaned core.
//
//     logos::host::LogosCore::Config cfg;
//     cfg.modulesDirs = { "/usr/lib/logos/modules" };
//     cfg.persistenceBasePath = "/var/lib/logos";
//
//     logos::host::LogosCore core(argc, argv, std::move(cfg));
//     core.start();
//     core.loadModule("package_manager");
// ─────────────────────────────────────────────────────────────────────────────
class LogosCore {
public:
    // Everything the C API requires BEFORE logos_core_start(). Passing these
    // through the constructor is the point of the type: it makes the illegal
    // ordering unrepresentable rather than documented.
    struct Config {
        // Applied in order, via logos_core_add_modules_dir.
        std::vector<std::string> modulesDirs;

        // Empty ⇒ not set. Each module gets {path}/{module_name}/{instance_id}/.
        std::string persistenceBasePath;

        // nullopt ⇒ install no policy at all, which is NOT the same as an empty
        // policy: liblogos treats "no policy" as unrestricted and only enforces
        // when a policy with mode "enforce" is present.
        std::optional<std::string> accessPolicyJson;

        // module name → JSON array of LogosTransportConfig. Registered before
        // start(), which is what capability_module requires; user modules only
        // need it before their own load, but doing it here covers both.
        std::map<std::string, std::string> moduleTransports;
    };

    LogosCore(int argc, char* argv[], Config config)
    {
        logos_core_init(argc, argv);
        // Ordered exactly as liblogos documents: dirs, then persistence, then
        // transports, then policy — all strictly before start().
        for (const std::string& dir : config.modulesDirs)
            logos_core_add_modules_dir(dir.c_str());
        if (!config.persistenceBasePath.empty())
            logos_core_set_persistence_base_path(config.persistenceBasePath.c_str());
        for (const auto& entry : config.moduleTransports)
            logos_core_set_module_transports(entry.first.c_str(), entry.second.c_str());
        if (config.accessPolicyJson.has_value())
            logos_core_set_access_policy(config.accessPolicyJson->c_str());
    }

    ~LogosCore() { logos_core_cleanup(); }

    LogosCore(const LogosCore&) = delete;
    LogosCore& operator=(const LogosCore&) = delete;
    LogosCore(LogosCore&&) = delete;
    LogosCore& operator=(LogosCore&&) = delete;

    // Boots the core and spawns the modules liblogos starts itself (notably
    // capability_module). After this, the pre-start settings above can no
    // longer be changed.
    void start()
    {
        logos_core_start();
        m_started = true;
    }

    bool isStarted() const { return m_started; }

    // ── Module lifecycle ────────────────────────────────────────────────────

    // Returns true on success. `withDependencies` resolves and loads the
    // module's declared dependency graph first, which is what a host almost
    // always wants — hence the default.
    bool loadModule(const std::string& name, bool withDependencies = true)
    {
        return logos_core_load_module(name.c_str(), withDependencies) == 1;
    }

    // Returns true on success. `withDependents` cascades to modules that depend
    // on this one; without it, unloading a module something else is using
    // fails rather than breaking the dependent.
    bool unloadModule(const std::string& name, bool withDependents = false)
    {
        return logos_core_unload_module(name.c_str(), withDependents) == 1;
    }

    // Re-scans the modules directories for changes on disk.
    void refreshModules() { logos_core_refresh_modules(); }

    // Registers a module file with the core, returning whatever liblogos
    // reports about it (nullopt on error).
    std::optional<std::string> processModule(const std::string& modulePath)
    {
        return detail::drainCString(logos_core_process_module(modulePath.c_str()));
    }

    // ── Introspection ───────────────────────────────────────────────────────

    std::vector<std::string> knownModules() const
    {
        return detail::drainCStringArray(logos_core_get_known_modules());
    }

    std::vector<std::string> loadedModules() const
    {
        return detail::drainCStringArray(logos_core_get_loaded_modules());
    }

    std::vector<std::string> dependencies(const std::string& name, bool recursive = false) const
    {
        return detail::drainCStringArray(
            logos_core_get_module_dependencies(name.c_str(), recursive));
    }

    std::vector<std::string> dependents(const std::string& name, bool recursive = false) const
    {
        return detail::drainCStringArray(
            logos_core_get_module_dependents(name.c_str(), recursive));
    }

    // Full metadata for every known module, as liblogos' JSON.
    std::optional<std::string> modulesInfoJson() const
    {
        return detail::drainCString(logos_core_get_modules_info());
    }

    // A bootstrap token from the core's store. nullopt when the key is absent.
    std::optional<std::string> token(const std::string& key) const
    {
        return detail::drainCString(logos_core_get_token(key.c_str()));
    }

    // ── Stats ───────────────────────────────────────────────────────────────
    //
    // The C call takes no module name: it returns ONE JSON array covering every
    // loaded module. Both accessors below share that single call, so asking for
    // one module's stats costs the same as asking for all of them — do not loop
    // over `stats(name)` for a whole list, call `allStats()` once.

    std::vector<ModuleStats> allStats() const
    {
        std::vector<ModuleStats> out;
        const std::optional<std::string> blob =
            detail::drainCString(logos_core_get_module_stats());
        if (!blob.has_value()) return out;

        const nlohmann::json parsed =
            nlohmann::json::parse(*blob, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_array()) return out;

        for (const nlohmann::json& entry : parsed) {
            if (!entry.is_object()) continue;
            ModuleStats s;
            s.name = entry.value("name", std::string{});
            s.cpuPercent = entry.value("cpu", 0.0);
            s.memoryBytes = entry.value("memory", 0LL);
            s.raw = entry;
            out.push_back(std::move(s));
        }
        return out;
    }

    // nullopt when the module is not loaded (and therefore has no entry).
    std::optional<ModuleStats> stats(const std::string& moduleName) const
    {
        std::vector<ModuleStats> all = allStats();
        for (ModuleStats& s : all) {
            if (s.name == moduleName) return std::move(s);
        }
        return std::nullopt;
    }

private:
    bool m_started = false;
};

} // namespace host
} // namespace logos
