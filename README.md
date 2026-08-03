> **Layering note:** since the protocol extraction and the Qt split, this
> repo is the **Qt-free base SDK** — header-only developer surface for
> universal module implementations (`logos_module_context.h`,
> `logos_result.h`, `logos_json.h`) plus the `logos-cpp-generator` code
> generator (Qt-free outputs: std typed wrappers, the `logos_sdk` umbrella,
> cdylib C-ABI impl-exports, LIDL derivation — ALL generated Qt glue comes
> from logos-qt-sdk's `logos-qt-generator`). Transports, the consumer core and the `lp_*` C ABI live in
> [logos-protocol](https://github.com/logos-co/logos-protocol); the Qt
> developer layer (LogosAPI, provider base classes, QObject glue) lives in
> [logos-qt-sdk](https://github.com/logos-co/logos-qt-sdk).

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
- `/lib/` - SDK libraries
- `/include/` - Headers (core/ and cpp/)

#### Build Individual Components

```bash
# Build only the generator binary (outputs to /bin)
nix build '.#logos-cpp-bin'

# Build only the SDK library (outputs to /lib)
nix build '.#logos-cpp-lib'

# Build only the headers (outputs to /include)
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
- `nix/lib.nix` - SDK library compilation
- `nix/include.nix` - Header installation
- `nix/tests.nix` - Test suite (build + run via `nix build '.#tests'`)

#### Run Tests

```bash
# Build and run all tests (build fails if any test fails)
nix build '.#tests'
```

The test binaries are available in `result/bin/` and can be re-run with filters:

```bash
./result/bin/sdk_tests --gtest_filter="LogosResultTest.*"
./result/bin/generator_tests --gtest_filter="*PascalCase*"
```

### Manual Build

#### Building the C++ SDK

```bash
cd cpp
./compile.sh
```

#### Building the Code Generator

```bash
cd cpp-generator
./compile.sh
```

The generator binary will be available at `build/bin/logos-cpp-generator`.

## Usage

### Code Generator

The `logos-cpp-generator` tool generates C++ wrapper code for Logos plugins.

#### Basic Usage

```bash
# Generate wrapper for a single plugin (uses default output directory)
logos-cpp-generator /path/to/plugin.dylib

# Specify custom output directory
logos-cpp-generator /path/to/plugin.dylib --output-dir /custom/output/path

# Generate only the module files (no core manager or umbrella headers)
logos-cpp-generator /path/to/plugin.dylib --module-only

# Combine options
logos-cpp-generator /path/to/plugin.dylib --output-dir /custom/output --module-only
```

#### Generate from Metadata

```bash
# List dependencies from metadata.json
logos-cpp-generator --metadata /path/to/metadata.json

# Generate wrappers for all dependencies
logos-cpp-generator --metadata /path/to/metadata.json --module-dir /path/to/modules

# Generate with custom output directory
logos-cpp-generator --metadata /path/to/metadata.json --module-dir /path/to/modules --output-dir /custom/output

# Generate only module files (no core manager or umbrella headers)
logos-cpp-generator --metadata /path/to/metadata.json --module-dir /path/to/modules --module-only

# Generate only core manager and umbrella files (assumes module files already exist)
logos-cpp-generator --metadata /path/to/metadata.json --general-only

# Generate general files with custom output directory
logos-cpp-generator --metadata /path/to/metadata.json --general-only --output-dir /custom/output
```

#### Options

**`--output-dir /path/to/output`**
- **Default:** If not specified, generated files are placed in `logos-cpp-sdk/cpp/generated/`
- **Custom:** Specify any directory for the generated files
- The output directory will be created automatically if it doesn't exist

**`--module-only`**
- When specified, generates only the requested module's `.h` and `.cpp` files
- Skips generation of `core_manager_api.*` and umbrella headers (`logos_sdk.*`)
- Useful when you only need wrapper code for specific modules

**`--general-only`**
- When specified with `--metadata`, generates only the core manager and umbrella SDK files
- Assumes module wrapper files already exist in the output directory
- Generates: `core_manager_api.h`, `core_manager_api.cpp`, `logos_sdk.h`, `logos_sdk.cpp`
- The umbrella headers will include references to all modules listed in the metadata's `dependencies` array
- For each dependency (e.g., `"waku_module"`), it will:
  - Include `waku_module_api.h` in the header
  - Include `waku_module_api.cpp` in the source
  - Create a `WakuModule waku_module;` member in the `LogosModules` struct
- Does not require `--module-dir` since it doesn't process plugins

#### Generated Files

**By default (without `--module-only` or `--general-only`):**
- `<module>_api.h` and `<module>_api.cpp` - Wrapper code for each module
- `core_manager_api.h` and `core_manager_api.cpp` - Core manager wrapper
- `logos_sdk.h` and `logos_sdk.cpp` - Umbrella headers including all modules

**With `--module-only`:**
- Only `<module>_api.h` and `<module>_api.cpp` - Wrapper code for the requested module(s)

**With `--general-only`:**
- Only `core_manager_api.h` and `core_manager_api.cpp` - Core manager wrapper
- Only `logos_sdk.h` and `logos_sdk.cpp` - Umbrella headers that reference existing module files

#### Typical Workflow

A common workflow is to generate module wrappers separately, then generate the umbrella SDK:

```bash
# Step 1: Generate individual module wrappers
logos-cpp-generator /path/to/plugin1.dylib --module-only --output-dir ./generated
logos-cpp-generator /path/to/plugin2.dylib --module-only --output-dir ./generated

# Step 2: Generate core manager and umbrella SDK (references modules from step 1)
logos-cpp-generator --metadata metadata.json --general-only --output-dir ./generated
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

Universal (codegen-driven) modules — those built from a `package_xxx_impl.h` header rather than a handcrafted `QObject` plugin — don't see the raw `LogosAPI` at all. Instead, the codegen-generated provider populates a narrow `LogosModuleContext` base class with everything an impl typically needs:

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
into the API). The same applies to `interface: "provider"` modules whose methods
are marked with `LOGOS_METHOD`. See `cpp-generator/docs/spec.md` →
*Method documentation* for details.

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
| `instanceId()` | Stable per-instance ID assigned by the host. Two side-by-side instances of the same module get distinct IDs. |
| `instancePersistencePath()` | Per-instance writable data directory the host owns the lifecycle of. The canonical place for module state (config, caches, small databases). Wiped on uninstall; survives upgrades. |
| `modules()` | The module's flat `LogosModules` aggregate — one accessor per `metadata.json#dependencies` entry (nothing else; apps that need to manage the core do so via liblogos' C API). `LogosModules` is forward-declared in the SDK header and made complete by the impl's `#include "logos_sdk.h"`, so the call site just writes `modules().some_dep.someMethod(...)`. Each accessor's wrapper class signatures use the type surface picked at THIS module's build time (see "API style" below). |

#### API style: Qt vs std

Each module's build picks **one** API style for the generated `<Module>` client wrappers and the `LogosModules` umbrella — they're mutually exclusive, no composite output:

| `metadata.json#interface` | `LOGOS_API_STYLE` | Wrapper signatures |
|---|---|---|
| `"universal"` / `"cdylib"` | `lp` | `std::string`, `std::vector<std::string>`, `LogosMap`, `LogosList`, `int64_t`, `StdLogosResult` |
| `"legacy"` / `"provider"` / absent | `qt` (default) | `QString`, `QStringList`, `QVariantList`, `QVariantMap`, `int`, `LogosResult` |

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
// Legacy / provider module (api-style=qt):
QString reply = modules().some_dep.echo(QString("hi"));
```

The two carry the same values; the `lp` wrapper marshals them over the logos-protocol C ABI (`lp_*`) instead of `QVariant`, so the calling translation unit needs zero Qt headers and links no qt-sdk.

> **Migrating to std types**: The choice is driven entirely by `interface`. A handcrafted module that wants std types should switch to `interface: "universal"` — there's no per-flag override on `metadata.json`.

All getters return empty / null values when the module is loaded outside a host that provisions a context (CLI tests, unit tests using the impl directly). The `onContextReady()` hook still fires once at framework load time; tests that bypass the framework can call `_logosCoreSetContext_` / `_logosCoreSetLogosModulesPtr_` directly to simulate.

Codegen does NOT require inheritance — modules that don't inherit `LogosModuleContext` compile unchanged. The generator emits a single `onInit` override per provider that delegates to SFINAE'd helpers (`_logos_codegen_::maybeSet*`), and the non-inheriting overloads collapse to no-ops.

#### Events: `logos_events:`

Universal modules declare events in a Qt-`signals:`-style `logos_events:` section. The codegen parses each prototype, emits the matching method bodies in a sidecar `<name>_events.cpp` (Qt-MOC style), and ships a `<name>.lidl` file describing them so consumer-side codegen can produce typed subscribers:

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

The author writes only the declarations; the codegen supplies the bodies (analogous to Qt MOC for `signals:`). Each call marshals typed args into a `QVariantList` and routes them through `LogosModuleContext::emitEventImpl_` → `LogosProviderBase::emitEvent` → the existing QRO `eventResponse` channel. No wire-format change.

**Consumer side** — typed `on<EventName>(...)` accessors are generated on the dep's `<Module>` wrapper. The generic `onEvent(name, cb)` channel stays available as a forward-compat escape hatch:

```cpp
// From any module that depends on the one declaring the events:
modules().my_module.onUserLoggedIn(
    [](const std::string& userId, int64_t timestamp) {
        // typed args, no manual QVariantList unpacking
    });
```

The accessor's parameter types follow the consumer's own `--api-style` (so a `universal` consumer sees `const std::string&` / `int64_t`, a handcrafted Qt consumer sees `const QString&` / `int`).

### API

#### LogosResult

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
target_link_libraries(my_target PRIVATE logos-cpp-sdk::logos_sdk)
```

The package config re-resolves transitive dependencies (`Qt6 Core/RemoteObjects`, `Boost system`, `OpenSSL`, `nlohmann_json`), so consumers don't have to wire them up manually. The static archive references OpenSSL `SSL_CTX_*`/`X509_*` and Boost `system::error_code`; without `find_package`'s imported target the link step fails.

### Transports

The SDK supports multiple transports, selected via `LogosTransportConfig`:

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

For processes that want to override the SDK-wide default, use `LogosTransportConfigGlobal::setDefault()` once at startup before any `LogosAPI` is constructed.

### Requirements

#### Build Tools
- CMake (3.x or later)
- Ninja build system
- pkg-config

#### Dependencies
- Qt6 (qtbase)
- Qt6 Remote Objects (qtremoteobjects)
- Boost (system)
- OpenSSL
- nlohmann_json

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
