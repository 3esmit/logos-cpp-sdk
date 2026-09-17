# Calling a Module From a Worker Thread (HTTP Server)

Logos inter-module calls travel over Qt Remote Objects, whose replicas only
work on the thread that owns them — the module's main/event-loop thread. So a
module that wants to call **another** module from a *worker* thread has a
problem: the call would otherwise run on the worker thread and hang on replica
acquisition (there's no event loop there to drive it).

That worker-thread case is not exotic — it's exactly what you hit the moment a
module embeds a server. The motivating example is a module that serves an HTTP
`/metrics` endpoint and, on each scrape, calls other modules to gather their
numbers. The HTTP server runs on its own thread; the inter-module calls happen
there.

This doc-test proves that path works on the SDK commit under test, and that the
module stays **pure C++** — it never touches Qt to make it work. The SDK does
the marshaling: `LogosAPIClient` transparently runs `getClient` /
`invokeRemoteMethod` / `requestObject` / `onEvent` on the module's owner thread
when they're called from another thread.

It is fully self-contained:

1. Create `sensor_module`, a tiny **callee** with one method, `readTemperature()`.
2. Create `http_module`, a **caller** that depends on `sensor_module`, embeds a
   [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) server, and on
   every HTTP request calls `sensor_module.readTemperature()` **from the server
   thread** through the generated `modules().sensor_module` wrapper.
3. Build **both** against the C++ SDK commit under test, run them in
   `logoscore`, start the server, and `curl` it.

A green run means the `curl` got the sensor's reading back — i.e. the
worker-thread inter-module call completed instead of hanging. Without the SDK's
thread marshaling it would deadlock on the server thread and the `curl` would
time out.

**What you'll build:** Two modules — a `sensor_module` callee and an `http_module` caller that embeds an HTTP server — built against this SDK commit and run in `logoscore`, where an HTTP request drives a cross-module call from the server's worker thread.

**What you'll learn:**

- Why inter-module calls must run on the module's owner thread (Qt Remote Objects replica affinity)
- How the SDK lets a module call another module from a worker thread without touching Qt
- How to embed a third-party C library (libmicrohttpd, via pkg-config) in a universal module
- How to drive a cross-module call from an HTTP handler and scrape it with `curl`

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **git** — nix flakes only see files tracked by git.
- **curl** — to scrape the endpoint.
- A Linux or macOS machine.

---

## Step 1: Create the callee: sensor_module

`sensor_module` is an ordinary `core` module in the pure-C++ (`interface:
universal`) style with a single method. `http_module` will call it on every
HTTP request.

### 1.1 metadata.json

No dependencies; `interface: universal` selects the pure-C++ pattern.

```json
{
  "name": "sensor_module",
  "version": "1.0.0",
  "type": "core",
  "category": "general",
  "description": "A callee module: returns a temperature reading",
  "main": "sensor_module_plugin",
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

```cmake
cmake_minimum_required(VERSION 3.14)
project(SensorModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

logos_module(
    NAME sensor_module
    SOURCES
        src/sensor_module_impl.h
        src/sensor_module_impl.cpp
)
```

### 1.3 flake.nix

```nix
{
  description = "Sensor core module - a callee for the worker-thread doc-test";

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

### 1.4 src/sensor_module_impl.h — the class

A plain C++ class — no base, no Qt. Its one public method becomes callable over IPC.

```cpp
#pragma once

#include <cstdint>

// A trivial sensor. http_module calls readTemperature() on every HTTP
// request, from its server thread.
class SensorModuleImpl {
public:
    /// Returns the current temperature reading (degrees Celsius).
    int64_t readTemperature();
};
```

### 1.5 src/sensor_module_impl.cpp — the implementation

```cpp
#include "sensor_module_impl.h"

int64_t SensorModuleImpl::readTemperature()
{
    return 42;
}
```

---

## Step 2: Create the caller: http_module

`http_module` declares `sensor_module` as a dependency (so the builder
generates a typed `modules().sensor_module` wrapper) and embeds an HTTP
server using **libmicrohttpd**. The server runs on its own thread; its
request handler calls `sensor_module.readTemperature()` from there. The
module code never mentions Qt — the SDK marshals the cross-module call onto
the module's owner thread.

### 2.1 metadata.json — declare the dependency and the C library

`dependencies` lists `sensor_module`. `nix.packages.runtime` adds
`libmicrohttpd` (a build input, so the plugin can link it) and
`nix.packages.build` adds `pkg-config` so CMake can find it.

```json
{
  "name": "http_module",
  "version": "1.0.0",
  "type": "core",
  "category": "general",
  "description": "A caller module: serves HTTP and reads sensor_module from the server thread",
  "main": "http_module_plugin",
  "interface": "universal",
  "dependencies": ["sensor_module"],

  "nix": {
    "packages": {
      "build": ["pkg-config"],
      "runtime": ["libmicrohttpd"]
    },
    "external_libraries": [],
    "cmake": {
      "find_packages": [],
      "extra_sources": []
    }
  }
}
```

### 2.2 CMakeLists.txt — link libmicrohttpd

After `logos_module(...)`, find libmicrohttpd via pkg-config and link it into the generated plugin target (`<name>_module_plugin`).

```cmake
cmake_minimum_required(VERSION 3.14)
project(HttpModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

logos_module(
    NAME http_module
    SOURCES
        src/http_module_impl.h
        src/http_module_impl.cpp
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MHD REQUIRED IMPORTED_TARGET libmicrohttpd)
target_link_libraries(http_module_module_plugin PRIVATE PkgConfig::MHD)
```

### 2.3 flake.nix — add the dependency input

Declare `sensor_module` as a flake input (the input name **must match**
the dependency name). The `path:` value is a placeholder — we lock it to
the real sensor checkout in the build step.

```nix
{
  description = "HTTP core module - reads sensor_module from its server thread";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";

    # The module this one depends on. Placeholder path — locked to the
    # real checkout in the build step via --override-input.
    sensor_module.url = "path:/path/to/your/sensor_module";
  };

  outputs = inputs@{ logos-module-builder, sensor_module, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
```

### 2.4 src/http_module_impl.h — the class

Pure C++: `LogosModuleContext` (for `modules()`), a `std::mutex`, and an
opaque `void* m_daemon` (the libmicrohttpd handle stays out of the header
the generator parses). `start`/`stop` control the server; `readSensor`
does the cross-module call and is what the HTTP handler invokes.

```cpp
#pragma once

#include <cstdint>
#include <mutex>

#include <logos_module_context.h>  // LogosModuleContext base + modules()

// Serves HTTP via libmicrohttpd. On each request the server thread calls
// sensor_module through modules().sensor_module — the SDK marshals that
// call onto this module's owner thread. No Qt here.
class HttpModuleImpl : public LogosModuleContext {
public:
    HttpModuleImpl() = default;
    ~HttpModuleImpl();

    /// Start the HTTP server on `port`. Returns 1 on success, 0 on
    /// failure (already running / bad port / bind error).
    int64_t start(int64_t port);

    /// Stop the HTTP server. Returns 1 if it was running, 0 otherwise.
    int64_t stop();

    /// Read sensor_module.readTemperature(). The HTTP handler calls this
    /// from the server (worker) thread; exposed as a method so it can
    /// also be driven directly for comparison.
    int64_t readSensor();

private:
    std::mutex m_mutex;
    void* m_daemon = nullptr;  // struct MHD_Daemon*
};
```

### 2.5 src/http_module_impl.cpp — the implementation

The handler runs on a libmicrohttpd worker thread and calls
`self->readSensor()`, which goes through `modules().sensor_module`. That
cross-module call is what the SDK marshals onto the owner thread — the
whole point of this doc-test.

```cpp
#include "http_module_impl.h"

#include <cstdint>
#include <string>

#include <microhttpd.h>

// Generated at build time: defines LogosModules with the typed
// modules().sensor_module accessor. Included only in the .cpp so the impl
// header the generator parses stays free of Qt / codegen types.
#include "logos_sdk.h"

namespace {

// libmicrohttpd access handler. `cls` is the HttpModuleImpl*. Runs on an
// MHD worker thread; the cross-module call inside is marshaled onto the
// module's owner thread by the SDK.
MHD_Result onRequest(void* cls, struct MHD_Connection* connection,
                     const char* /*url*/, const char* /*method*/,
                     const char* /*version*/, const char* /*upload_data*/,
                     size_t* /*upload_data_size*/, void** /*req_cls*/)
{
    auto* self = static_cast<HttpModuleImpl*>(cls);
    const std::string body =
        "temperature " + std::to_string(self->readSensor()) + "\n";

    MHD_Response* response = MHD_create_response_from_buffer(
        body.size(), const_cast<char*>(body.data()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "text/plain; charset=utf-8");
    MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

}  // namespace

HttpModuleImpl::~HttpModuleImpl()
{
    stop();
}

int64_t HttpModuleImpl::readSensor()
{
    // Cross-module call. From the HTTP handler this runs on the server's
    // worker thread; the SDK marshals it onto this module's owner thread.
    return modules().sensor_module.readTemperature();
}

int64_t HttpModuleImpl::start(int64_t port)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_daemon) return 0;
    if (port <= 0 || port > 65535) return 0;

    MHD_Daemon* daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, static_cast<uint16_t>(port),
        nullptr, nullptr, &onRequest, this, MHD_OPTION_END);
    if (!daemon) return 0;

    m_daemon = daemon;
    return 1;
}

int64_t HttpModuleImpl::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_daemon) return 0;
    MHD_stop_daemon(static_cast<MHD_Daemon*>(m_daemon));
    m_daemon = nullptr;
    return 1;
}
```

---

## Step 3: Build both modules against this SDK

Nix flakes only see git-tracked files, so initialise a repo in each module
first, then build each `.lgx`, overriding `logos-cpp-sdk` to the commit
under test.

> The override URLs carry a `` placeholder the runner expands to a
> concrete ref — locally this checkout's `HEAD`, in CI the commit being
> tested.

### 3.1 Initialise git repos

```bash
(cd sensor_module && git init -q && git add -A)
(cd http_module && git init -q && git add -A)

```

### 3.2 Build the sensor's .lgx against this SDK

```bash
nix build 'path:./sensor_module#lgx' \
  --override-input logos-module-builder/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input logos-module-builder/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input logos-module-builder/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  -o sensor-lgx
```

The sensor package is under `./sensor-lgx/`:

```bash
ls sensor-lgx/*.lgx
```

### 3.3 Build the http module's .lgx against this SDK

Lock `sensor_module` to the local checkout and override `logos-cpp-sdk`
in both builders, so the dependency wrapper and both plugins are built
against one consistent SDK.

```bash
nix build 'path:./http_module#lgx' \
  --override-input sensor_module 'path:./sensor_module' \
  --override-input logos-module-builder/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input logos-module-builder/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input logos-module-builder/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  --override-input sensor_module/logos-module-builder/logos-cpp-sdk 'github:logos-co/logos-cpp-sdk' \
  --override-input sensor_module/logos-module-builder/logos-qt-sdk 'github:logos-co/logos-qt-sdk' \
  --override-input sensor_module/logos-module-builder/logos-qt-sdk/logos-lidl 'github:logos-co/logos-lidl' \
  -o http-lgx
```

The http package is under `./http-lgx/`:

```bash
ls http-lgx/*.lgx
```

---

## Step 4: Build the runtime and install both modules

Build `logoscore` and `lgpm` (against this SDK), seed the modules directory
with the capability module, and install both modules.

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

```bash
mkdir -p modules
cp -RL ./logos/modules/. ./modules/

```

### 4.4 Install the sensor

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file sensor-lgx/*.lgx
```

### 4.5 Install the http module

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file http-lgx/*.lgx
```

### 4.6 Confirm both modules are installed

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

---

## Step 5: Serve over HTTP and scrape from the worker thread

Start the daemon, load both modules, then start the HTTP server and `curl`
it. The `curl` triggers a request whose handler — on the server's worker
thread — calls `sensor_module.readTemperature()`. Getting `temperature 42`
back is the proof that the worker-thread cross-module call completed.

### 5.1 Start the daemon

```bash
logoscore -D -m ./modules > logs.txt &
```

```bash
sleep 3
```

### 5.2 Load the sensor (the dependency first)

```bash
logoscore load-module sensor_module
```

### 5.3 Load the http module

```bash
logoscore load-module http_module
```

### 5.4 Read the sensor directly (main thread)

Called via `logoscore`, `readSensor()` runs on the module's own event-loop
thread — the easy case. It returns the sensor's reading:

```bash
logoscore call http_module readSensor
```

### 5.5 Start the HTTP server

```bash
logoscore call http_module start 8080
```

```bash
sleep 1
```

### 5.6 Scrape it — the cross-module call now happens on the server thread

The HTTP handler runs on a libmicrohttpd worker thread and calls
`sensor_module.readTemperature()` from there. The SDK marshals that call
onto the module's owner thread, so it completes and the response carries
the sensor's reading. Without the marshaling this request would hang.

```bash
curl http://127.0.0.1:8080/
```

### 5.7 Stop the HTTP server

```bash
logoscore call http_module stop
```

### 5.8 Stop the daemon

```bash
logoscore stop
```

```bash
sleep 2
```

### 5.9 Confirm the daemon has stopped

```bash
logoscore status
```

---

## Recap

| Call site | Thread | Result |
| --------- | ------ | ------ |
| `logoscore call http_module readSensor` | module event-loop thread | `42` |
| `curl http://127.0.0.1:8080/` → HTTP handler → `readSensor()` | libmicrohttpd worker thread | `temperature 42` |

Both reach `sensor_module.readTemperature()` through the generated
`modules().sensor_module` wrapper. The second does it from a worker thread —
and it works because the SDK marshals the call onto the module's owner
thread, where Qt Remote Objects replicas live. The module itself stays pure
C++. A green run is evidence that worker-thread inter-module calls work on
this SDK commit.
