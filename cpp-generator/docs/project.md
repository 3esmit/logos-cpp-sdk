# Logos Code Generator — Project Description

## Project Structure

```
cpp-generator/
├── main.cpp                        # Entry point — dispatches to legacy or experimental
├── CMakeLists.txt                  # Build config
├── compile.sh                      # Standalone build script
├── legacy/                         # Original generator (unchanged from master)
│   ├── main.cpp                    # legacy_main() — plugin/metadata/provider-header modes
│   ├── generator_lib.h/cpp         # Shared utilities, type mapping, header parser
│   └── legacy_main.h              # Forward declaration
├── experimental/                   # C++/Qt-specific generator backends
│   ├── lidl_compat.h              # Bridges the backends onto logos-lidl's std AST
│   ├── lidl_emit_common.h/cpp     # LIDL type → Qt/std type-name mapping
│   ├── lidl_gen_client.h/cpp      # Typed client stub generation (+ Doxygen /// docs)
│   ├── lidl_gen_cdylib.h/cpp      # cdylib module-impl C-ABI export generation
│   └── impl_header_parser.h/cpp   # C++ impl header → lidl::ModuleDecl
│   # The lexer/parser/AST/serializer/validator now live in the standalone
│   # logos-lidl repo (linked via find_package(logos-lidl)); the Qt glue
│   # emitters live in logos-qt-sdk's logos-qt-generator.
└── docs/                          # This documentation
```

## Components

### Entry Point (`main.cpp`)

Checks for `--from-header` or `--lidl` flags before creating `QCoreApplication`. If neither is present, falls through to `legacy_main()`.

### LIDL frontend — `logos-lidl` (consumed as a library)

The lexer, parser, AST, serializer, and validator are **no longer embedded here** — they live in the standalone **`logos-lidl`** repo, the language-neutral (Qt-free) common frontend every Logos SDK shares (C++ here, Rust in logos-rust-sdk, …). cpp-generator links it via `find_package(logos-lidl)` and reaches it through `experimental/lidl_compat.h`.

`logos-lidl` exposes (`namespace lidl`):

- `lidl::parse(std::string) → ParseResult` (`ModuleDecl` + error/line/column)
- `lidl::serialize(ModuleDecl) → std::string`
- `lidl::validate(ModuleDecl) → ValidationResult`
- the **AST**: `TypeExpr` (`Kind`: Primitive/Array/Map/Optional/Named, `name`, `elements`), `ParamDecl`, `FieldDecl`, `MethodDecl` (name, params, returnType, `description`, `jsonReturn`, `resultReturn`), `EventDecl` (name, params, `description`), `TypeDecl`, `ModuleDecl`. (logos-lidl also exposes an AST↔JSON bridge and a C ABI that the Rust SDK consumes over FFI — not used by this generator.)

The `.lidl` grammar (defined in logos-lidl):

```
module     = "module" IDENT "{" body "}"
body       = (metadata | type_def | method_def | event_def)*
metadata   = "version" STRING | "description" STRING | "category" STRING
           | "depends" "[" (IDENT ("," IDENT)*)? "]"
type_def   = "type" IDENT "{" field* "}"
field      = "?"? IDENT ":" type_expr
method_def = "method" IDENT "(" params ")" "->" type_expr ("description" STRING)?
event_def  = "event" IDENT "(" params ")" ("description" STRING)?
params     = (IDENT ":" type_expr ("," IDENT ":" type_expr)*)?
type_expr  = IDENT | "[" type_expr "]" | "{" type_expr ":" type_expr "}"
           | "?" type_expr
```

Validation (in logos-lidl) checks: empty module name, duplicate type/method/event names, builtin type shadowing, unknown named type references, duplicate parameter names. Serialization round-trips `ModuleDecl` back to `.lidl` text (incl. the trailing `description "…"` clause).

### Compat shim (`lidl_compat.h`)

Bridges the existing Qt-flavored backends onto logos-lidl's std AST so they compile unchanged:

- brings the `lidl::` AST types into the global scope the backends use (via `using`)
- `qs(std::string) → QString` plus a `QTextStream << std::string` overload, so emission of AST string fields just works
- name-compatible shims `lidlParse` / `lidlSerialize` / `lidlValidate` over `lidl::parse`/`serialize`/`validate`

