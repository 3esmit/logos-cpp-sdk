// logos::LpClient over STUBBED lp_* symbols — the two behaviours that are the
// wrapper's own rather than the transport's: when it creates its client, and how
// it decodes the C ABI's success/failure form.
//
// logos::LpClient creates its lp_client lazily, on whichever thread makes the
// first call through a generated `<dep>_api` wrapper. That thread is genuinely
// arbitrary and there can be more than one of it: a concurrency:"multi" module
// dispatches handlers on concurrent QThreads, and any module running a worker
// of its own (an HTTP handler, a chain-sync pump) races that worker against the
// dispatch thread on the very first call.
//
// Pre-fix, ensure() was `if (!m_client) m_client = lp_client_create(...)` — a
// data race on a plain pointer, with the losing racer's client leaked. This
// suite pins the replacement: construct outside any lock, publish with a CAS,
// and have the loser destroy its own client.
//
// The lp_* symbols below are LOCAL STUBS, not the real C ABI. sdk_tests
// deliberately does not link logos-protocol's library (see the CMakeLists),
// and the point here is the SDK-side logic, not the transport: the stubs let
// the test widen the create race, count exactly how many clients were built,
// published and destroyed, and drive lp_invoke_async through each of its
// documented outcomes — none of which the real library exposes.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "logos_lp_client.h"

namespace {

std::atomic<int> g_created{0};
std::atomic<int> g_destroyed{0};
// How many creates still have to fail before one is allowed to succeed. Models
// the deterministic-failure case (bad target/origin) the real
// lp_client_create() reports by returning NULL.
std::atomic<int> g_failNext{0};
// Widens the window between "created" and "published" so both racers really do
// build a client, rather than the test passing because one happened to win.
std::atomic<bool> g_slowCreate{false};

std::mutex g_seenMutex;
std::vector<lp_client*> g_seen;   // the client each getMethods() call observed
std::atomic<int> g_stringsFreed{0};
std::atomic<int> g_unsubscribed{0};

// How the next lp_invoke_async should behave. Named for the C ABI outcome each
// one models, not for the test that uses it.
enum class AsyncStub {
    Success,        // ok != 0, `json` is the result value
    FailWithError,  // ok == 0, `json` is the canonical {code, message, origin}
    FailMalformed,  // ok == 0, `json` is not a usable error object
    RefuseSync,     // returns LP_ERR_INVALID_ARG and does NOT call back
};
AsyncStub g_asyncStub = AsyncStub::Success;

void resetStubs() {
    g_created = 0;
    g_destroyed = 0;
    g_failNext = 0;
    g_slowCreate = false;
    g_asyncStub = AsyncStub::Success;
    g_stringsFreed = 0;
    g_unsubscribed = 0;
    std::lock_guard<std::mutex> lock(g_seenMutex);
    g_seen.clear();
}

}  // namespace

