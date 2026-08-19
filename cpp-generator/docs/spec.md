# Logos Code Generator — Experimental

## Overall Description

The experimental code generator extends `logos-cpp-generator` with two new capabilities: a lightweight Interface Definition Language (LIDL) for declaring module contracts, and a C++header parser that can infer module interfaces directly from pure C++ implementation classes. Both paths produce the same output: the **Qt-free** `logos_module_*` C-ABI provider glue that bridges pure C++ module implementations to the runtime. (It used to emit the Qt plugin glue directly; turning the C ABI into a Qt plugin is now a downstream step, `logos-qt-host-generator --backend cdylib` in logos-plugin-qt, and that seam is what lets the Rust and JS providers target the same ABI.)

The goal is to decouple module business logic from the Qt framework. Module authors write standard C++ using `std::string`, `int64_t`, `std::vector<T>`, and the build system generates all Qt boilerplate (`QObject`, `Q_PLUGIN_METADATA`, `QString` conversions, method dispatch) automatically.

## Definitions & Acronyms


| Term                 | Definition                                                                                                                               |
| -------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| **LIDL**             | Logos Interface Definition Language — a lightweight DSL for declaring module interfaces                                                  |
| **Universal Module** | A module whose implementation is pure C++ (no Qt types) with all Qt glue generated at build time                                         |
| **Provider Glue**    | Generated code that wraps a pure C++ impl class in a `LogosProviderObject` with `callMethod()` dispatch and `getMethods()` introspection |
| **Client Stub**      | Generated type-safe C++ wrapper class that callers use to invoke a module's methods without string-based dispatch                        |
| **Dispatch**         | The generated `callMethod()` function that maps string method names to typed method calls on the provider object                         |
| **Impl Header**      | The pure C++header file (`_impl.h`) that declares a module's public methods using standard C++ types                                     |
| **TypeExpr**         | The AST node representing a type in the LIDL type system                                                                                 |
| **ModuleDecl**       | The AST node representing a complete module declaration (name, version, methods, events, types)                     |


## Domain Model

### Two Paths to the Same Output

```
Path 1: LIDL file                    Path 2: C++ impl header
    │                                     │
    ▼                                     ▼
 lidlParse()                       parseImplHeader()
    │  (logos-lidl's lidl::parse,         │
    │   via lidl_compat.h)                │
    ▼                                     │
 lidlValidate()                           │
    │                                     │
    ▼                                     ▼
 ModuleDecl  ◄────── same AST ──────► ModuleDecl
    │                                     │
    ├──► lidlMakeTypesHeaderCdylib()      │
    │    lidlMakeModuleImplExports()      │
    │    lidlMakeEventsSourceCdylib()     │
    │         → logos_module_* C ABI       │
    │           (Qt packaging is a         │
    │            downstream step:          │
    │            logos-qt-host-generator)  │
    │                                     │
    ├──► lidlMakeHeader()                 │
    │         → <name>_api.h              │
    │                                     │
    └──► lidlMakeSource()                 │
              → <name>_api.cpp            │
```

(There is no `lidlTokenize()` step here any more: the lexer lives in
logos-lidl with the rest of the frontend, behind `lidl::parse`.)

Both paths converge at `ModuleDecl`, the shared AST. From there, the same generation functions produce identical output regardless of the input format.

### LIDL Language

LIDL is a minimal interface definition language. A module declaration contains metadata, type definitions, method signatures, and event signatures:

```
module wallet_module {
    version "1.0.0"
    description "Wallet operations"
    category "finance"
    depends [crypto_module]

    type Account {
        address: tstr
        balance: uint
        ? label: tstr          ; optional field
    }

    method createAccount(passphrase: tstr) -> tstr
    method getBalance(address: tstr) -> uint
    method listAccounts() -> [tstr]
    method transfer(from: tstr, to: tstr, amount: uint) -> result

    event onTransfer(from: tstr, to: tstr, amount: uint)
}
```

