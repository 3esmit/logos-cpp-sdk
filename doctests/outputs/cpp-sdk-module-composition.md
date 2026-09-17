# Composing Two Modules Built Against This C++ SDK

The whole point of `logos-cpp-sdk` is that a module can call **another**
module without ever touching the raw `LogosAPI`: you declare a dependency,
and the SDK's code generator emits a typed `modules().<dep>` wrapper with
synchronous callers, asynchronous callers, and event subscribers. This
doc-test proves that cross-module path works end-to-end on the SDK commit
under test — by building *both* sides from scratch against it.

It is fully self-contained — no pre-existing module, no `requires:` chain:

1. Create `greeter_module`, a small **callee** with a couple of methods
   (`greet`, `addInts`, `greetCount`) and two events — `greeted`, carrying a
   string, and `blobReady`, carrying raw bytes.
2. Create `orchestrator_module`, a **caller** that declares `greeter_module`
   as a dependency and composes it through the generated
   `modules().greeter_module` wrapper — synchronously, asynchronously, and by
   subscribing to both of its events.
3. Build **both** modules' `.lgx` packages **against the C++ SDK commit under
   test**, so the generated wrappers, the plugin glue, and the IPC layer all
   come from this SDK.
4. Build `logoscore` (also against this SDK), install both modules, load them
   together, and call the orchestrator's methods — each of which calls into
   the greeter across the process boundary.

Because the caller, the callee, the generated cross-module wrappers, and the
runtime are all built from the SDK commit under test, a green run is real
evidence that this change keeps inter-module composition — the SDK's core
promise — working.

**What you'll build:** Two modules — a `greeter_module` callee and an `orchestrator_module` caller — built against this SDK commit and run together in `logoscore`, with the caller driving the callee over IPC.

**What you'll learn:**

- How one module declares another as a dependency (`metadata.json` + `flake.nix` input)
- How the SDK generates a typed `modules().<dep>` wrapper with sync, async, and event APIs
- How to build a module — and its module dependency — against a specific `logos-cpp-sdk` commit
- How to load two modules in `logoscore` and chain calls so the caller drives the callee
- How async replies and event subscriptions survive between `call` commands under the daemon
- How a **binary** (`bstr`) event payload crosses the boundary intact, encoded and decoded by generated code on both sides

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **git** — nix flakes only see files tracked by git.
- A Linux or macOS machine.

---

## Step 1: Create the callee: greeter_module

`greeter_module` is an ordinary `core` module written in the pure-C++
(`interface: universal`) style: you write one plain class, and the builder
generates the Qt plugin glue. Every `public` method becomes callable over
IPC, and the `logos_events:` block declares an event other modules can
subscribe to.

### 1.1 metadata.json

`dependencies` is empty — the greeter calls no one. `interface:
universal` selects the pure-C++ pattern.

```json
{
  "name": "greeter_module",
  "version": "1.0.0",
  "type": "core",
  "category": "general",
  "description": "A callee module: greets, counts greetings, and emits an event",
  "main": "greeter_module_plugin",
  "interface": "universal",
  "dependencies": [],

  "nix": {
    "packages": {
      "build": [],
      "runtime": []
    },
    "external_libraries": [],
    "cmake": {
      "find_packages": [],
      "extra_sources": []
    }
  }
}
```

### 1.2 CMakeLists.txt

For a universal module you list only your plain C++ sources; the generated glue is compiled automatically.

```cmake
cmake_minimum_required(VERSION 3.14)
project(GreeterModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

logos_module(
    NAME greeter_module
    SOURCES
        src/greeter_module_impl.h
        src/greeter_module_impl.cpp
)
```

### 1.3 flake.nix

A minimal flake that hands every input to `mkLogosModule`. The
`` on the builder URL is pinned by the doc-test runner; the
builder owns the module's `logos-cpp-sdk` pin, which we override to the
commit under test in the build step.

```nix
{
  description = "Greeter core module - a callee for the composition doc-test";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
```

### 1.4 src/greeter_module_impl.h — the class

Plain C++ inheriting `LogosModuleContext`. The `///` doc comments
become each method's description; the `logos_events:` block declares
the `greeted` event (the token expands to `public` under a normal
compile, and the generator emits the event body).