extern "C" {

lp_client* lp_client_create(const char*, const char*, const char*, const char*) {
    if (g_failNext.load() > 0 && g_failNext.fetch_sub(1) > 0) return nullptr;
    g_created.fetch_add(1, std::memory_order_relaxed);
    if (g_slowCreate.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // lp_client is opaque; any distinct heap address stands in for one.
    return reinterpret_cast<lp_client*>(new std::uintptr_t(0xC0FFEEu));
}

void lp_client_destroy(lp_client* client) {
    g_destroyed.fetch_add(1, std::memory_order_relaxed);
    delete reinterpret_cast<std::uintptr_t*>(client);
}

// The cheapest public LpClient method that goes through ensure().
//
// It returns a HEAP string the caller must hand back to lp_string_free, which
// is the real ABI contract — and stubbing it that way is load-bearing rather
// than cosmetic. This first returned NULL, which left getMethods()'s
// lp_string_free call unreachable: clang may inline this same-TU definition,
// prove the pointer null and delete the call, so a missing lp_string_free stub
// linked fine on macOS/clang and failed on GCC with an undefined reference.
// Returning a real allocation keeps that path live on every compiler.
char* lp_get_methods(lp_client* client) {
    {
        std::lock_guard<std::mutex> lock(g_seenMutex);
        g_seen.push_back(client);
    }
    char* out = static_cast<char*>(std::malloc(3));
    std::memcpy(out, "[]", 3);
    return out;
}

void lp_string_free(char* s) {
    g_stringsFreed.fetch_add(1, std::memory_order_relaxed);
    std::free(s);
}

int lp_invoke_async(lp_client*, const char*, const char*, int, lp_result_cb cb, void* ud) {
    switch (g_asyncStub) {
    case AsyncStub::Success:
        cb(1, "\"hi\"", ud);
        return LP_OK;
    case AsyncStub::FailWithError:
        cb(0, "{\"code\":\"object_unavailable\",\"message\":\"not loaded\",\"origin\":\"target\"}", ud);
        return LP_OK;
    case AsyncStub::FailMalformed:
        cb(0, "not json at all", ud);
        return LP_OK;
    case AsyncStub::RefuseSync:
        // The ABI's rule: a synchronous argument/handle rejection does NOT
        // call back.
        return LP_ERR_INVALID_ARG;
    }
    return LP_OK;
}

void lp_unsubscribe(lp_subscription*) { g_unsubscribed.fetch_add(1); }

}  // extern "C"

class LpClientEnsureTest : public ::testing::Test {
protected:
    void SetUp() override { resetStubs(); }

    static lp_client* soleSeenClient() {
        std::lock_guard<std::mutex> lock(g_seenMutex);
        return g_seen.empty() ? nullptr : g_seen.front();
    }
};

TEST_F(LpClientEnsureTest, RepeatedCallsOnOneThreadBuildExactlyOneClient) {
    {
        logos::LpClient client("target", "origin");
        for (int i = 0; i < 5; ++i) client.getMethods();
        EXPECT_EQ(g_created.load(), 1);
        EXPECT_EQ(g_destroyed.load(), 0);
        // Every string the ABI handed out went back through lp_string_free —
        // the ownership rule getMethods() has to honour, and the reason this
        // stub returns a real allocation rather than NULL.
        EXPECT_EQ(g_stringsFreed.load(), 5);
    }
    EXPECT_EQ(g_destroyed.load(), 1) << "the published client outlives every call, not the last one";
}

// The regression itself. Every thread must come away with the SAME client, and
// every client that was built but not published must have been destroyed —
// leaking one was the pre-fix behaviour whenever two threads first-called at
// once.
TEST_F(LpClientEnsureTest, ConcurrentFirstCallsPublishOneClientAndLeakNone) {
    constexpr int kThreads = 8;
    g_slowCreate = true;

    {
        logos::LpClient client("target", "origin");

        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&]() {
                ready.fetch_add(1);
                while (!go.load(std::memory_order_acquire)) { /* spin: no sleep, keep the start tight */ }
                client.getMethods();
            });
        }
        while (ready.load() < kThreads) { /* spin */ }
        go.store(true, std::memory_order_release);
        for (std::thread& t : threads) t.join();

        std::lock_guard<std::mutex> lock(g_seenMutex);
        ASSERT_EQ(static_cast<int>(g_seen.size()), kThreads);
        lp_client* published = g_seen.front();
        ASSERT_NE(published, nullptr);
        for (lp_client* seen : g_seen)
            EXPECT_EQ(seen, published) << "threads disagreed about which client is the module's";

        // Losers destroy their own before returning the published one, so by
        // the time every thread has joined the books already balance.
        EXPECT_EQ(g_destroyed.load(), g_created.load() - 1)
            << "created=" << g_created.load() << " destroyed=" << g_destroyed.load()
            << " — a client was built and neither published nor destroyed";
    }
    EXPECT_EQ(g_destroyed.load(), g_created.load()) << "the published client survived its owner";
}

// A create that fails is not cached: the real lp_client_create returns NULL for
// a bad target/origin, and latching that would turn one bad early call into a
// permanently dead dependency.
TEST_F(LpClientEnsureTest, AFailedCreateIsRetriedRatherThanLatched) {
    logos::LpClient client("target", "origin");

    g_failNext = 2;
    client.getMethods();
    client.getMethods();
    EXPECT_EQ(g_created.load(), 0);
    {
        std::lock_guard<std::mutex> lock(g_seenMutex);
        EXPECT_TRUE(g_seen.empty()) << "a NULL client must not reach lp_get_methods";
    }

    client.getMethods();
    EXPECT_EQ(g_created.load(), 1);
    EXPECT_NE(soleSeenClient(), nullptr);
}

// ─── invokeAsyncResult: the error-carrying async ────────────────────────────
//
// The generated `<name>AsyncResult` wrappers are built on this, so what it
// reports IS what the error channel reports. invokeAsync, next to it, has
// nowhere to put an error and collapses every failure into a bare JSON null —
// which is also a successful call that returned nothing. These pin the
// difference.

class LpClientAsyncResultTest : public LpClientEnsureTest {};