Comments start with `;` and run to end of line.

### Type System

Built-in primitive types:


| LIDL type | Meaning                                 | Qt mapping    | C++ std mapping        |
| --------- | --------------------------------------- | ------------- | ---------------------- |
| `tstr`    | Text string                             | `QString`     | `std::string`          |
| `bstr`    | Binary data                             | `QByteArray`  | `std::vector<uint8_t>` |
| `int`     | Signed 64-bit integer                   | `qlonglong`   | `int64_t`              |
| `uint`    | Unsigned 64-bit integer                 | `qulonglong`  | `uint64_t`             |
| `float64` | Double precision float                  | `double`      | `double`               |
| `bool`    | Boolean                                 | `bool`        | `bool`                 |
| `result`  | Structured result (success/value/error) | `LogosResult` | `LogosResult`          |
| `any`     | Untyped value                           | `QVariant`    | `QVariant`             |
| `void`    | No return value                         | `void`        | `void`                 |


Composite types:

- `[T]` — Array of T (e.g., `[tstr]` → `QStringList` / `std::vector<std::string>`)
- `{K: V}` — Map from K to V (e.g., `{tstr: int}` → `QVariantMap`)
- `?T` — Optional T (→ `QVariant` on the Qt surface, which loses the value type;
  `std::optional<T>` on the std surface — see *Optionality* in `project.md`)

Named types reference `type` definitions within the same module.

### C++ Header Parsing

The `--from-header` mode parses a C++implementation header to extract public method signatures. It maps C++ types to LIDL types:


| C++ type                             | LIDL type                                                          |
| ------------------------------------ | ------------------------------------------------------------------ |
| `std::string` / `const std::string&` | `tstr`                                                             |
| `bool`                               | `bool`                                                             |
| `int64_t`                            | `int`                                                              |
| `uint64_t`                           | `uint`                                                             |
| `double`                             | `float64`                                                          |
| `void`                               | `void`                                                             |
| `std::vector<std::string>`           | `[tstr]`                                                           |
| `std::vector<uint8_t>`               | `bstr`                                                             |
| `std::vector<int64_t>`               | `[int]`                                                            |
| `std::vector<uint64_t>`              | `[uint]`                                                           |
| `std::vector<double>`                | `[float64]`                                                        |
| `std::vector<bool>`                  | `[bool]`                                                           |
| `LogosMap`                           | `{tstr: any}` (Map) — nlohmann::json alias; sets `jsonReturn` flag |
| `LogosList`                          | `[any]` (Array) — nlohmann::json alias; sets `jsonReturn` flag     |
| `QVariantMap`                        | `{tstr: any}` (Map) — legacy Qt type                               |
| `QVariantList`                       | `[any]` (Array) — legacy Qt type                                   |
| `QStringList`                        | `[tstr]` (Array) — legacy Qt type                                  |
| Anything else                        | `any`                                                              |


`LogosMap` and `LogosList` are `using` aliases for `nlohmann::json` defined in `logos_json.h` (part of the SDK). They allow module implementations to remain completely Qt-free while returning rich structured data. The parser maps them to the same LIDL shapes as `QVariantMap`/`QVariantList`, but sets the `jsonReturn` flag on the method so the generator emits an `nlohmannToQVariant()` conversion in the glue layer.

The parser uses a state machine to find the target class, track access specifiers (`public`/`private`/`protected`), and extract method declarations. It skips constructors, destructors, typedefs, using declarations, `std::function` members, and non-method statements. While scanning, it also captures any doc comment immediately above a method declaration as that method's `description` (see [Method documentation](#method-documentation)).

Module metadata (name, version, description, dependencies) comes from `metadata.json`, not from the header.

### Method documentation

A doc comment written directly above a method's declaration in the impl header
becomes that method's `description`, stored on `MethodDecl.description` in the
shared AST and emitted into the `description` field of each `getMethods()`
entry. Because `getMethods()` is what the framework's `getPluginMethods()`
returns, the description flows — with no extra call — to `lm methods`,
`logoscore module-info`, and Basecamp's Methods list.