```cpp
#pragma once

#include <cstdint>
#include <string>

#include <logos_module_context.h>  // LogosModuleContext base + logos_events

// A simple callee module. The orchestrator composes these methods over
// IPC. It also emits a `greeted` event so the caller can exercise a
// typed event subscription.
class GreeterModuleImpl : public LogosModuleContext {
public:
    GreeterModuleImpl() = default;
    ~GreeterModuleImpl() = default;

    /// Returns a greeting for the given name.
    std::string greet(const std::string& name);

    /// Adds two integers and returns the sum.
    int64_t addInts(int64_t a, int64_t b);

    /// Returns how many times greet() has been called on this instance.
    int64_t greetCount() const;

    /// Greets the name and also emits a `greeted` event carrying it.
    void greetNotify(const std::string& name);

    /// Builds a blob of `size` bytes and emits it on `blobReady`.
    /// Returns the number of bytes emitted.
    int64_t emitBlob(int64_t size);

logos_events:
    /// Emitted by greetNotify() with the produced greeting string.
    void greeted(const std::string& greeting);

    /// Emitted by emitBlob() carrying raw bytes. `bstr` payloads take
    /// the canonical tagged form on the wire; the generated code on
    /// both sides encodes and decodes them, so the author on either
    /// end only ever sees a `std::vector<uint8_t>`.
    void blobReady(const std::string& label,
                   const std::vector<uint8_t>& payload);

private:
    int64_t m_greetCount = 0;
};
```

### 1.5 src/greeter_module_impl.cpp — the implementation

Plain C++ — no Qt, no IPC plumbing. `greetNotify` fires the generated event.

```cpp
#include "greeter_module_impl.h"

std::string GreeterModuleImpl::greet(const std::string& name)
{
    ++m_greetCount;
    return "Hello, " + name + "!";
}

int64_t GreeterModuleImpl::addInts(int64_t a, int64_t b)
{
    return a + b;
}

int64_t GreeterModuleImpl::greetCount() const
{
    return m_greetCount;
}

void GreeterModuleImpl::greetNotify(const std::string& name)
{
    // Emit the event declared in logos_events:. When loaded by a host
    // this reaches every subscriber; constructed outside a host it is a
    // safe no-op.
    greeted("Hello, " + name + "!");
}

int64_t GreeterModuleImpl::emitBlob(int64_t size)
{
    // A deterministic blob the subscriber can check byte-for-byte.
    // It deliberately contains 0x00 and bytes >= 0x80 — the values a
    // text encoding would mangle.
    std::vector<uint8_t> payload;
    payload.reserve(static_cast<size_t>(size));
    for (int64_t i = 0; i < size; ++i)
        payload.push_back(static_cast<uint8_t>((i * 7 + 11) & 0xff));

    blobReady("blob", payload);
    return static_cast<int64_t>(payload.size());
}
```

---

## Step 2: Create the caller: orchestrator_module

`orchestrator_module` declares `greeter_module` as a dependency. That one
line in `metadata.json` is what makes the builder run the SDK's code
generator over the greeter's exported interface and emit a typed
`modules().greeter_module` wrapper — with sync callers, async callers, and
event subscribers — that the orchestrator uses without any raw `LogosAPI`.

### 2.1 metadata.json — declare the dependency

The `dependencies` entry must match `greeter_module`'s own
`metadata.json` `name`.

```json
{
  "name": "orchestrator_module",
  "version": "1.0.0",
  "type": "core",
  "category": "general",
  "description": "A caller module: composes greeter_module via typed sync/async calls and an event subscription",
  "main": "orchestrator_module_plugin",
  "interface": "universal",
  "dependencies": ["greeter_module"],

  "nix": {
    "packages": {
      "build": [],
      "runtime": []
    },
    "external_libraries": [],
    "cmake": {
      "find_packages": [],
      "extra_sources": []
    }
  }
}
```

### 2.2 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(OrchestratorModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