TEST_F(LpClientAsyncResultTest, SuccessDeliversTheValueAndAnOkError) {
    logos::LpClient client("target", "origin");
    g_asyncStub = AsyncStub::Success;

    int calls = 0;
    nlohmann::json got;
    logos::CallError err;
    err.code = "sentinel";   // must be overwritten, not merely left alone
    client.invokeAsyncResult("m", nlohmann::json::array(), [&](nlohmann::json r, const logos::CallError& e) {
        ++calls; got = std::move(r); err = e;
    });

    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(err.ok()) << err.code;
    EXPECT_EQ(got, nlohmann::json("hi"));
}

TEST_F(LpClientAsyncResultTest, FailureDeliversTheCanonicalErrorAndNoValue) {
    logos::LpClient client("target", "origin");
    g_asyncStub = AsyncStub::FailWithError;

    int calls = 0;
    nlohmann::json got = nlohmann::json("stale");
    logos::CallError err;
    client.invokeAsyncResult("m", nlohmann::json::array(), [&](nlohmann::json r, const logos::CallError& e) {
        ++calls; got = std::move(r); err = e;
    });

    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.code, "object_unavailable");
    EXPECT_EQ(err.message, "not loaded");
    EXPECT_EQ(err.origin, "target");
    // The error object is NOT handed back as if it were a value — that is the
    // distinction the plain invokeAsync path cannot make.
    EXPECT_TRUE(got.is_null());
}

TEST_F(LpClientAsyncResultTest, AMalformedErrorObjectStillReportsNotOk) {
    // Reporting ok() for a call the ABI said failed is the one outcome this
    // entry point exists to prevent, so an undecodable error body must not
    // degrade into success.
    logos::LpClient client("target", "origin");
    g_asyncStub = AsyncStub::FailMalformed;

    int calls = 0;
    logos::CallError err;
    client.invokeAsyncResult("m", nlohmann::json::array(), [&](nlohmann::json, const logos::CallError& e) {
        ++calls; err = e;
    });

    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.code, "call_failed");
}

TEST_F(LpClientAsyncResultTest, ASynchronousRefusalStillCompletesExactlyOnce) {
    // lp_invoke_async does NOT call back on LP_ERR_INVALID_ARG, so the
    // completion is the wrapper's to make. A caller schedules on "fires exactly
    // once"; silently never firing strands it.
    logos::LpClient client("target", "origin");
    g_asyncStub = AsyncStub::RefuseSync;

    int calls = 0;
    logos::CallError err;
    client.invokeAsyncResult("m", nlohmann::json::array(), [&](nlohmann::json, const logos::CallError& e) {
        ++calls; err = e;
    });

    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.code, "call_failed");
    EXPECT_EQ(err.origin, "target");
}

TEST_F(LpClientAsyncResultTest, AnUncreatableClientCompletesRatherThanHanging) {
    logos::LpClient client("target", "origin");
    g_failNext = 1000;   // every create fails

    int calls = 0;
    logos::CallError err;
    client.invokeAsyncResult("m", nlohmann::json::array(), [&](nlohmann::json, const logos::CallError& e) {
        ++calls; err = e;
    });

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(err.code, "object_unavailable");
    EXPECT_EQ(err.origin, "target");
}

TEST_F(LpClientAsyncResultTest, ANullCallbackIsANoOpRatherThanACall) {
    logos::LpClient client("target", "origin");
    client.invokeAsyncResult("m", nlohmann::json::array(), nullptr);
    EXPECT_EQ(g_created.load(), 0) << "a callback-less call must not even build a client";
}

// ── SubHandle's conversion ──────────────────────────────────────────────────

namespace {
template <typename T, typename = void>
struct IsEqualityComparable : std::false_type {};
template <typename T>
struct IsEqualityComparable<
    T, decltype(void(std::declval<const T&>() == std::declval<const T&>()))>
    : std::true_type {};
}  // namespace

// The generated `on<Event>()` returns a SubHandle where it used to return bool,
// so the conversion has to be IMPLICIT or every `bool ok = dep.onFoo(cb);`
// written against the old signature stops compiling. Deleting the comparison is
// the price of that: without it `a == b` would compare the two conversions
// rather than the handles, silently and with no diagnostic.
TEST(SubHandleTest, ConvertsToBoolImplicitlyButIsNotComparable) {
    static_assert(std::is_convertible<logos::SubHandle, bool>::value,
                  "on<Event>()'s return must convert to bool implicitly");
    static_assert(!IsEqualityComparable<logos::SubHandle>::value,
                  "SubHandle must not be comparable: == would compare two bools");

    g_unsubscribed = 0;
    logos::SubHandle empty;
    EXPECT_FALSE(empty);
    EXPECT_TRUE(empty.expired());
    EXPECT_FALSE(empty.cancel());
    EXPECT_EQ(g_unsubscribed.load(), 0)
        << "cancelling a handle that owns nothing still reached the C ABI";
}
