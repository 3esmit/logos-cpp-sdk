> **Layering note:** since the protocol extraction and the Qt split, this
> repo is the **Qt-free base SDK** — header-only developer surface for
> universal module implementations (`logos_module_context.h`,
> `logos_result.h`, `logos_json.h`) plus the `logos-cpp-generator` code
> generator (Qt-free outputs: std typed wrappers, the `logos_sdk` umbrella,
> cdylib C-ABI impl-exports, LIDL derivation). Generated **Qt** glue comes
> from two other binaries: the Qt-typed *consumer* wrappers and the `ui_qml`
> view-plugin glue from logos-qt-sdk's `logos-qt-generator`
> (`--backend consumer` / `--backend ui`), and the Qt-*plugin* (provider)
> glue from logos-plugin-qt's `logos-qt-host-generator --backend cdylib`.
> Transports, the consumer core, the Qt `LogosResult` and the `lp_*` C ABI
> live in [logos-protocol](https://github.com/logos-co/logos-protocol); the
> Qt developer layer is published as
> [logos-qt-sdk](https://github.com/logos-co/logos-qt-sdk)'s CMake package
> (`logos-qt-sdk::logos_qt_sdk`), but the code behind it — `LogosAPI`,
> `LogosProviderBase`, the QObject adapter — now lives in
> [logos-plugin-qt](https://github.com/logos-co/logos-plugin-qt)'s
> `logos-qt-host`.

# logos-cpp-sdk

## How to Build

### Using Nix (Recommended)

The project includes a Nix flake for reproducible builds with a modular structure:

#### Build Complete SDK (Library + Headers + Generator)

```bash
# Build everything (default)
nix build

# Or explicitly
nix build '.#logos-cpp-sdk'
nix build '.#default'
```

The result will include:
- `/bin/logos-cpp-generator` - Code generator binary
- `/lib/cmake/logos-cpp-sdk/` - The CMake package (`find_package(logos-cpp-sdk)`).
  There is no compiled library here: the base SDK is **header-only** since the
  transports moved to logos-protocol
- `/include/` and `/include/cpp/` - The same headers in both roots (the
  CMake-export layout and the source-export layout). A quoted include has to
  resolve its siblings from whichever root pulled it in, so both are shipped
- `/share/lidl-frontend/` - The shared C++/Qt codegen helpers logos-qt-sdk's
  `logos-qt-generator` compiles against

#### Build Individual Components

```bash
# Build only the generator binary (outputs to /bin)
nix build '.#logos-cpp-bin'

# Build only the CMake package (header-only: /include + /lib/cmake, no archive)
nix build '.#logos-cpp-lib'

# Build only the headers, in the source-export layout (/include and /include/cpp)
nix build '.#logos-cpp-include'

# Legacy alias for generator
nix build '.#cpp-generator'
```

#### Development Shell

```bash
# Enter development shell with all dependencies
nix develop
```

**Note:** In zsh, you need to quote the target (e.g., `'.#logos-cpp-sdk'`) to prevent glob expansion.

If you don't have flakes enabled globally, add experimental flags:

```bash
nix build '.#logos-cpp-sdk' --extra-experimental-features 'nix-command flakes'
```

The compiled artifacts can be found at `result/`

#### Modular Architecture

The nix build system is organized into modular files in the `/nix` directory:
- `nix/default.nix` - Common configuration (dependencies, flags, metadata)
- `nix/bin.nix` - Generator binary compilation
- `nix/lib.nix` - Header-only SDK: installs the headers + the CMake package
- `nix/include.nix` - Header installation (source-export layout)
- `nix/tests.nix` - gtest suite (build + run via `nix build '.#tests'`)
- `nix/tests-generator-cli.nix` - The `generator-cli` check: runs the built
  binary, which is the only place a retired CLI flag can be asserted on (the
  gtest suite links the generator's internals and never executes it)

#### Run Tests

```bash
# Build and run all tests (build fails if any test fails)
nix build '.#tests'

# Run the built binary against its retired/renamed CLI flags
nix build '.#checks.<system>.generator-cli'
```

The three test binaries are available in `result/bin/` and can be re-run with
filters:

```bash
./result/bin/sdk_tests --gtest_filter="LogosModuleContextTest.*"
./result/bin/generator_tests --gtest_filter="*PascalCase*"
./result/bin/experimental_tests --gtest_filter="*Cdylib*"
```

### Manual Build

#### Building the Code Generator

```bash
cd cpp-generator
./compile.sh
```

`compile.sh` builds into `<parent-of-this-repo>/build/cpp-generator`, so the
binary lands at `../../build/cpp-generator/bin/logos-cpp-generator` relative to
this checkout (it assumes the checkout directory is named `logos-cpp-sdk`).

CMake must be able to resolve two out-of-tree dependencies for this to work:
`find_package(logos-lidl)` — the canonical LIDL frontend the generator links —
and the logos-protocol headers, via `-DLOGOS_PROTOCOL_ROOT=` / the
`LOGOS_PROTOCOL_ROOT` environment variable / a sibling `../../logos-protocol`
checkout. The Nix build (above) wires both for you.

## Usage

### Code Generator

The `logos-cpp-generator` tool generates C++ wrapper code for Logos plugins.

#### Basic Usage

```bash
# Generate the wrapper for a single BUILT plugin (uses default output directory).
# This path loads the plugin and reads its Qt metaobject / getMethods(), so it
# only works for a plugin built for the machine running the generator.
logos-cpp-generator /path/to/plugin.dylib

# Specify custom output directory
logos-cpp-generator /path/to/plugin.dylib --output-dir /custom/output/path

# `--module-only` is accepted here but is a no-op: this path only ever emits the
# module pair. It is kept because existing callers still pass it.
logos-cpp-generator /path/to/plugin.dylib --output-dir /custom/output --module-only
```

#### Generate from Metadata

```bash
# List dependencies from metadata.json
logos-cpp-generator --metadata /path/to/metadata.json

# Generate a wrapper per dependency, each from that dependency's LIDL contract
logos-cpp-generator --metadata /path/to/metadata.json --umbrella \
  --dep waku_module=/path/to/waku_module.lidl

# Generate only the umbrella (assumes the module wrapper files already exist)
logos-cpp-generator --metadata /path/to/metadata.json --umbrella

# Generate the umbrella into a custom output directory
logos-cpp-generator --metadata /path/to/metadata.json --umbrella --output-dir /custom/output
```

`--general-only` is an exact alias for `--umbrella` (it is the spelling
`LogosModule.cmake`, `buildPlugin.nix` and `buildHeaders.nix` all pass today),
so the two run the same single implementation.

#### Options

**`--output-dir /path/to/output`**
- **Default:** If not specified, generated files are placed in `logos-cpp-sdk/cpp/generated/`
- **Custom:** Specify any directory for the generated files
- The output directory will be created automatically if it doesn't exist

**`--module-only`**
- On the **plugin** path (`logos-cpp-generator /path/to/plugin.dylib`) it is
  accepted and **ignored** — that path only ever emits the requested module's
  `<name>_api.h` / `<name>_api.cpp` pair. `generate-module-headers.sh` always
  passes the flag, so it stays tolerated rather than rejected
- On the `--lidl` client-stub path it is honoured: it suppresses the umbrella
  (`logos_sdk.*`) and emits only the module pair

**`--umbrella`** (alias: **`--general-only`**)
- When specified with `--metadata`, generates only the umbrella SDK files
- Assumes module wrapper files already exist in the output directory
- Generates: `logos_sdk.h`, `logos_sdk.cpp`. There is **no** `core_manager_api.*`
  — the runtime's core manager was never a `LogosModules` member, and the
  generator emits no wrapper for it; apps that need to manage the core use
  liblogos' C API
- The umbrella headers will include references to all modules listed in the metadata's `dependencies` array
- For each dependency (e.g., `"waku_module"`), it will:
  - Include `waku_module_api.h` in the header
  - Include `waku_module_api.cpp` in the source
  - Create a `WakuModule waku_module;` member in the `LogosModules` struct
- Takes one `--dep <name>=<path/to/<name>.lidl>` per dependency and generates that
  dependency's wrapper from its contract, so no dependency plugin has to be built
  (and it works under cross-compilation)
- `--interface <name>=<file.lidl|file.h>[=<ImplClass>]` does the same for an
  interface dependency, which additionally gets a `bind_<name>(provider)` factory
- `--api-style qt|lp` picks the type surface (see *API style* below);
  `--binding api|origin` picks whether the umbrella holds a `LogosAPI` or states
  this module's own name as the call origin

**`--provider-header`** — REMOVED
- Generated the `LOGOS_METHOD`-marked provider dispatch behind
  `interface: "provider"`. Both are gone: every provider now goes through the
  module-impl C ABI
- The generator refuses the flag with a message naming `interface: "universal"`,
  where a plain `src/<name>_impl.h` is the contract

**`--module-dir /path/to/modules`** — REMOVED
- Generated a wrapper per dependency by loading each dependency's BUILT plugin
  from a modules directory and reading its Qt metaobject
- The generator now refuses the flag rather than ignoring it; use `--umbrella`
  with `--dep` as above

#### Generated Files

**Plugin path (`logos-cpp-generator /path/to/plugin.dylib`), with or without `--module-only`:**
- `<module>_api.h` and `<module>_api.cpp` — the wrapper for that one plugin, and
  nothing else

**With `--umbrella` / `--general-only`:**
- `logos_sdk.h` and `logos_sdk.cpp` — the umbrella that aggregates the wrappers
- Plus one `<name>_api.{h,cpp}` pair per `--dep` / `--interface` spec passed

**With `--lidl <contract> --backend cdylib --impl-class <C>`:**
- `<name>_types.h`, `<name>_module_impl.cpp`, and — when the contract declares
  events — `<name>_events_cdylib.cpp`

**With `--from-header <impl.h> --backend cdylib`:** the same three, plus the
derived `<name>.lidl`. `--header-to-lidl` emits only the `.lidl`.

#### Typical Workflow

A common workflow is to generate module wrappers separately, then generate the umbrella SDK:

```bash
# Step 1: Generate individual module wrappers
logos-cpp-generator /path/to/plugin1.dylib --output-dir ./generated
logos-cpp-generator /path/to/plugin2.dylib --output-dir ./generated

# Step 2: Generate the umbrella SDK (references the modules from step 1)
logos-cpp-generator --metadata metadata.json --umbrella --output-dir ./generated
```

This approach gives you fine-grained control over which modules to include and allows rebuilding just the umbrella headers without regenerating all module wrappers.

#### The three call surfaces on a generated wrapper

Every LIDL `method foo(...) -> T` produces three entry points:

```cpp
// 1. sync — optional error out-channel, optional deadline. Both trailing and
//    defaulted, so `dep.foo(a, b)` and `dep.foo(a, b, &err)` are unchanged.
T    foo(params…, logos::CallError* err = nullptr, Timeout timeout = Timeout());

// 2. async, value only — the historical form, unchanged.
void fooAsync(params…, std::function<void(T)> cb, Timeout timeout = Timeout());

// 3. async, value + error.
void fooAsyncResult(params…, std::function<void(logos::AsyncResult<T>)> cb,
                    Timeout timeout = Timeout());
```

Use (3) whenever a default-constructed `T` is also a legal success value — which
is almost always. `fooAsync` hands the callback a bare `T`, so a failed call and
a provider that genuinely returned `0` / `""` / `false` are indistinguishable;
that is exactly the ambiguity the sync form's `CallError*` exists to resolve.

```cpp
dep.balanceAsyncResult(account, [](logos::AsyncResult<qlonglong> r) {
    if (!r.ok()) {                       // r.error is {code, message, origin}
        qWarning() << "balance failed:" << r.error.code.c_str();
        return;
    }
    use(r.value);                        // now known to be a real answer
});
```

`logos::AsyncResult<T>` (`logos_async_result.h`) is `{ T value; CallError error; }`
plus `ok()`; `AsyncResult<void>` carries only the error, so a `void`-returning
method has the same callback shape as every other one.

The name is deliberately distinct rather than an overload of `fooAsync`: two
overloads differing only in `std::function<void(T)>` vs
`std::function<void(AsyncResult<T>)>` are ambiguous for a generic lambda
(`[](auto v){…}`), which would break existing call sites.

**Qt-free (`--api-style lp`) wrappers** spell the deadline `int timeout_ms = 0`
(`<= 0` selects the protocol default) because `Timeout` lives in a Qt header,
and they do **not** yet get `fooAsyncResult` — logos-protocol's
`lp_invoke_async` does not report the call error to its callback, so an
`AsyncResult` there would report success on a failed call.

### Universal modules: LogosModuleContext

Universal (codegen-driven) modules — those built from a plain `src/<name>_impl.h` header rather than a handcrafted `QObject` plugin — don't see the raw `LogosAPI` at all. The contract is **derived from that header**: the module's ordinary public methods *are* its API, with no marker of any kind (there used to be a `LOGOS_METHOD` marker under `interface: "provider"`; both are gone). `metadata.json#codegen.impl_class` / `codegen.impl_header` name the class and the header when they differ from the defaults (`<Name>Impl` in `src/<name>_impl.h`). Instead of a `LogosAPI`, the generated C-ABI export TU (`<name>_module_impl.cpp`) populates a narrow `LogosModuleContext` base class with everything an impl typically needs:

- Three host-injected properties exposed as typed getters
- A `LogosModules` aggregate for calling other modules

An impl opts in by inheriting from `LogosModuleContext` (defined in `logos_module_context.h`):

```cpp
#include <logos_module_context.h>
#include <logos_json.h>
#include "logos_sdk.h"      // generated at build time; defines LogosModules

class MyModuleImpl : public LogosModuleContext {
public:
    LogosMap doWork(const std::string& input) {
        // Cross-module call through the flat LogosModules aggregator.
        // Because this module is `interface: "universal"`, mkLogosModule.nix
        // passed -DLOGOS_API_STYLE=lp to the codegen, so every <Dep>
        // wrapper takes/returns std types — no Qt at the call site.
        std::string reply = modules().some_dep.echo(input);
        // ...
    }

protected:
    void onContextReady() override {
        // One-time setup: the getters below are now readable.
        // Fires exactly once, before any method dispatch.
        std::string dataDir = instancePersistencePath();
        // open files, prime caches, etc.
    }
};
```

**Documenting methods:** a doc comment (`///` or `/** … */`) directly above a
method declaration becomes that method's `description` in the generated
`getMethods()` output, so it surfaces in `lm methods`, `logoscore module-info`,
and Basecamp's Methods list — no `describe` call needed:

```cpp
/// Processes the input and returns a result map.
LogosMap doWork(const std::string& input);
```

Plain `//` and `/* … */` comments are ignored (so section separators don't leak
into the API). See `cpp-generator/docs/spec.md` → *Method documentation* for
details.

**Documenting events:** events are the other half of a module's API — declared
in a `logos_events:` section and surfaced the same way. A doc comment above an
event declaration becomes that event's `description`. Events are reported
*inside* the generated `getMethods()` output (each entry tagged `type: "event"`,
methods tagged `type: "method"`); the framework exposes filtered views —
`getPluginMethods()`, `getPluginEvents()`, `getPluginInterface()` — so the event
surfaces in `lm events`, `logoscore module-info`'s Events section, and Basecamp's
Interface screen:

```cpp
logos_events:
    /// Emitted once the user has authenticated.
    /// Carries the freshly issued session token.
    void userLoggedIn(const std::string& userId, const std::string& token);
```

Event entries carry `type: "event"`, `name`, `signature`, `parameters[]`, and
`description` (no `returnType` — events are fire-and-forget). Folding events into
`getMethods()` rather than adding a `getEvents()` vtable method keeps the
provider ABI stable across SDK versions. See `cpp-generator/docs/spec.md` →
*Event documentation* for details.

Available getters:

| Getter | Description |
|---|---|
| `modulePath()` | Directory containing the module's plugin file. Useful for loading bundled resources (icons, QML files, schema docs). |
| `moduleName()` | This module's own registry name — the name other modules address it by, and the `origin` it authenticates as. The typed wrappers bake their origin in at codegen time; a by-name call has to state it. |
| `instanceId()` | Stable per-instance ID assigned by the host. Two side-by-side instances of the same module get distinct IDs. |
| `instancePersistencePath()` | Per-instance writable data directory the host owns the lifecycle of. The canonical place for module state (config, caches, small databases). Wiped on uninstall; survives upgrades. |
| `isContextReady()` | True once the framework has populated the getters above. Flipped *before* `onContextReady()` fires, so helpers that may run earlier (e.g. during construction in tests that bypass the framework) can guard on it. |
| `modules()` | The module's flat `LogosModules` aggregate — one accessor per `metadata.json#dependencies` entry, plus a `bind_<name>(provider)` factory per interface dependency and — on the `lp` surface universal modules get — an untyped `dynamic(target)` escape hatch returning a `logos::LpClient` (the runtime's core manager is deliberately not there; apps that need to manage the core do so via liblogos' C API). `LogosModules` is forward-declared in the SDK header and made complete by the impl's `#include "logos_sdk.h"`, so the call site just writes `modules().some_dep.someMethod(...)`. Each accessor's wrapper class signatures use the type surface picked at THIS module's build time (see "API style" below). |

#### API style: Qt vs std

Each module's build picks **one** API style for the generated `<Module>` client wrappers and the `LogosModules` umbrella — they're mutually exclusive, no composite output:

| `metadata.json#interface` | `LOGOS_API_STYLE` | Wrapper signatures |
|---|---|---|
| `"cdylib"`, or `"universal"` with `type` other than `ui_qml` | `lp` | `std::string`, `std::vector<std::string>`, `LogosMap`, `LogosList`, `int64_t`, `StdLogosResult` |
| `"legacy"` / absent, and `"universal"` with `type: "ui_qml"` | `qt` (default) | `QString`, `QStringList`, `QVariantList`, `QVariantMap`, `qlonglong`/`qulonglong`, `LogosResult` |

The valid `interface` values are `"legacy"` (the default when the key is
absent), `"universal"` and `"cdylib"`. A fourth, `"provider"` — the
`LOGOS_METHOD`-marked Qt provider — was removed; `logos-module-builder` now
throws on it rather than silently generating no glue.

A third value, `std`, used to name a std-typed surface whose body still went
through `QVariant` + `LogosAPIClient`. It was retired once universal modules
moved to `lp`; `--api-style=std` is now rejected outright rather than aliased,
so a stale build fails loudly instead of silently getting Qt signatures.

`mkLogosModule.nix` reads `interface` and threads `-DLOGOS_API_STYLE=lp` through to the codegen for universal modules; everyone else defaults to Qt and stays bit-for-bit backward compatible. Inside the universal module's `.cpp`, the call site is:

```cpp
// Universal module (api-style=lp):
std::string reply = modules().some_dep.echo("hi");
```

…and in a handcrafted Qt module the same call is:

```cpp
// Legacy module, or a universal ui_qml view plugin (api-style=qt):
QString reply = modules().some_dep.echo(QString("hi"));
```

The two carry the same values; the `lp` wrapper marshals them over the logos-protocol C ABI (`lp_*`) instead of `QVariant`, so the calling translation unit needs zero Qt headers and links no qt-sdk.

> **Migrating to std types**: The default is derived from `interface` (plus
> `type`, per the table above). A handcrafted module that wants std types should
> switch to `interface: "universal"`. There is one override key —
> `metadata.json#codegen.consumer_api_style` — and only one direction of it is
> reachable: a module packaged as a cdylib may ask for `"qt"` (Qt-typed,
> origin-bound wrappers). A Qt-plugin module asking for `"lp"` is refused,
> because nothing would populate the token store the lp wrappers read, and every
> outbound call would come back as a default value with no error raised.

All getters return empty / null values when the module is loaded outside a host that provisions a context (CLI tests, unit tests using the impl directly). The `onContextReady()` hook still fires once at framework load time; tests that bypass the framework can call `_logosCoreSetContext_` / `_logosCoreSetLogosModulesPtr_` directly to simulate.

Codegen does NOT require inheritance — modules that don't inherit `LogosModuleContext` compile unchanged. The generated export TU routes every wire-up through SFINAE'd helpers (`_logos_codegen_::maybeSetModuleName` / `maybeSetContext` / `maybeSetLogosModules` / `maybeSetEmitEvent`), called from a one-shot latch that the first `logos_module_dispatch` / `logos_module_set_context` / `logos_module_set_emit_callback` trips; the non-inheriting overloads collapse to no-ops.

#### Events: `logos_events:`

Universal modules declare events in a Qt-`signals:`-style `logos_events:` section. The codegen parses each prototype, emits the matching method bodies in a sidecar `<name>_events_cdylib.cpp` (Qt-MOC style), and ships a `<name>.lidl` file describing them so consumer-side codegen can produce typed subscribers:

```cpp
#include <logos_module_context.h>

class MyModuleImpl : public LogosModuleContext {
public:
    void doWork() {
        userLoggedIn("alice", 12345);              // typed emit — same name as the declaration
    }

logos_events:                                       // expands to `public:`; parsed by the codegen
    void userLoggedIn(const std::string& userId, int64_t timestamp);
    void messageReceived(const std::string& from, const std::string& body);
};
```

The author writes only the declarations; the codegen supplies the bodies (analogous to Qt MOC for `signals:`). Each call marshals typed args into an `nlohmann::json` array and routes them through `LogosModuleContext::emitEventImpl_` → the `logos_module_emit_cb` the host installed via `logos_module_set_emit_callback` → the host's own event channel. (The marshalling used to be into a `QVariantList` handed to `LogosProviderBase::emitEvent`; that path belonged to the Qt provider glue, which a universal module no longer has — its whole impl side is Qt-free.) No wire-format change.

**Consumer side** — typed `on<EventName>(...)` accessors are generated on the dep's `<Module>` wrapper. On the **Qt** surface a generic `on(eventName, callback)` channel sits alongside them as a forward-compat escape hatch; the **lp** surface has only the typed accessors (reach for `logos::LpClient::subscribe` directly if you need an untyped one):

```cpp
// From any module that depends on the one declaring the events:
modules().my_module.onUserLoggedIn(
    [](const std::string& userId, int64_t timestamp) {
        // typed args, no manual QVariantList unpacking
    });
```

The accessor's parameter types follow the consumer's own `--api-style` (so a `universal` consumer sees `const std::string&` / `int64_t`, a handcrafted Qt consumer sees `const QString&` / `qlonglong`).

### API

#### LogosResult

> **Where it lives:** the Qt `LogosResult` shown below is **not** in this repo —
> it is declared in logos-protocol's `cpp/logos_types.h`, along with
> `LogosResultException`. What this repo's `logos_result.h` ships is the Qt-free
> `StdLogosResult` (`{ bool success; nlohmann::json value; std::string error; }`),
> which is what a universal module returns; the generated glue converts it to the
> Qt `LogosResult` for Qt callers. The section below describes the Qt-typed
> consumer surface.

`LogosResult` provides a structured way to return either a value or an error from synchronous method calls.

If the `success` attribute is `true`, you can retrieve the value using a cast. Otherwise, retrieve the error which should be a string (though not enforced).

The `success` attribute should ALWAYS be asserted. Accessing the value of an errored `LogosResult` or the error of a valid `LogosResult` will result in a `LogosResultException` being thrown.

### Example

```cpp
LogosResult result = m_logos->my_module.someMethod();
if (result.success) {
    // Use shorthand
    QString value = result.getString();
    // Or
    QString value = result.getValue<QString>();
} else {
    // Use shorthand
    QString error = result.getError();
    // Or
    QString error = result.getError<QString>();
}
```

#### Complex objects

Let's say you need to return a complex object. In the SDK, you have to build your type with primitive like QVariantMap:

```cpp
// Received JSON: {"cid": "QmXyz...", "filename": "photo.jpg", "size": 2048576, "mimetype": "image/jpeg"}

QVariantMap manifest;
manifest["cid"] = "QmXyz...";
manifest["filename"] = "photo.jpg";
manifest["size"] = 2048576;
manifest["mimetype"] = "image/jpeg";
return {true, manifest};
```

And then to consume by using the shorthand function:

```cpp
LogosResult result = m_logos->my_plugin.someMethod(cid);
if (result.success) {
  QString cid = result.getString("cid");
  // You can define a default value as well
  QString cid = result.getString("cid", "unknown");
}
```

Or you can use the value directly:

```cpp
LogosResult result = m_logos->my_plugin.someMethod(cid);
if (result.success) {
    QVariantMap manifest = result.getMap();
    QString cid = manifest["cid"].toString();
}
```

Same thing for a list, you can use `QVariantList`:

```cpp
QVariantList manifests;

QVariantMap m1;
m1["cid"] = "QmAbc...";
m1["filename"] = "document.pdf";
m1["size"] = 1024000;
manifests.append(m1);

QVariantMap m2;
m2["cid"] = "QmDef...";
m2["filename"] = "image.png";
m2["size"] = 512000;
manifests.append(m2);

return {true, manifests};
```

To consume it using the shorthand function:

```cpp
LogosResult result = m_logos->my_plugin.someMethod();
if (result.success) {
    for (int i = 0; i < list.size(); ++i) {
        QString cid = result.getString(i, "cid");
        // You can define a default value as well
        QString cid = result.getString(0, "cid", "unknown");
    }
}
```

Or you can use the value directly:

```cpp
LogosResult result = m_logos->my_plugin.someMethod();
if (result.success) {
    QVariantList list = result.getList();
    for (const QVariant& item : list) {
        QVariantMap manifest = item.toMap();
        QString cid = manifest["cid"].toString();
    }
}
```

### Consuming the SDK

The SDK installs a CMake package. Consumers use `find_package`:

```cmake
find_package(logos-cpp-sdk REQUIRED)
target_link_libraries(my_target PRIVATE logos-cpp-sdk::logos_headers)
```

Every target is an `INTERFACE` library — the base SDK is header-only, so there is
no archive to link and nothing to resolve beyond `nlohmann_json`, which the
package config pulls in with `find_dependency`. (It used to also re-resolve
`Qt6 Core/RemoteObjects`, `Boost system` and `OpenSSL` for a static archive that
referenced them; the transports that needed those moved to logos-protocol.)

`logos_headers` is the umbrella over four narrower targets, split by what a
program actually is — take the narrow one when touching a repo:

| Target | Headers | For |
|---|---|---|
| `logos-cpp-sdk::logos_common` | `logos_json.h`, `logos_result.h` | The shared value types; everything below links it |
| `logos-cpp-sdk::logos_consumer` | `logos_lp_client.h`, `logos_async_result.h` | CALLING other modules — also where the generated `<dep>_api.{h,cpp}` and `logos_sdk.h` compile |
| `logos-cpp-sdk::logos_provider` | `logos_module_context.h`, `logos_host_services.h` | IMPLEMENTING a module |
| `logos-cpp-sdk::logos_host` | `logos_host_core.h` | STANDING UP a core and loading modules (basecamp, logoscore-cli, standalone-app, module-viewer). A module never needs this |

### Transports

> **Where they live:** none of the types in this section are in this repo any
> more. `LogosTransportConfig` / `LogosTransportSet` /
> `LogosTransportConfigGlobal` / `LogosProtocol` are declared in logos-protocol
> (`cpp/logos_transport_config.h`), and `LogosAPI` — which consumes them — in
> logos-plugin-qt's `logos-qt-host` (published through the `logos-qt-sdk` CMake
> package). The section is kept here because it is the shape a Qt host still
> writes.

The runtime supports multiple transports, selected via `LogosTransportConfig`:

| Protocol | Backend | Use case |
|----------|---------|----------|
| `LocalSocket` | Qt Remote Objects over `QLocalSocket` | In-host, module-to-module (default) |
| `Tcp` | Boost.Asio + JSON/CBOR framing | Cross-host or container-to-host |
| `TcpSsl` | Boost.Asio + OpenSSL + JSON/CBOR framing | Same as TCP, with TLS |

A `LogosTransportSet` (= `std::vector<LogosTransportConfig>`) lets a single provider publish on multiple endpoints simultaneously (e.g. local socket for in-process clients + TCP+SSL for remote ones):

```cpp
LogosTransportConfig local;  // protocol = LocalSocket (default)

LogosTransportConfig tls;
tls.protocol = LogosProtocol::TcpSsl;
tls.host     = "0.0.0.0";
tls.port     = 7443;
tls.caFile   = "/etc/logos/ca.pem";
tls.certFile = "/etc/logos/server.pem";
tls.keyFile  = "/etc/logos/server.key";

LogosAPI* api = new LogosAPI("core_service", LogosTransportSet{local, tls}, this);
```

For processes that want to override the process-wide default, use `LogosTransportConfigGlobal::setDefault()` once at startup before any `LogosAPI` is constructed.

### Requirements

These are what building **this repo** needs. A consumer of the installed SDK
needs only `nlohmann_json` — see *Consuming the SDK* above.

#### Build Tools
- CMake (3.14 or later)
- Ninja build system
- pkg-config

#### Dependencies
- logos-lidl — the canonical LIDL frontend; the generator links it via
  `find_package(logos-lidl)` rather than embedding a copy
- logos-protocol — headers only, located via `LOGOS_PROTOCOL_ROOT`
- Qt6 (qtbase) — the generator itself is a Qt Core program (`QCoreApplication`,
  `QPluginLoader`, `QJson*`)
- Qt6 Remote Objects (qtremoteobjects)
- Boost (system)
- OpenSSL
- nlohmann_json

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
