#ifndef LOGOS_ASYNC_RESULT_H
#define LOGOS_ASYNC_RESULT_H

// The async counterpart of logos::CallError's sync out-parameter.
//
// The sync generated wrapper takes an optional `logos::CallError*` precisely
// "to distinguish a failed remote call from a legitimately default-valued
// result". The async wrapper had no such channel: `fooAsync` hands the callback
// a bare T, and a failed call is indistinguishable from a provider that
// legitimately returned 0 / "" / false. AsyncResult<T> is that missing channel
// — the value and the error travel together, so `fooAsyncResult`'s callback can
// check `.ok()` before trusting `.value`.
//
// This mirrors logos-rust-sdk, where BOTH surfaces already carry the error
// (lidl-gen/src/rustgen.rs: `-> Result<T, LogosError>` on the sync wrapper and
// `FnOnce(Result<T, LogosError>)` on the async one). C++ was the outlier.
//
// Deliberately Qt-FREE, exactly like logos_call_error.h next to which it
// conceptually lives: it is named in the signatures of BOTH generated surfaces
// — the Qt-typed one (ApiStyle::Qt) and the Qt-free lp one (ApiStyle::Lp, used
// by cdylib/universal modules whose translation units must not see Qt). Only
// std types and logos::CallError may appear here.
//
// (It lives in logos-cpp-sdk rather than in logos-protocol's
// logos_call_error.h only because that is where the generator that emits it
// lives; ${LOGOS_CPP_SDK_ROOT}/include is on the include path of every module
// build — see logos-module-builder/cmake/LogosModule.cmake, which adds it
// unconditionally. That file used to have a twin in logos-plugin-qt; it does
// not any more.)

#include "logos_call_error.h"

namespace logos {

/**
 * @brief An async call's outcome: the decoded value plus the call error.
 *
 * `error.ok()` (surfaced as `ok()`) is the ONLY reliable success test — a
 * failed call leaves `value` default-constructed, which for most return types
 * is also a perfectly legal success value.
 *
 *   dep.echoIntAsyncResult(7, [](logos::AsyncResult<qlonglong> r) {
 *       if (!r.ok()) { qWarning() << r.error.code.c_str(); return; }
 *       use(r.value);
 *   });
 *
 * Aggregate — `AsyncResult<T>{v, err}` and designated-ish brace init both work.
 */
template <typename T>
struct AsyncResult {
    T value{};
    CallError error;

    bool ok() const { return error.ok(); }
    explicit operator bool() const { return error.ok(); }
};

/**
 * @brief The void specialization: an error channel with no value.
 *
 * A `void`-returning method could equally have been given a plain
 * `std::function<void(logos::CallError)>` callback. It is spelled
 * AsyncResult<void> instead so that EVERY generated `fooAsyncResult` takes
 * `std::function<void(logos::AsyncResult<R>)>` for its own return type R with
 * no special case — forwarding/proxy code (and the generator itself) can write
 * the callback type from the return type mechanically, and every call site
 * reads `if (!r.ok())` regardless of what the method returns.
 */
template <>
struct AsyncResult<void> {
    CallError error;

    bool ok() const { return error.ok(); }
    explicit operator bool() const { return error.ok(); }
};

}  // namespace logos

#endif  // LOGOS_ASYNC_RESULT_H