### Type Mapping (`lidl_emit_common.h/cpp`)

- `lidlTypeToQt(TypeExpr)` / `lidlTypeToStd(TypeExpr)` — LIDL type → Qt / std type-name strings
- `lidlIsStdConvertible(TypeExpr)` — whether a type has a pure-C++ (Qt-free) representation
- `lidlToPascalCase(name)` — converts `snake_case` to `PascalCase`

### Client stubs (`lidl_gen_client.h/cpp`)

- `lidlMakeHeader(ModuleDecl)` / `lidlMakeSource(ModuleDecl)` — typed `<Module>` client wrapper; each method (and its `…Async` twin) carries a Doxygen `///` comment generated from the method's `description`
- `lidlGenerateMetadataJson(ModuleDecl)` — generates metadata.json content

### cdylib backend (`lidl_gen_cdylib.h/cpp`)

Emits the Qt-free half of a universal C++ cdylib module:

- `lidlCdylibSupported(ModuleDecl)` — gate to the std-convertible (Qt-free) type subset
- `lidlMakeModuleImplExports(...)` — the `logos_module_impl.h` C-ABI export wrapper around the universal impl class (compiled into the module's cdylib; dispatches via nlohmann::json)
- `lidlMakeEventsSourceCdylib(...)` — typed `logos_events:` bodies marshalling into nlohmann::json

### Per-build API-style choice (`legacy/generator_lib.{h,cpp}`)

The codegen exposes **one** wrapper class per module — `<Module>` — with signatures that match the API style picked at the consumer's build time. The two styles are mutually exclusive (no composite output):

| `--api-style` | Wrapper signatures |
|---|---|
| `qt` (default) | `QString` / `QStringList` / `QVariantList` / `QVariantMap` / `int` / `LogosResult` |
| `lp` | `std::string` / `std::vector<std::string>` / `LogosMap` / `LogosList` / `int64_t` / `StdLogosResult`, over the Qt-free logos-protocol C ABI |

(A third value, `std` — std signatures over a `QVariant` / `LogosAPIClient` body — was retired; the generator now rejects `--api-style=std` instead of aliasing it.)

Both styles emit:

- A `<Module>` client class with sync method shapes + matching `<method>Async(...)` overloads.
- The std variant additionally inlines Qt↔std conversion in its `.cpp` so the caller's translation unit needs zero Qt headers.

The umbrella `logos_sdk.h` is also generated per-build and aggregates every dep into a flat `LogosModules` struct — no nested view:

```cpp
struct LogosModules {
    LogosAPI*       api;
    SomeDep         some_dep;              // one accessor per `metadata.json#dependencies` entry
    // ...
};
```

Only the modules explicitly listed as dependencies are exposed. The runtime's `core_manager` is intentionally NOT in `LogosModules` — apps that need to manage the core do so via liblogos' C API, not via a typed RPC wrapper.

`ApiStyle` enum + new helpers in `generator_lib`:

- `enum class ApiStyle { Qt, Lp }` — passed to every wrapper-emitting function.
- File-local `mapParamTypeStd` / `mapReturnTypeStd` — the std-side type-mapping table the `lp` surface exposes. Hidden from `generator_lib.h` (not part of the public surface).
- `makeHeader(moduleName, className, methods, apiStyle, events)` / `makeSource(moduleName, className, headerBaseName, methods, apiStyle, events)` — single entry points that branch on `apiStyle` internally to emit the right include block, signature shape, and conversion bridges. `events` is loaded from a `<name>.lidl` sidecar via `--events-from`; when non-empty, the wrapper also gets one typed `on<EventName>(callback)` adapter per declared event (callback arg types follow `apiStyle`).

Flag plumbing:

1. `metadata.json#interface == "universal"` (or `"cdylib"`) → `mkLogosModule.nix` adds `-DLOGOS_API_STYLE=lp` to `extraCmakeFlags`. Anything else (`"legacy"`, `"provider"`, absent) leaves the default `qt`.
2. `LogosModule.cmake` reads `${LOGOS_API_STYLE}` (default `qt`) and forwards `--api-style=${LOGOS_API_STYLE}` to the `logos-cpp-generator --general-only` invocation that writes the umbrella. Each module's Nix build emits **two** header derivations (`<name>.headers-qt` and `<name>.headers-lp`) via `buildHeaders.nix` — one `logos-cpp-generator --api-style=…` run per style, at the dep's build time. A consumer's `buildPlugin.nix` picks `dep.headers-${apiStyle}` and copies its `include/` straight into the build sandbox; no codegen runs at consume time. Nix's laziness means only the variant a downstream actually depends on is realised.
3. `legacy/main.cpp` parses `--api-style` once (rejecting the retired `std`) and threads the resulting `ApiStyle` through `generateFromPlugin`, `writeUmbrellaHeader{,FromDeps}`. No per-style filenames are ever emitted; each module gets a single `<name>_api.h` + `<name>_api.cpp` pair regardless of style.

