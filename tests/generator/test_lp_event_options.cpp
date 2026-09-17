// The lp emitter's EVENT surface, which had no golden coverage at all before
// this file — every existing generator test passes methods and no events.
//
// What is pinned here is the shape module authors actually see: a ticket back
// from each subscribe, and ONE set of state controls per module rather than per
// event. A wrapper that silently loses either costs an author the ability to
// unsubscribe or to know their subscriptions were lost — and nothing else in
// the suite would notice, because the payload callback keeps working either
// way.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "generator_lib.h"

namespace {

QJsonObject param(const QString& name, const QString& type)
{
    QJsonObject p;
    p["name"] = name;
    p["type"] = type;
    return p;
}

QJsonArray sampleEvents()
{
    QJsonObject ev;
    ev["name"] = "somethingHappened";
    QJsonArray params;
    params.append(param("count", "int64"));
    params.append(param("label", "tstr"));
    ev["params"] = params;

    QJsonObject bare;
    bare["name"] = "pinged";
    bare["params"] = QJsonArray{};

    QJsonArray a;
    a.append(ev);
    a.append(bare);
    return a;
}

QString lpHeaderWithEvents()
{
    return makeHeaderLp("mod", "Mod", QJsonArray{}, sampleEvents());
}

QString lpSourceWithEvents()
{
    return makeSourceLp("mod", "Mod", "mod.h", QJsonArray{}, sampleEvents());
}

} // namespace

// ── the author-facing signature ─────────────────────────────────────────────

// The accessor returns a non-owning ticket rather than a bare bool, which is
// the ONLY way an author can reach a subscription the wrapper stores privately.
TEST(LpEventOptionsTest, HeaderReturnsASubHandle)
{
    const QString h = lpHeaderWithEvents();
    EXPECT_TRUE(h.contains("logos::SubHandle onSomethingHappened("))
        << "the accessor must return a ticket, or unsubscribing is unreachable";
    EXPECT_TRUE(h.contains("logos::SubHandle onPinged("));
}

// One std::function and nothing else. A second callback parameter would be
// ambiguous for a generic lambda — the same hazard that made invokeAsyncResult
// a distinct name rather than an overload — and an options struct has nothing
// left to carry now that policy and status are per module.
TEST(LpEventOptionsTest, TheAccessorTakesOnlyTheCallback)
{
    const QString h = lpHeaderWithEvents();
    const int at = h.indexOf("onSomethingHappened(");
    ASSERT_GT(at, 0);
    const QString decl = h.mid(at, h.indexOf(';', at) - at);
    EXPECT_EQ(decl.count("std::function"), 1) << decl.toStdString();
    EXPECT_FALSE(decl.contains("SubscribeOptions"))
        << "per-subscription options are gone; policy and status are per module";
}

// ── the per-target surface ──────────────────────────────────────────────────

// Policy, status, generation and rearm are emitted ONCE per dep, not once per
// event. That is the whole shape of the change: a provider dying is a module
// event, so the controls for it hang off the module.
TEST(LpEventOptionsTest, TheStateSurfaceIsPerModuleNotPerEvent)
{
    const QString h = lpHeaderWithEvents();
    for (const char* m : {"onSubscriptionStatus(", "subscriptionGeneration(",
                          "setRestartPolicy(", "rearmSubscriptions("})
        EXPECT_EQ(h.count(QString(m)), 1)
            << m << " must be emitted once per module, not once per event";
}

// A dep with no events gets none of it: there are no subscriptions whose state
// could be asked about, and emitting the accessors anyway would advertise a
// capability that answers nothing.
TEST(LpEventOptionsTest, ADepWithNoEventsGetsNoStateSurface)
{
    const QString h = makeHeaderLp("mod", "Mod", QJsonArray{}, QJsonArray{});
    EXPECT_FALSE(h.contains("onSubscriptionStatus("));
    EXPECT_FALSE(h.contains("rearmSubscriptions("));
}

TEST(LpEventOptionsTest, SourceForwardsTheStateSurfaceToTheClient)
{
    const QString src = lpSourceWithEvents();
    EXPECT_TRUE(src.contains(".onSubscriptionStatus(std::move(cb))"));
    EXPECT_TRUE(src.contains(".subscriptionGeneration()"));
    EXPECT_TRUE(src.contains(".setRestartPolicy(policy)"));
    EXPECT_TRUE(src.contains(".rearmSubscriptions()"));
}

// ── the body ────────────────────────────────────────────────────────────────

// Plain subscribe: there is no per-subscription options call left to route
// through, and routing through one would be routing through a function that no
// longer exists.
TEST(LpEventOptionsTest, SourceRoutesThroughPlainSubscribe)
{
    const QString src = lpSourceWithEvents();
    EXPECT_TRUE(src.contains(".subscribe(\"somethingHappened\""));
    EXPECT_FALSE(src.contains("subscribeOpts"));
}

// The OWNING handle stays the wrapper's; what the author gets is the weak
// ticket taken from it. If these ever swap, an author dropping a return value
// would silently end their own subscription.
TEST(LpEventOptionsTest, TheOwningHandleIsStillKeptByTheWrapper)
{
    const QString src = lpSourceWithEvents();
    EXPECT_TRUE(src.contains("logos::SubHandle _h = _sub.handle();"));
    EXPECT_TRUE(src.contains("push_back(std::move(_sub))"));
    EXPECT_TRUE(src.contains("return _h;"));
    EXPECT_TRUE(src.contains("if (!_sub.valid()) return {};"));
}

// The payload path is unchanged: still a JSON array decoded positionally into
// typed args. This is the control — without it, "the surface moved" could be
// true of a wrapper that no longer delivers events.
TEST(LpEventOptionsTest, ThePayloadPathIsUnchanged)
{
    const QString src = lpSourceWithEvents();
    EXPECT_TRUE(src.contains("nlohmann::json _a"));
    EXPECT_TRUE(src.contains("_a.is_array()"));
    EXPECT_TRUE(src.contains("callback("));
}