Only **doc comments** are captured: `///` line comments and `/** … */` /
`/*! … */` block comments. Plain `//` and `/* … */` comments are ignored, so
section separators and incidental notes don't leak into the API. A multi-line
doc comment is preserved with its line breaks (markers stripped, lines joined
with `\n`; leading/trailing blank lines dropped, interior blank lines kept), and
only comments *immediately adjacent* to the declaration (no blank line in
between) attach.

```cpp
class WalletModuleImpl : public LogosModuleContext {
public:
    /// Transfers `amount` from the active account to `toAddress`.
    /// Returns the resulting transaction hash.
    std::string transfer(const std::string& toAddress, int64_t amount);
};
```

→ the `transfer` entry in `getMethods()` gains
`"description": "Transfers `amount` from the active account to `toAddress`.\nReturns the resulting transaction hash."` (the two lines preserved, joined with `\n`)

A method with no doc comment simply has no `description` field. Methods
introspected purely via Qt's `QMetaObject` (legacy `Q_INVOKABLE` modules with no
generated dispatch) carry no comments at runtime and therefore have no
`description`.

### Event documentation

Events are the subscribe-half of a module's API (methods are the call-half), and
document the same way. A doc comment directly above an event declaration in the
`logos_events:` section (see [Event Emission](#event-emission-via-logos_events)
below) becomes that event's `description`, stored on `EventDecl.description` in
the shared AST and emitted into the `description` field of the event's entry in
**`getMethods()`**.

`getMethods()` returns the module's *whole* interface — methods **and** events —
with each entry tagged by a `"type"` field (`"method"` or `"event"`). Events ride
inside `getMethods()` deliberately: there is **no** separate `getEvents()` vtable
method, so `LogosProviderObject`'s vtable layout never shifts and old/new hosts
and modules stay binary-compatible (see *Why events live in `getMethods()`*
below). The framework then offers three filtered views over that one call —
`getPluginMethods()` (entries that aren't events), `getPluginEvents()`
(`type == "event"`), and `getPluginInterface()` (everything) — so the
description flows, with no extra provider call, to `lm events`, `logoscore
module-info`'s Events section, and Basecamp's Interface screen.

The capture rules are identical to methods: only `///` line comments and
`/** … */` / `/*! … */` block comments are captured (plain `//` and `/* … */`
are ignored); multi-line comments preserve their line breaks (markers stripped,
joined with `\n`, leading/trailing blanks dropped); only comments immediately
adjacent to the declaration attach.

```cpp
logos_events:
    /// Emitted once the user has authenticated.
    /// Carries the freshly issued session token.
    void userLoggedIn(const std::string& userId, const std::string& token);
```

→ the `userLoggedIn` entry in `getMethods()` gains
`"type": "event"` and
`"description": "Emitted once the user has authenticated.\nCarries the freshly issued session token."`

An event entry carries `type: "event"`, `name`, `signature`, `parameters[]`
(each with `type` and `name`), and — when documented — `description`. Unlike a
method entry it has no `returnType` or `isInvokable`: events are void,
fire-and-forget. Events are a universal (`--from-header`) concept.
(An entry with no `"type"` is treated as a method, so a module built
against a pre-events SDK simply reports zero events.)

An event's `description` may also be supplied out-of-band via an optional
`description` field on the corresponding `metadata.json` `events[]` entry (the
doc comment takes the same role for both sources).

### Event Emission via `logos_events:`

Universal modules declare events in a Qt-`signals:`-style section parsed by the codegen. The same method name appears on both sides — declared in `logos_events:`, called directly to emit:

```cpp
#include <logos_module_context.h>

class MyModuleImpl : public LogosModuleContext {
public:
    void doWork() {
        userLoggedIn("alice", 12345);            // typed emit, same name
    }

logos_events:                                     // expands to `public:`; recognised by impl_header_parser
    void userLoggedIn(const std::string& userId, int64_t timestamp);
    void messageReceived(const std::string& from, const std::string& body);
};
```

`impl_header_parser.cpp` recognises the raw `logos_events:` token (before preprocessing) and populates `ModuleDecl.events` with one `EventDecl` per prototype. Three artifacts get emitted from this:

1. **`<name>_events_cdylib.cpp`** — Qt-MOC-style definitions of each declared event method on the impl class. Bodies marshal typed args into an `nlohmann::json` array and call `this->emitEventImpl_("<event>", &args)`, a protected helper on `LogosModuleContext`:

   ```cpp
   void MyModuleImpl::userLoggedIn(const std::string& userId, int64_t timestamp) {
       nlohmann::json args = nlohmann::json::array();
       args.push_back(userId);
       args.push_back(timestamp);
       emitEventImpl_("userLoggedIn", &args);
   }
   ```

   (This used to be a `<name>_events.cpp` marshalling into a `QVariantList`, back
   when the emitter it fed was a Qt provider object. A universal module's impl
   side is Qt-free, so the payload is JSON and the file carries the `_cdylib`
   suffix.)

2. **Emit-callback wiring** — `<name>_module_impl.cpp`, the generated C-ABI export TU, installs the callback through `_logos_codegen_::maybeSetEmitEvent` alongside `maybeSetModuleName` / `maybeSetContext` / `maybeSetLogosModules`. The lambda casts the void* back to `nlohmann::json`, dumps it, and hands it to the `logos_module_emit_cb` the host registered via `logos_module_set_emit_callback`:

   ```cpp
   _logos_codegen_::maybeSetEmitEvent(lidlImpl(),
       [](const std::string& name, void* args) {
           const nlohmann::json* payload = static_cast<const nlohmann::json*>(args);
           std::lock_guard<std::mutex> lock(g_emitMutex);
           if (g_emitCb)
               g_emitCb(name.c_str(), payload ? payload->dump().c_str() : "[]", g_emitUd);
       });
   ```

   (Was a `<name>_qt_glue.h` lambda forwarding to `LogosProviderBase::emitEvent(QString, QVariantList)`; that glue is the retired shape described under *Generated Output* below.)

3. **`<name>.lidl` sidecar** — a serialised view of the module's declared events (using `lidlSerialize`, which since the frontend extraction is `lidl::serialize` in the logos-lidl library, re-exported by `experimental/lidl_compat.h`; the `lidl_serializer.cpp` that used to hold it is gone from this repo):

   ```
   module my_module {
     event userLoggedIn(userId: tstr, timestamp: int)
     event messageReceived(from: tstr, body: tstr)
   }
   ```

   `buildPlugin.nix` ships this at `$out/share/logos/<name>.lidl`. `buildHeaders.nix` passes it to the consumer-side codegen via `--events-from`, which adds typed `on<EventName>(callback)` accessors to the generated `<Module>` wrapper (one per declared event, callback-arg types respect `--api-style`).

Module metadata (name, version, description, dependencies) still comes from `metadata.json`, not from the header.

### Generated Output

> **Historical.** `<name>_qt_glue.h` / `<name>_dispatch.cpp` were emitted by
> `lidl_gen_provider`, which is deleted. A module now emits the `logos_module_*` C ABI
> (`--backend cdylib`) and `logos-qt-host-generator` turns that into a Qt plugin. The sections
> below describe the retired shape and are kept because the `onInit` wiring they document still
> applies to the cdylib glue.

#### Provider Glue (`<name>_qt_glue.h`)

Contains two classes:

1. **ProviderObject** — inherits `LogosProviderBase`, holds an instance of the impl class (`m_impl`). Each public method is wrapped with type conversion:
  - Qt parameters → C++ std parameters (e.g., `QString.toStdString()`)
  - Call `m_impl.method(...)`
  - C++ std return → Qt return (e.g., `QString::fromStdString(result)`)
  - For `jsonReturn` methods (returning `LogosMap`/`LogosList`), the glue calls a generated `nlohmannToQVariant()` recursive helper to convert `nlohmann::json` → `QVariant`/`QVariantMap`/`QVariantList`
  - Always overrides `onInit(LogosAPI*)` to (a) copy the three runtime-injected properties (`modulePath`, `instanceId`, `instancePersistencePath`) into the impl when it inherits from `LogosModuleContext`, and (b) construct a per-module `LogosModules` (from `generated_code/logos_sdk.h`) owned by the provider, threading its pointer through the same context base. Both wire-ups go through SFINAE'd helpers in `logos_module_context.h` (`_logos_codegen_::maybeSetContext` / `maybeSetLogosModules`), so non-inheriting impls compile unchanged and the `LogosAPI` never escapes the provider.
2. **Plugin** — `QObject` subclass implementing `PluginInterface` and `LogosProviderPlugin`. Carries `Q_PLUGIN_METADATA` and `Q_INTERFACES`. Its `createProviderObject()` factory returns a new ProviderObject instance.

#### Dispatch (`<name>_dispatch.cpp`)

Implements two methods on the ProviderObject:

1. `**callMethod(methodName, args)`** — string-based dispatch table. For each method, extracts args from `QVariantList`, calls the typed wrapper, returns result as `QVariant`. Void methods return `QVariant(true)`.
2. `**getMethods()**` — returns a `QJsonArray` describing the module's **whole interface — both methods and events**. Each entry carries a `"type"` of `"method"` or `"event"`:
   - **method** entries have `type: "method"`, `name`, `signature`, `returnType`, `isInvokable`, `parameters[]` (with `type` and `name`), and — when the declaration has a doc comment — `description` (see [Method documentation](#method-documentation)).
   - **event** entries (one per `logos_events:` declaration) have `type: "event"`, `name`, `signature`, `parameters[]`, and an optional `description` (see [Event documentation](#event-documentation)). They omit `returnType`/`isInvokable` — events are void.

   The framework slices this single array into `getPluginMethods()` (non-event entries), `getPluginEvents()` (`type == "event"`), and `getPluginInterface()` (everything), which is what surfaces in `lm methods`/`lm events`, `logoscore module-info`, and Basecamp's Interface screen.

##### Why events live in `getMethods()`

Folding events into `getMethods()` — rather than adding a sibling `getEvents()` virtual — is a deliberate **ABI** choice. `LogosProviderObject` is the in-process vtable contract between a host/runtime and a loaded module; inserting a new virtual would shift every later vtable slot and break any mix of old/new host and module binaries. Reusing the existing `getMethods()` slot keeps the vtable byte-for-byte stable: a new host reading an old module just sees no `type: "event"` entries (so zero events), and an old host reading a new module ignores the `"type"` field (events show up in its method list — cosmetic, never a crash). Legacy Qt modules declare no events, so their `getMethods()` is methods-only.

#### Client Stubs (`<name>_api.h` + `<name>_api.cpp`)

Generated from LIDL (not from `--from-header`). Each module gets **one** `<Module>` wrapper class whose signature shape is picked by the consumer's build via `--api-style`:

| `--api-style` | Wrapper signatures |
|---|---|
| `qt` (default) | QString / QStringList / QVariantList / QVariantMap / int / LogosResult |
| `lp` | std::string / std::vector<std::string> / LogosMap / LogosList / int64_t / StdLogosResult, over the Qt-free logos-protocol C ABI |

(`std` — the same signatures over a `QVariant` / `LogosAPIClient` body — was retired; `--api-style=std` is now an error.)

Both styles provide:

- Typed sync methods. The Qt style calls `LogosAPIClient::invokeRemoteMethod()` and converts the `QVariant` result; the lp style calls `logos::LpClient::invoke()` and converts the `nlohmann::json` result — no Qt anywhere in the call.
- Async overloads with callback + timeout.
- Event subscription. The Qt style exposes the generic `on(eventName, callback)` channel plus one typed `on<EventName>(callback)` adapter per declared event; the std style exposes the typed adapters over `logos::LpClient::subscribe`, holding each RAII `LpSubscription` for the wrapper's lifetime. (Both styles once also emitted `setEventSource()` / `eventSource()` / `trigger()` — a consumer-side *emission* surface. It is gone: `test_lidl_gen_client.cpp` asserts no `trigger(` is emitted. A module emits its own events through `logos_events:`, never through a dependency's wrapper.)

The lp wrappers marshal over the logos-protocol C ABI (`lp_*`) instead, so the calling translation unit needs zero Qt headers and links no qt-sdk. (The retired `std` style was the one that shared `invokeRemoteMethod` with the Qt path and generated a Qt<->std conversion inline in its `.cpp`.) Both styles emit the **same filename** (`<name>_api.h` / `<name>_api.cpp`) and the **same class name** (`<Module>`) — the two are mutually exclusive at build time. No `_api_std.{h,cpp}` files are ever produced.

Umbrella files (`logos_sdk.h` / `logos_sdk.cpp`) aggregate every dep into a flat `LogosModules` struct — one accessor per `metadata.json#dependencies` entry, nothing else:

```cpp
struct LogosModules {
    LogosAPI*    api;
    SomeDep      some_dep;       // one per declared dependency
    // ...
};
```

Only the modules explicitly listed as dependencies appear. The runtime's `core_manager` is intentionally NOT exposed here — apps that need to manage the core (basecamp, logoscore) use liblogos' C API directly, not a typed RPC wrapper.

## Features & Requirements

### LIDL Pipeline

The whole frontend now lives in the standalone **logos-lidl** repo; this
generator links it and reaches it through `experimental/lidl_compat.h`, which
re-exports the three stages below under their historical `lidl*` names. There is
no separately callable lexer entry point here any more — `lidlTokenize` was part
of the embedded copy that was deleted.

1. **Lexer** — tokenizes source into keywords, identifiers, string literals, symbols (internal to `lidl::parse`)
2. **Parser** (`lidlParse` → `lidl::parse`) — recursive descent parser producing a `ModuleDecl` AST
3. **Validator** (`lidlValidate` → `lidl::validate`) — checks for duplicate names, unknown type references, builtin shadowing, duplicate parameters
4. **Serializer** (`lidlSerialize` → `lidl::serialize`) — pretty-prints a `ModuleDecl` back to LIDL text (useful for roundtrip testing)

### Impl Header Pipeline

1. **parseImplHeader** — reads `metadata.json` + C++ header, produces a `ModuleDecl`
2. Same generation functions as LIDL path

### Backwards Compatibility

- The remaining generator modes (`--metadata`, plugin path) continue to work unchanged via `runPluginIntrospectMode()` (`plugin_introspect.cpp`; was `legacy/main.cpp`'s `legacy_main()`). `--provider-header` (the `LOGOS_METHOD` dispatch behind `interface: "provider"`) was REMOVED — every provider now goes through the module-impl C ABI; the flag is refused with a message pointing at `interface: "universal"`
- The new `--from-header` and `--lidl` modes are additive
- Generated plugins implement both `PluginInterface` (for `lm` introspection) and `LogosProviderPlugin` (for new-API provider creation)
- The runtime (`logos-liblogos`) already supports both old and new plugin types via `qobject_cast` detection

### Conversion Helper Generation

Conversion helpers are only emitted when needed:

- **String vector helpers** (`lidlToQStringList`, `lidlToStdStringVector`) — emitted when the module uses `[tstr]` parameters or return types
- **nlohmann→Qt helper** (`nlohmannToQVariant`) — emitted when any method has `jsonReturn = true` (i.e., the impl returns `LogosMap` or `LogosList`). This recursive function converts `nlohmann::json` objects, arrays, strings, numbers, and booleans to their `QVariant` equivalents.