logos_module(
    NAME orchestrator_module
    SOURCES
        src/orchestrator_module_impl.h
        src/orchestrator_module_impl.cpp
)
```

### 2.3 flake.nix — add the dependency input

Declare `greeter_module` as a flake input; the input name **must
match** the dependency name in `metadata.json`. The `path:` value is a
placeholder — we lock it to the real greeter checkout in the build step
with `--override-input` (Nix won't accept a relative `../` path written
directly into `flake.nix`).

```nix
{
  description = "Orchestrator core module - calls greeter_module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";

    # The module this one depends on. Placeholder path — locked to the
    # real checkout in the build step via --override-input.
    greeter_module.url = "path:/path/to/your/greeter_module";
  };

  outputs = inputs@{ logos-module-builder, greeter_module, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
```

### 2.4 src/orchestrator_module_impl.h — the class

Three composition paths, all through `modules().greeter_module`:
`greetThrough`/`greetReport` (sync), `startAsyncGreet`/`asyncGreeting`
(async), and `subscribeGreeted`/`lastGreetedEvent` (event).

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <logos_json.h>            // LogosMap
#include <logos_module_context.h>  // LogosModuleContext base + modules()

// A caller module. It does nothing on its own — it composes
// greeter_module through the typed modules().greeter_module wrapper the
// builder generates from the dependency declared in metadata.json.
class OrchestratorModuleImpl : public LogosModuleContext {
public:
    OrchestratorModuleImpl() = default;
    ~OrchestratorModuleImpl() = default;

    /// Calls greeter_module.greet(name) and returns its result verbatim.
    std::string greetThrough(const std::string& name);

    /// Composes several greeter_module calls into one map: a greeting,
    /// an integer sum, and the greeter's current greet count.
    LogosMap greetReport(const std::string& name, int64_t a, int64_t b);

    /// Fires greeter_module.greetAsync(name) asynchronously and returns
    /// "queued" immediately; the reply lands in a callback. Read it back
    /// with asyncGreeting().
    std::string startAsyncGreet(const std::string& name);

    /// The greeting delivered by startAsyncGreet()'s callback, or empty
    /// until it arrives.
    std::string asyncGreeting() const;

    /// Subscribes to greeter_module's `greeted` event with a typed
    /// callback. Returns "ok" once registered.
    std::string subscribeGreeted();

    /// The last greeting captured by the `greeted` subscription, or
    /// empty until the event fires.
    std::string lastGreetedEvent() const;

    /// Subscribes to greeter_module's `blobReady` event — the binary
    /// one. Returns "ok" once registered.
    std::string subscribeBlob();

    /// How many bytes the `blobReady` subscription actually received,
    /// or -1 until the event fires.
    int64_t lastBlobSize() const;

    /// A checksum over those bytes — proves the payload arrived
    /// intact, not merely with the right length.
    int64_t lastBlobChecksum() const;

private:
    std::string m_asyncGreeting;
    std::string m_lastGreetedEvent;
    bool        m_subscribed = false;
    bool        m_blobSubscribed = false;
    int64_t     m_lastBlobSize = -1;
    int64_t     m_lastBlobChecksum = -1;
};
```

### 2.5 src/orchestrator_module_impl.cpp — the implementation

The `.cpp` includes the generated `logos_sdk.h` (which defines
`LogosModules`) — that's why the cross-module calls live here and not in
the header the generator parses. Each method drives `greeter_module`
through the generated wrapper: `.greet(...)` (sync), `.greetAsync(...,
cb)` (async), `.onGreeted(cb)` (event).

```cpp
#include "orchestrator_module_impl.h"

// Generated at build time by logos-cpp-generator. Defines LogosModules
// with one std-typed accessor per metadata.json dependency — here
// greeter_module. Included only in the .cpp so the impl header the
// generator parses stays free of Qt and codegen types.
#include "logos_sdk.h"

std::string OrchestratorModuleImpl::greetThrough(const std::string& name)
{
    // The simplest cross-module round-trip: one typed sync call.
    return modules().greeter_module.greet(name);
}

LogosMap OrchestratorModuleImpl::greetReport(const std::string& name,
                                             int64_t a, int64_t b)
{
    // Three typed sync calls into greeter_module, composed into one map.
    auto& greeter = modules().greeter_module;
    LogosMap report;
    report["greeting"]   = greeter.greet(name);
    report["sum"]        = greeter.addInts(a, b);
    report["greetCount"] = greeter.greetCount();
    return report;
}

std::string OrchestratorModuleImpl::startAsyncGreet(const std::string& name)
{
    // The generated async overload returns immediately; the reply is
    // delivered to the callback on this module's event loop.
    modules().greeter_module.greetAsync(name, [this](const std::string& g) {
        m_asyncGreeting = g;
    });
    return "queued";
}

std::string OrchestratorModuleImpl::asyncGreeting() const
{
    return m_asyncGreeting;
}

std::string OrchestratorModuleImpl::subscribeGreeted()
{
    if (m_subscribed) return "ok";
    // Typed subscriber generated from greeter_module's logos_events:
    // greeted(const std::string&). The accessor is `on` + the
    // capitalized event name.
    m_subscribed = modules().greeter_module.onGreeted(
        [this](const std::string& greeting) {
            m_lastGreetedEvent = greeting;
        });
    return m_subscribed ? "ok" : "failed";
}

std::string OrchestratorModuleImpl::lastGreetedEvent() const
{
    return m_lastGreetedEvent;
}

std::string OrchestratorModuleImpl::subscribeBlob()
{
    if (m_blobSubscribed) return "ok";
    // The binary event. The callback takes real bytes: the tagged
    // wire form is encoded by the greeter's generated event body and
    // decoded by this generated subscriber, so neither author writes
    // any base64.
    m_blobSubscribed = modules().greeter_module.onBlobReady(
        [this](const std::string& label,
               const std::vector<uint8_t>& payload) {
            (void)label;
            m_lastBlobSize = static_cast<int64_t>(payload.size());
            int64_t sum = 0;
            for (size_t i = 0; i < payload.size(); ++i)
                sum += static_cast<int64_t>(payload[i])
                     * static_cast<int64_t>(i % 31 + 1);
            m_lastBlobChecksum = sum;
        });
    return m_blobSubscribed ? "ok" : "failed";
}

int64_t OrchestratorModuleImpl::lastBlobSize() const
{
    return m_lastBlobSize;
}

int64_t OrchestratorModuleImpl::lastBlobChecksum() const
{
    return m_lastBlobChecksum;
}
```

---

## Step 3: Build both modules against this SDK

Nix flakes only see files tracked by git, so initialise a repo in each
module first. Then build each module's `.lgx`, overriding `logos-cpp-sdk`
to the commit under test so the generated wrappers, plugin glue, and IPC
come from this SDK.

> Each override URL carries a `` placeholder the doc-test runner
> expands to a concrete ref: locally that is this `logos-cpp-sdk`
> checkout's `HEAD` (see `run.sh`); in CI it is the commit being tested.
> With no pin it falls back to latest `master`.

### 3.1 Initialise git repos

The greeter is a dependency of the orchestrator, so both must be tracked.

```bash
(cd greeter_module && git init -q && git add -A)
(cd orchestrator_module && git init -q && git add -A)

```

### 3.2 Build the greeter's .lgx against this SDK

The greeter has no module dependency. Its builder's `logos-cpp-sdk`,
Qt SDK, and both Qt plugin-backend aliases still move together: the
Qt-side generators parse the interface emitted by the SDK under test.

```bash
# From inside the greeter clone this is simply:
#   nix build '.#lgx' --override-input logos-module-builder/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk'
nix build 'path:./greeter_module#lgx' \
  --override-input logos-module-builder/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input logos-module-builder/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input logos-module-builder/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input logos-module-builder/logos-plugin-qt 'github:logos-co/logos-plugin-qt' \
  --override-input logos-module-builder/logos-plugin-qt/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input logos-module-builder/logos-plugin-core 'github:logos-co/logos-plugin-qt' \
  --override-input logos-module-builder/logos-plugin-core/logos-lidl 'github:logos-co/logos-lidl' \
  -o greeter-lgx
```

The greeter package is under `./greeter-lgx/`:

```bash
ls greeter-lgx/*.lgx
```

### 3.3 Build the orchestrator's .lgx against this SDK

The orchestrator pulls in `greeter_module` as a dependency, so we lock
that input to the local greeter checkout **and** override
the SDK and Qt-side generator inputs in both the orchestrator's builder
and the greeter's builder — so the dependency wrapper the generator
emits, and both plugins, use one consistent LIDL grammar.

```bash
nix build 'path:./orchestrator_module#lgx' \
  --override-input greeter_module 'path:./greeter_module' \
  --override-input logos-module-builder/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input logos-module-builder/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input logos-module-builder/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input logos-module-builder/logos-plugin-qt 'github:logos-co/logos-plugin-qt' \
  --override-input logos-module-builder/logos-plugin-qt/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input logos-module-builder/logos-plugin-core 'github:logos-co/logos-plugin-qt' \
  --override-input logos-module-builder/logos-plugin-core/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input greeter_module/logos-module-builder/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input greeter_module/logos-module-builder/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input greeter_module/logos-module-builder/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input greeter_module/logos-module-builder/logos-plugin-qt 'github:logos-co/logos-plugin-qt' \
  --override-input greeter_module/logos-module-builder/logos-plugin-qt/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input greeter_module/logos-module-builder/logos-plugin-core 'github:logos-co/logos-plugin-qt' \
  --override-input greeter_module/logos-module-builder/logos-plugin-core/logos-lidl 'github:logos-co/logos-lidl' \
  -o orchestrator-lgx
```

The orchestrator package is under `./orchestrator-lgx/`:

```bash
ls orchestrator-lgx/*.lgx
```

---

## Step 4: Build the runtime and install both modules

Build `logoscore` (against this SDK, the same way as the runtime doc-test)
and `lgpm`, then install both modules into a `./modules` directory the
daemon can scan.

### 4.1 Build logoscore against this SDK

```bash
nix build 'github:logos-co/logos-logoscore-cli' \
  --override-input logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input logos-liblogos/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input logos-liblogos/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input logos-liblogos/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input logos-capability-module/logos-module-builder/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input logos-capability-module/logos-module-builder/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input logos-capability-module/logos-module-builder/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input logos-capability-module/logos-module-builder/logos-test-framework/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input logos-capability-module/logos-module-builder/logos-test-framework/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  --out-link ./logos
```

### 4.2 Build lgpm

```bash
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm
```

### 4.3 Seed the modules directory with the capability module

Loading a module goes through the host's capability layer, so the
modules directory needs the `capability_module` that ships with
`logoscore` (rebuilt against this SDK). Copy it across first.

```bash
mkdir -p modules
cp -RL ./logos/modules/. ./modules/

```

### 4.4 Install the greeter

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file greeter-lgx/*.lgx
```

### 4.5 Install the orchestrator

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file orchestrator-lgx/*.lgx
```

### 4.6 Confirm both modules are installed

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

---

## Step 5: Load both modules and drive the caller

Start `logoscore` in daemon mode (`-D`) — it keeps each module's process
alive between `call` commands, so an event subscription registered by one
call is still active when a later call triggers it, and an async reply
lands before the call that reads it. Load **both** modules, then call the
orchestrator's methods — each one reaches across the process boundary into
the greeter.

### 5.1 Start the daemon

```bash
logoscore -D -m ./modules > logs.txt &
```

```bash
sleep 3
```

### 5.2 Load the greeter (the dependency first)

```bash
logoscore load-module greeter_module
```

### 5.3 Load the orchestrator

```bash
logoscore load-module orchestrator_module
```

### 5.4 Confirm both report loaded

```bash
logoscore status
```

### 5.5 Synchronous cross-module call

`greetThrough(name)` forwards straight to `greeter_module.greet(name)`
and returns the result — one typed sync call across the process
boundary:

```bash
logoscore call orchestrator_module greetThrough World
```

### 5.6 Compose several calls into one result

`greetReport(name, a, b)` fans out to three greeter calls — `greet`,
`addInts`, and `greetCount` — and returns them as one map. The greeter
has now been greeted twice (once above, once here), so `greetCount` is
`2`:

```bash
logoscore call orchestrator_module greetReport Logos 3 5
```

### 5.7 Asynchronous cross-module call

`startAsyncGreet(name)` fires `greeter_module.greetAsync(name)` and
returns `"queued"` immediately. The reply arrives on the daemon's event
loop; the next call, `asyncGreeting()`, reads what the callback stashed:

```bash
logoscore call orchestrator_module startAsyncGreet Async
```

```bash
sleep 1
```

### 5.8 Read the async reply

```bash
logoscore call orchestrator_module asyncGreeting
```

### 5.9 Subscribe to the greeter's event

`subscribeGreeted()` registers a typed callback on `greeter_module`'s
`greeted` event. Then `greeter_module.greetNotify(...)` makes the
greeter emit it, and `lastGreetedEvent()` reads what the subscription
captured. Because the daemon keeps both modules loaded, the event fires
between the calls:

```bash
logoscore call orchestrator_module subscribeGreeted
```

### 5.10 Trigger the event from the greeter

```bash
logoscore call greeter_module greetNotify Events
```

```bash
sleep 1
```

### 5.11 Read the captured event payload

```bash
logoscore call orchestrator_module lastGreetedEvent
```

### 5.12 Subscribe to the greeter's binary event

The same flow, but the event carries a **byte string** (`bstr`) rather
than text. Binary payloads cannot ride in a JSON string — a NUL would
truncate them and any byte above 0x7f would be mangled — so they travel
in the canonical tagged form `{"_bytes": "<base64url>"}`. Both halves of
that are generated: the greeter's event body encodes, this subscriber
decodes, and neither author writes a line of base64.

```bash
logoscore call orchestrator_module subscribeBlob
```

### 5.13 Emit 4096 bytes from the greeter

The greeter reports how many bytes it put on the wire.

```bash
logoscore call greeter_module emitBlob 4096
```

```bash
sleep 1
```

### 5.14 The subscriber received every byte

`lastBlobSize` is the length the subscription actually saw. This is the
assertion that pins [#99](https://github.com/logos-co/logos-cpp-sdk/issues/99),
where the generator dropped `bstr` event arguments: the greeter emitted
a full payload and the subscriber received `0` bytes.

```bash
logoscore call orchestrator_module lastBlobSize
```

### 5.15 ...and the bytes are the right bytes

Length alone would not catch a corrupted payload — a wrong base64
alphabet or a botched tail group round-trips to the same size. The
checksum is computed over the received bytes and must match the blob the
greeter built.

```bash
logoscore call orchestrator_module lastBlobChecksum
```

### 5.16 Stop the daemon

```bash
logoscore stop
```

```bash
sleep 2
```

### 5.17 Confirm the daemon has stopped

```bash
logoscore status
```

---

## Recap

| Composition path | In the orchestrator | Driven via `logoscore` |
| ---------------- | ------------------- | ---------------------- |
| Typed **sync** call | `greetThrough()` → `greeter.greet(...)` | `"Hello, World!"` |
| Composed sync calls | `greetReport()` → `greet` + `addInts` + `greetCount` | one map of three results |
| Typed **async** call | `startAsyncGreet()` → `greetAsync(..., cb)` | `queued`, then `"Hello, Async!"` |
| Typed **event** subscription | `subscribeGreeted()` → `onGreeted(cb)` | captured `"Hello, Events!"` |
| **Binary** event payload | `subscribeBlob()` → `onBlobReady(cb)` | all 4096 bytes, checksum intact |

Every path went through `modules().greeter_module`, the wrapper the SDK's
code generator emitted from the `greeter_module` dependency — and both
modules, the wrapper, and the runtime were built against the SDK commit
under test. A green run means inter-module composition still works on this
SDK, end to end.

The binary row is the one with teeth. Bytes are the only payload that
cannot ride in a JSON string, so they are the only one that needs an
encoder on the emitting side and a decoder on the receiving side — two
pieces of generated code that must agree exactly. When they did not
([#99](https://github.com/logos-co/logos-cpp-sdk/issues/99)), everything
above still passed: the module emitted a full payload, the transport
carried it, and the subscriber received zero bytes. Asserting on the
*length and the contents* of what actually arrived is what catches that.
