# Logos Code Generator — Project Description

## Project Structure

```
cpp-generator/
├── main.cpp                        # Entry point — `--umbrella`/`--general-only` mode, dispatch to the LIDL backends or plugin introspection
├── CMakeLists.txt                  # Build config
├── compile.sh                      # Standalone build script
├── metadata_dependencies.h         # What a metadata.json `dependencies[]` array declares
├── generator_lib.h/cpp             # Shared emitter library: type mapping, wrapper + umbrella emission
├── lidl_to_json.h/cpp              # ModuleDecl → the JSON surface generator_lib consumes
├── plugin_introspect.h/cpp         # runPluginIntrospectMode() — the QPluginLoader path
│                                   # (plugin/metadata modes). Was `legacy/`, which was
│                                   # never a library: one exported symbol, compiled into
│                                   # this binary and reached by fallthrough.
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

Checks for `--from-header` or `--lidl` flags before creating `QCoreApplication`. If neither is present, falls through to `runPluginIntrospectMode()` in `plugin_introspect.cpp`.

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

### Optionality

`?T` is **two-state**: a value of `T`, or empty. Never three-state — "one LIDL type ↔ one
type per language" leaves nowhere for a third state, because every target has exactly one
empty inhabitant.

**Two spellings, one meaning.** A record field may be written `? name: T` (the flag) or
`name: ?T` (the type kind); the spec binds them to the same declaration, so they MUST emit
identical code. Backends never answer this themselves — logos-lidl's `fieldIsOptional(f)` /
`fieldValueType(f)` (re-exported by `lidl_compat.h`) are the one place the two are
reconciled. **Reading `f.optional` or `f.type.kind == Optional` on its own is a bug.**

**Wire rule.** Absent and explicit null are the *same* state on decode and *different* on
encode:

| | empty is spelled |
|---|---|
| decode, optional slot | absent **or** null → empty |
| decode, required slot | absent and null are both still errors |
| encode, **named** slot (a record field) | the key is **omitted** |
| encode, **positional** slot (argument, return, event param) | `null` — there is no key to omit, and arity must never change |

A round trip therefore **canonicalises**: a peer that sent `"f": null` gets the key back
omitted. A present-but-wrong-typed value is still an error — optional widens the domain by
exactly one inhabitant, it does not switch type checking off.

Per surface:

| Surface | `?T` | Notes |
|---|---|---|
| cdylib / std (`lidlTypeToStd`, `lidl_gen_cdylib`) | `std::optional<T>` | encoded by logos-protocol's `Codec<std::optional<T>>`; key omission is the record emitter's job (a codec never sees the slot) |
| `?any` / `?{tstr: any}` / `?[any]` | `LogosMap` / `LogosList` | collapses: `nlohmann::json` already carries `null`, so wrapping it would make the slot three-state |
| Qt (`lidlTypeToQt`, `lidl_gen_client`) | `QVariant` | two-state (an invalid QVariant is Qt's empty inhabitant) but **untyped** — Qt has no optional template |
| legacy consumer, record **field** (`lidl_to_json` + `generator_lib`) | `QVariant` (Qt) / `std::optional<T>` (Lp) | same answer as the client-stub and cdylib backends respectively; the alias collapse above applies on Lp |
| legacy consumer, **positional** slot (param, return, event param) | `QVariant` (Qt) / `LogosMap` (Lp) | still flattened — see Known Limitations |
| header-first (`impl_header_parser`) | `std::optional<T>` ↔ `?T` | `std::optional<std::optional<T>>` has no LIDL type; it collapses to `?T` and is reported on stderr |

### Client stubs (`lidl_gen_client.h/cpp`)

- `lidlMakeHeader(ModuleDecl)` / `lidlMakeSource(ModuleDecl)` — typed `<Module>` client wrapper; each method (and its `…Async` twin) carries a Doxygen `///` comment generated from the method's `description`
- `lidlGenerateMetadataJson(ModuleDecl)` — generates metadata.json content

### cdylib backend (`lidl_gen_cdylib.h/cpp`)

Emits the Qt-free half of a universal C++ cdylib module:

- `lidlCdylibSupported(ModuleDecl)` — gate to the std-convertible (Qt-free) type subset
- `lidlMakeModuleImplExports(...)` — the `logos_module_impl.h` C-ABI export wrapper around the universal impl class (compiled into the module's cdylib; dispatches via nlohmann::json)
- `lidlMakeEventsSourceCdylib(...)` — typed `logos_events:` bodies marshalling into nlohmann::json

### Per-build API-style choice (`generator_lib.{h,cpp}`)

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

A `dependencies[]` element is either a bare name or an object carrying that name alongside the constraints an installer resolves it by — the two declare the same dependency and generate the same code:

```json
"dependencies": [
    "dep_a",
    { "name": "dep_b", "version": "=1.2.3" },
    { "name": "dep_c", "version": "^2.0", "signer": "did:jwk:abc" }
]
```

Read the array through `dependencyNames()` (`metadata_dependencies.h`) rather than element by element. The umbrella is emitted by several passes over the same array — includes, constructor initialisers, members — and a pass that decides on its own what an element names can decide differently from its neighbours, yielding a member whose type was never included. That aggregate no longer compiles, and nothing catches it until a module builds against it. One reader, one answer.

`ApiStyle` enum + new helpers in `generator_lib`:

- `enum class ApiStyle { Qt, Lp }` — passed to every wrapper-emitting function.
- File-local `mapParamTypeStd` / `mapReturnTypeStd` — the std-side type-mapping table the `lp` surface exposes. Hidden from `generator_lib.h` (not part of the public surface).
- `makeHeader(moduleName, className, methods, apiStyle, events)` / `makeSource(moduleName, className, headerBaseName, methods, apiStyle, events)` — single entry points that branch on `apiStyle` internally to emit the right include block, signature shape, and conversion bridges. `events` is loaded from a `<name>.lidl` sidecar via `--events-from`; when non-empty, the wrapper also gets one typed `on<EventName>(callback)` adapter per declared event (callback arg types follow `apiStyle`).
- `makeUmbrellaHeaderFromDeps(deps, interfaceNames, apiStyle, originName, binding)` / `makeUmbrellaSourceFromDeps(deps, interfaceNames)` — the `logos_sdk.{h,cpp}` aggregate above. `binding` is the `UmbrellaBinding` from `--binding api|origin`: `FromApi` emits the `LogosModules(LogosAPI*)` constructor, `ExplicitOrigin` emits a default-constructible umbrella that names `originName` as the call origin and mentions no `LogosAPI` at all. They return the text; `main.cpp`'s `runUmbrellaMode` writes it. That split is what lets the aggregate be asserted on directly, without a filesystem.

Flag plumbing:

1. `metadata.json#interface == "universal"` (or `"cdylib"`) → `mkLogosModule.nix` adds `-DLOGOS_API_STYLE=lp` to `extraCmakeFlags`. Anything else (`"legacy"`, absent — and `"universal"` with `type: "ui_qml"`, which is packaged as a Qt plugin) leaves the default `qt`. The only other value that was ever accepted, `"provider"`, was removed: `logos-module-builder` now throws on it rather than silently generating no glue. `metadata.json#codegen.consumer_api_style` can override the derived answer in one direction only — a cdylib-packaged module may ask for `"qt"`; a Qt-plugin module asking for `"lp"` is refused.
2. `LogosModule.cmake` reads `${LOGOS_API_STYLE}` (default `qt`) and forwards `--api-style=${LOGOS_API_STYLE}` to the `logos-cpp-generator --general-only` invocation that writes the umbrella. Each module's Nix build emits **two** header derivations (`<name>.headers-qt` and `<name>.headers-lp`) via `buildHeaders.nix` — one `logos-cpp-generator --api-style=…` run per style, at the dep's build time. A consumer's `buildPlugin.nix` picks `dep.headers-${apiStyle}` and copies its `include/` straight into the build sandbox; no codegen runs at consume time. Nix's laziness means only the variant a downstream actually depends on is realised.
3. `parseApiStyleFlag()` in `generator_lib` parses `--api-style` once (rejecting the retired `std`); `main.cpp`'s `runUmbrellaMode` threads the resulting `ApiStyle` into `makeUmbrella*FromDeps`, and `plugin_introspect.cpp` threads it through `generateFromPlugin` (the QPluginLoader path). The directory-scraping `writeUmbrellaHeader`/`writeUmbrellaSource` pair that used to sit beside it is deleted — `makeUmbrella*FromDeps` is the only umbrella emitter now, so the two cannot drift. No per-style filenames are ever emitted; each module gets a single `<name>_api.h` + `<name>_api.cpp` pair regardless of style.

### Provider Generation — REMOVED

> The Qt provider glue emitter (`lidl_gen_provider.{h,cpp}` in logos-qt-sdk) is **deleted**. It
> wrapped a plain impl directly in a Qt provider object, skipping the language-neutral seam.
>
> A module is a plain shared library. Turning one into a Qt plugin is a downstream HOSTING step,
> and the two halves meet only at `logos_module_impl.h`:
>
> ```
> plain std impl
>   --> logos-cpp-generator --backend cdylib      -> logos_module_* C ABI exports
>   --> logos-qt-host-generator --backend cdylib  -> <name>CdylibProvider : LogosProviderBase
>       (logos-plugin-qt)
> ```
>
> That seam is what lets the Rust and JS providers target the same ABI. `logos-qt-generator` still
> owns `--backend consumer` (Qt-typed dependency wrappers) and `--backend ui` (view plugins) — and
> nothing else: **both** `--backend qt` and `--backend cdylib` were removed from it and are refused
> with a message naming the replacement. `cdylib` is the one that moved rather than died: the
> HOSTING half now lives with the host, as `logos-qt-host-generator --backend cdylib` in
> logos-plugin-qt.

### Impl Header Parser (`impl_header_parser.h/cpp`)

- `parseImplHeader(headerPath, className, metadataPath, err)` — parses C++ header + metadata.json into ModuleDecl
- State machine: `LookingForClass` → `InClass` → `InPublic`/`InPrivate`/`InLogosEvents`
- The literal `logos_events:` token (defined in `logos_module_context.h` as `#define logos_events public`) opens an events section; bare prototypes inside become `EventDecl{name, params, description}` entries appended to `ModuleDecl.events` (the `description` is the doc comment immediately above the declaration, captured via `joinDocLines` exactly as for methods)
- Skips: constructors, destructors, typedefs, using, friend, enum, struct, `std::function` declarations
- Recognizes `LogosMap` and `LogosList` return types (nlohmann::json aliases) and sets `MethodDecl.jsonReturn = true`
- Recognizes `std::optional<T>` → `?T` (see Optionality). Anything it does *not* recognize still falls back to the opaque `any`, silently — that fallback is why an optional was unexpressible header-first until it was named explicitly
- Template-aware parameter splitting (handles `std::vector<std::string>` correctly)

## CLI Usage

### From C++ impl header (primary use case for universal modules)

```bash
logos-cpp-generator --from-header src/my_module_impl.h \
    --backend cdylib \
    --metadata metadata.json \
    --output-dir ./generated_code
```

Generates the module-impl C ABI exports. Qt-plugin packaging is a separate step
(`logos-qt-host-generator --backend cdylib`).

### From LIDL file — cdylib glue

```bash
logos-cpp-generator --lidl my_module.lidl \
    --backend cdylib \
    --output-dir ./generated_code
```

### From LIDL file — client stubs

```bash
logos-cpp-generator --lidl my_module.lidl \
    --output-dir ./generated_code \
    --module-only
```

### Plugin-introspection and umbrella modes

```bash
logos-cpp-generator /path/to/plugin.so --output-dir ./generated
logos-cpp-generator --metadata metadata.json --umbrella --output-dir ./generated
```

Only the first line is legacy: it is the QPluginLoader path in
`plugin_introspect.cpp`. The umbrella is not — `LogosModuleContext::modules()`
returns `LogosModules&`, so every `interface: "universal"` module that calls a
declared dependency goes through it, and `LogosModule.cmake` runs it for every
module build. `--general-only` is an exact alias for `--umbrella` (it is what
`LogosModule.cmake`, `buildPlugin.nix` and `buildHeaders.nix` pass today), and
both route to the one implementation in `main.cpp`.

### Consumer wrapper with typed event accessors

The `--events-from <path>` flag points the `<plugin>.dylib` plugin-introspection codegen at a LIDL sidecar shipped alongside the dep's pre-built headers. When set, the generated `<name>_api.{h,cpp}` gains one typed `on<EventName>(callback)` accessor per declared event (callback arg types match `--api-style`):

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

The LIDL backends are tested in `tests/experimental/`, the shared `generator_lib` emitters in `tests/generator/`:

```bash
ws test logos-cpp-sdk      # runs all tests including experimental
```

The frontend tests (lexer/parser/validator/serializer) moved to the **logos-lidl** repo along with the code; only the C++/Qt-specific backends are tested here:

| Test file | What it tests |
|-----------|---------------|
| `test_lidl_type_mapping.cpp` | `lidlTypeToQt`, `lidlTypeToStd`, `lidlIsStdConvertible`, `lidlToPascalCase`, optionality on both surfaces |
| `test_lidl_gen_client.cpp` | Client stub generation: sync/async methods, events, metadata JSON, edge cases, both optional spellings agreeing |
| `test_lidl_gen_cdylib.cpp` | cdylib eligibility + emission: bytes at depth, records, typed maps, optionality (key omission, arity, `?any` collapse) |
| `test_impl_header_parser.cpp` | Header parsing: type mapping, access specifiers, skipping private/protected, error cases, `std::optional<T>` |

(The lexer/parser/AST/serializer/validator round-trip + description tests live in logos-lidl's own `tests/test_lidl.cpp`.)

In `tests/generator/`, alongside the wrapper-emitter tests:

| Test file | What it tests |
|-----------|---------------|
| `test_make_umbrella.cpp` | The `LogosModules` aggregate: both dependency forms on both API styles, that every member's type is included, dropped nameless entries, empty deps |

Fixture files in `tests/experimental/fixtures/`:
- `sample_impl.h` — module with all supported type variations
- `sample_metadata.json` — metadata with dependencies
- `object_deps_metadata.json` — dependencies declared in both forms, with resolution constraints
- `complex_impl.h` — module with multiple access specifier sections
- `empty_class_impl.h` — class with no public methods
- `empty_metadata.json` — minimal metadata
- `optional_impl.h` / `optional_metadata.json` — `std::optional<T>` header-first, incl. an optional over a declared record

## Known Limitations

- The impl header parser is lightweight (regex + state machine). It does not handle:
  - Multi-line method declarations
  - Default parameter values
  - Method definitions in the header (only declarations ending with `;`)
  - Nested classes
  - Template methods
  - `std::function` members are silently skipped (never treated as methods)
- LIDL does not support generic/parameterized types or inheritance
- `--from-header` emits the **cdylib** backend here (the `qt` glue backend moved to logos-plugin-qt's `logos-qt-host-generator --backend cdylib`, NOT to logos-qt-generator, which refuses both `qt` and `cdylib`); the **Rust** backend lives in logos-rust-sdk's `lidl-gen`, generating over logos-lidl's C ABI
- Client stub generation (`lidlMakeHeader`/`lidlMakeSource`) is only available from LIDL files, not from `--from-header`
- **Optionality is still untyped in *positional* slots on the legacy consumer path.** The
  consumer wrappers real modules get come from `main.cpp` →
  `generateInterfaceWrappers` → `lidl_to_json` → `generator_lib`, and that JSON boundary
  carries a single Qt **type-name string** per slot. Record *fields* now carry an
  `optional` flag alongside the value type, so both LIDL spellings emit identical, typed
  code (`QVariant` on Qt, `std::optional<T>` on Lp). A **method parameter, a return type
  and an event parameter still do not**: they arrive as `QVariant` (Qt) / `LogosMap` (Lp)
  — the right *shape* (an invalid QVariant / a JSON `null` is the empty inhabitant) with
  no *type*, so a caller gets no compile-time check and cannot tell `?tstr` from `?uint`
  or recover a `?Record`'s struct. There is no spelling divergence there — a positional
  slot has no name to hang a flag on, so it only ever had the type-kind form — and closing
  it changes generated method **signatures**, i.e. a source break for every existing call
  site. Until then the generator prints a `Note:` naming every still-flattened slot, so an
  affected build is never silent. `OptionalSpellings.PositionalSlotsAreStillFlattened`
  pins the current behaviour so closing the gap is a deliberate change.
- **Nesting, map key types and descriptions still do not cross that boundary either** —
  the flag added for optionality is per-field, not a general widening.
- `lidlRecordCollidesWithBytesTag` reads *through* an optional (via `fieldValueType`), so a
  single-`_bytes`-field record is refused under both spellings. It used to read `f.type`,
  which refused `? _bytes: tstr` and let `_bytes: ?tstr` through — the same declaration,
  two answers. `?bstr` is unaffected either way: the tag lives in the value, not the slot.
- **A provider REJECTION reaches `…Async`'s callback only as a log line** (but
  `…AsyncResult`'s callback gets it properly). A provider that refuses a call answers the
  canonical `{"code":"dispatch_failed", "message":…, "origin":…}` object as its RESULT, not
  as a transport error, and the Qt return table would convert it like any other value —
  erasing it (`_result.toList()` on that map is `[]`). The Qt consumer emitter therefore
  detects it (`logosDispatchRejection`, emitted once per wrapper) and folds it into the
  error channel of every surface that HAS one:
  - **sync** — the `logos::CallError*` out-parameter, so `mod.echoUintList(v, &err)` can
    tell a rejection from an empty return;
  - **`…AsyncResult`** — `logos::AsyncResult<T>::error`, so `r.ok()` is false and
    `r.error.code == "dispatch_failed"` exactly as on the sync path.

  The historical **`…Async`** overload is the one exception: its callback is
  `std::function<void(T)>`, and adding an error parameter would change a generated public
  surface (which logos-qt-sdk's `qt-generator --backend consumer` veneer mirrors 1:1). It
  is left untouched, so there an async rejection is reported with `qWarning` and the
  callback still receives the default-converted value. `…AsyncResult` exists precisely
  because giving async an error channel was an API addition rather than a code-generation
  fix — a caller that needs to SEE the rejection uses it.