### Provider Generation (logos-qt-generator)

> The Qt provider glue (`lidl_gen_provider.{h,cpp}`) is emitted by **logos-qt-sdk's `logos-qt-generator`**, not this binary — it consumes the same `logos-lidl` frontend (+ the shared `lidl_emit_common` / `impl_header_parser` / `lidl_compat.h` helpers, distributed under `share/lidl-frontend`). Documented here for reference.

- `lidlMakeProviderHeader(ModuleDecl, implClass, implHeader)` — generates Qt glue header
  - Emits `nlohmannToQVariant()` helper when any method has `jsonReturn = true`
  - Always emits an `onInit(LogosAPI*) override` that, via SFINAE'd helpers in `logos_module_context.h`, (a) copies the three runtime-injected properties (`modulePath`, `instanceId`, `instancePersistencePath`) into the impl, (b) constructs a per-module `LogosModules` aggregate and threads its pointer through the same base, and (c) installs the typed-event callback (`maybeSetEmitEvent`) consumed by `<name>_events.cpp` method bodies. Impls that don't inherit `LogosModuleContext` compile unchanged — the helper overloads collapse to no-ops. The full `LogosAPI` is never exposed past the provider boundary.
  - Always emits `#include "logos_sdk.h"` and a `std::unique_ptr<LogosModules> m_logosModules` member; ownership lives on the provider, the context base sees only a non-owning `void*` reinterpreted in `LogosModuleContext::modules()` (which depends on the impl's TU having included `logos_sdk.h`).
- `lidlMakeProviderDispatch(ModuleDecl)` — generates callMethod/getMethods dispatch. `getMethods()` emits the full interface: each method tagged `type: "method"`, then each `module.events` entry tagged `type: "event"` (name, signature, parameters, escaped `description`; no returnType/isInvokable). There is no separate `getEvents()` — folding events into `getMethods()` keeps the provider vtable ABI-stable.
- `lidlMakeEventsSource(ModuleDecl, implClass, implHeader)` — generates `<name>_events.cpp`: Qt-MOC-style method bodies for prototypes declared in the impl's `logos_events:` block. Each body marshals typed args into a `QVariantList` and calls `this->emitEventImpl_("<name>", &args)` on the LogosModuleContext base.
- `lidlGenerateProviderGlue(lidlPath, ...)` — full pipeline from .lidl file. Also emits `<name>_events.cpp` and a `<name>.lidl` sidecar (via `lidlSerialize`) when the module has any events; both ride the dep's `headers-*` outputs to power consumer-side typed `on<X>()` accessors.

### Impl Header Parser (`impl_header_parser.h/cpp`)

- `parseImplHeader(headerPath, className, metadataPath, err)` — parses C++ header + metadata.json into ModuleDecl
- State machine: `LookingForClass` → `InClass` → `InPublic`/`InPrivate`/`InLogosEvents`
- The literal `logos_events:` token (defined in `logos_module_context.h` as `#define logos_events public`) opens an events section; bare prototypes inside become `EventDecl{name, params, description}` entries appended to `ModuleDecl.events` (the `description` is the doc comment immediately above the declaration, captured via `joinDocLines` exactly as for methods)
- Skips: constructors, destructors, typedefs, using, friend, enum, struct, `std::function` declarations
- Recognizes `LogosMap` and `LogosList` return types (nlohmann::json aliases) and sets `MethodDecl.jsonReturn = true`
- Template-aware parameter splitting (handles `std::vector<std::string>` correctly)

## CLI Usage

### From C++ impl header (primary use case for universal modules)

```bash
logos-cpp-generator --from-header src/my_module_impl.h \
    --backend qt \
    --impl-class MyModuleImpl \
    --impl-header my_module_impl.h \
    --metadata metadata.json \
    --output-dir ./generated_code
```

Generates: `my_module_qt_glue.h`, `my_module_dispatch.cpp`

### From LIDL file — provider glue

```bash
logos-cpp-generator --lidl my_module.lidl \
    --backend qt \
    --impl-class MyModuleImpl \
    --impl-header my_module_impl.h \
    --output-dir ./generated_code
```

### From LIDL file — client stubs

```bash
logos-cpp-generator --lidl my_module.lidl \
    --output-dir ./generated_code \
    --module-only
```

### Legacy modes (unchanged)

```bash
logos-cpp-generator /path/to/plugin.so --output-dir ./generated
logos-cpp-generator --metadata metadata.json --general-only --output-dir ./generated
logos-cpp-generator --provider-header src/provider.h --output-dir ./generated
```

### Consumer wrapper with typed event accessors

The `--events-from <path>` flag points the legacy `<plugin>.dylib --module-only` codegen at a LIDL sidecar shipped alongside the dep's pre-built headers. When set, the generated `<name>_api.{h,cpp}` gains one typed `on<EventName>(callback)` accessor per declared event (callback arg types match `--api-style`):

```bash
logos-cpp-generator /path/to/plugin.dylib \
    --module-only --api-style lp \
    --events-from /path/to/dep/share/logos/my_module.lidl \
    --output-dir ./generated
```

In Nix builds this is wired automatically: `buildHeaders.nix` looks for `<pluginLib>/share/logos/<name>.lidl` (which `buildPlugin.nix`'s installPhase placed there) and threads it through.

## Building

The generator is built as part of logos-cpp-sdk:

```bash
ws build logos-cpp-sdk     # builds everything including the generator
```

The generator binary is available as `logos-cpp-generator` in module build environments (provided by logos-module-builder's `nativeBuildInputs`).

## Testing

Tests are in `tests/experimental/`:

```bash
ws test logos-cpp-sdk      # runs all tests including experimental
```

The frontend tests (lexer/parser/validator/serializer) moved to the **logos-lidl** repo along with the code; only the C++/Qt-specific backends are tested here:

| Test file | What it tests |
|-----------|---------------|
| `test_lidl_type_mapping.cpp` | `lidlTypeToQt`, `lidlTypeToStd`, `lidlIsStdConvertible`, `lidlToPascalCase` |
| `test_lidl_gen_client.cpp` | Client stub generation: sync/async methods, events, metadata JSON, edge cases |
| `test_impl_header_parser.cpp` | Header parsing: type mapping, access specifiers, skipping private/protected, error cases |

(The lexer/parser/AST/serializer/validator round-trip + description tests live in logos-lidl's own `tests/test_lidl.cpp`.)

Fixture files in `tests/experimental/fixtures/`:
- `sample_impl.h` — module with all supported type variations
- `sample_metadata.json` — metadata with dependencies
- `complex_impl.h` — module with multiple access specifier sections
- `empty_class_impl.h` — class with no public methods
- `empty_metadata.json` — minimal metadata

## Known Limitations

- The impl header parser is lightweight (regex + state machine). It does not handle:
  - Multi-line method declarations
  - Default parameter values
  - Method definitions in the header (only declarations ending with `;`)
  - Nested classes
  - Template methods
  - `std::function` members are silently skipped (never treated as methods)
- LIDL does not support generic/parameterized types or inheritance
- `--from-header` emits the **cdylib** backend here (the `qt` glue backend moved to logos-qt-generator); the **Rust** backend lives in logos-rust-sdk's `lidl-gen`, generating over logos-lidl's C ABI
- Client stub generation (`lidlMakeHeader`/`lidlMakeSource`) is only available from LIDL files, not from `--from-header`
