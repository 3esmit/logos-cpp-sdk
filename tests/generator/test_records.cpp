// Records on the consumer side of the LEGACY generator — the wrapper every
// C++ module actually gets for its dependencies (`--dep <name>=<lidl>`).
//
// A contract's `type Status { ... }` used to reach every C++ consumer as an
// untyped bag: QVariant on the Qt surface, LogosMap on the lp one. The
// caller then had to know the field names AND, for a `bstr` field, that the
// value arrives as the canonical `{"_bytes": "..."}` envelope it must unwrap
// itself — while Rust and the client-stub backend hand back a real struct.
//
// These assert on generated source text. That the emitted conversions compile
// and round-trip (bytes tagged at every depth, uint64 above 2^32 intact) is
// covered by generating a wrapper and building it — see the PR description.

#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include "generator_lib.h"

namespace {

QJsonObject field(const char* name, const char* type)
{
    QJsonObject f;
    f["name"] = name;
    f["type"] = type;
    return f;
}

QJsonObject param(const char* name, const char* type)
{
    QJsonObject p;
    p["name"] = name;
    p["type"] = type;
    return p;
}

QJsonObject method(const char* name, const char* returnType, const QJsonArray& params = {})
{
    QJsonObject m;
    m["name"] = name;
    m["returnType"] = returnType;
    m["isInvokable"] = true;
    m["parameters"] = params;
    return m;
}

// `type Status { port: uint, blob: bstr }` plus a record that nests it in
// both container shapes.
QJsonArray statusRecords()
{
    QJsonObject status;
    status["name"] = "Status";
    status["fields"] = QJsonArray{field("port", "qulonglong"), field("blob", "QByteArray")};

    QJsonObject batch;
    batch["name"] = "Batch";
    batch["fields"] = QJsonArray{field("label", "QString"),
                                 field("items", "QList<Status>"),
                                 field("tags", "QMap<QString, Status>")};
    return QJsonArray{status, batch};
}

QJsonArray statusMethods()
{
    return QJsonArray{
        method("getStatus", "Status"),
        method("describeStatus", "QString", QJsonArray{param("s", "Status")}),
        method("listStatuses", "QList<Status>"),
        method("getBatch", "Batch"),
    };
}

} // namespace

// The Qt surface: a struct nested in the wrapper class, typed accessors, and
// no QVariant anywhere a record is named.
TEST(Records, QtWrapperExposesTheStruct)
{
    const QString h = makeHeader("info_module", "InfoModule", statusMethods(),
                                 ApiStyle::Qt, {}, BindMode::Static, statusRecords());

    // Nested, so two deps may each declare a `Status` in one consumer.
    EXPECT_TRUE(h.contains("    struct Status {"));
    EXPECT_TRUE(h.contains("        qulonglong port{};"));
    EXPECT_TRUE(h.contains("        QByteArray blob{};"));

    // Containers of records keep their element type.
    EXPECT_TRUE(h.contains("        QList<Status> items{};"));
    EXPECT_TRUE(h.contains("        QMap<QString, Status> tags{};"));

    EXPECT_TRUE(h.contains("Status getStatus(logos::CallError* err = nullptr, Timeout timeout = Timeout());"));
    EXPECT_TRUE(h.contains("QString describeStatus(const Status& s,"));
    EXPECT_TRUE(h.contains("QList<Status> listStatuses("));

    // The old fallback is gone.
    EXPECT_FALSE(h.contains("QVariant getStatus("));
    EXPECT_FALSE(h.contains("describeStatus(QVariant"));
}

// The lp surface spells the same records in std types — a universal
// (Qt-free) module never sees a Qt name.
TEST(Records, LpWrapperUsesStdFieldTypes)
{
    for (ApiStyle style : {ApiStyle::Lp}) {
        const QString h = makeHeader("info_module", "InfoModule", statusMethods(),
                                     style, {}, BindMode::Static, statusRecords());
        EXPECT_TRUE(h.contains("        uint64_t port{};")) << h.toStdString();
        EXPECT_TRUE(h.contains("        std::vector<uint8_t> blob{};"));
        EXPECT_TRUE(h.contains("        std::vector<Status> items{};"));
        EXPECT_TRUE(h.contains("        std::map<std::string, Status> tags{};"));
        EXPECT_TRUE(h.contains("std::vector<Status> listStatuses("));
        // No LogosMap stand-in for a record.
        EXPECT_FALSE(h.contains("LogosMap getStatus("));
    }

    // std::map needs its header on the lp surface.
    const QString lp = makeHeader("info_module", "InfoModule", statusMethods(),
                                  ApiStyle::Lp, {}, BindMode::Static, statusRecords());
    EXPECT_TRUE(lp.contains("#include <map>"));
}

// A `bstr` field must ride the canonical tagged form at any depth — the
// defect class that made a record-as-LogosMap actively wrong rather than
// merely inconvenient.
TEST(Records, BytesFieldsUseTheCanonicalEncoding)
{
    const QString lp = makeSource("info_module", "InfoModule", "info_module_api.h",
                                  statusMethods(), ApiStyle::Lp, {}, BindMode::Static,
                                  statusRecords());
    EXPECT_TRUE(lp.contains("__j[\"blob\"] = logos::bytesToJson(v.blob);"));
    EXPECT_TRUE(lp.contains("__out.blob = logos::jsonToBytes(w.at(\"blob\"));"));

    const QString qt = makeSource("info_module", "InfoModule", "info_module_api.h",
                                  statusMethods(), ApiStyle::Qt, {}, BindMode::Static,
                                  statusRecords());
    EXPECT_TRUE(qt.contains("__out.blob = __m.value(QStringLiteral(\"blob\")).toByteArray();"));
}

// The conversions are file-local statics in the .cpp: an lp consumer's own
// translation units must not need the wire type to include the header.
TEST(Records, ConversionsStayOutOfTheHeader)
{
    const QString h = makeHeader("info_module", "InfoModule", statusMethods(),
                                 ApiStyle::Lp, {}, BindMode::Static, statusRecords());
    EXPECT_FALSE(h.contains("recToWire_Status"));

    const QString c = makeSource("info_module", "InfoModule", "info_module_api.h",
                                 statusMethods(), ApiStyle::Lp, {}, BindMode::Static,
                                 statusRecords());
    EXPECT_TRUE(c.contains("static nlohmann::json recToWire_Status(const InfoModule::Status& v);"));
    EXPECT_TRUE(c.contains("static InfoModule::Status recFromWire_Status(const nlohmann::json& w);"));
    // Declared before defined, so records may reference each other in any order.
    EXPECT_LT(c.indexOf("static nlohmann::json recToWire_Batch(const InfoModule::Batch& v);"),
              c.indexOf("static nlohmann::json recToWire_Status(const InfoModule::Status& v) {"));
}

// A return type written before the `Class::` of a definition is outside class
// scope and must be qualified; a parameter is inside it and must not be
// (an unqualified return type simply does not compile).
TEST(Records, ReturnTypesAreQualifiedInTheDefinition)
{
    const QString c = makeSource("info_module", "InfoModule", "info_module_api.h",
                                 statusMethods(), ApiStyle::Qt, {}, BindMode::Static,
                                 statusRecords());
    EXPECT_TRUE(c.contains("InfoModule::Status InfoModule::getStatus("));
    EXPECT_TRUE(c.contains("QList<InfoModule::Status> InfoModule::listStatuses("));
    EXPECT_TRUE(c.contains("QString InfoModule::describeStatus(const Status& s,"));
}

// Container decode lambdas are emitted INSIDE the record decoder, which has
// its own `__m` / `__j`. Reusing those names made a map-of-records field read
// from its own uninitialized local — it compiled, with only a warning.
TEST(Records, ContainerLambdasDoNotShadowTheDecoderLocals)
{
    for (ApiStyle style : {ApiStyle::Qt, ApiStyle::Lp}) {
        const QString c = makeSource("info_module", "InfoModule", "info_module_api.h",
                                     statusMethods(), style, {}, BindMode::Static,
                                     statusRecords());
        // The decoder's own map is `__m` (Qt); a nested lambda must not
        // declare another one.
        EXPECT_FALSE(c.contains("const QVariantMap __m = (__m.value")) << c.toStdString();
        EXPECT_FALSE(c.contains("const nlohmann::json& __j = w.at")) << c.toStdString();
    }
}

// The record path is additive: with no records declared, every byte of the
// generated output is what it was before.
TEST(Records, EmptyRecordSetChangesNothing)
{
    const QJsonArray methods{method("ping", "QString", QJsonArray{param("msg", "QString")})};
    for (ApiStyle style : {ApiStyle::Qt, ApiStyle::Lp}) {
        EXPECT_EQ(makeHeader("m", "M", methods, style, {}, BindMode::Static, {}),
                  makeHeader("m", "M", methods, style, {}, BindMode::Static));
        EXPECT_EQ(makeSource("m", "M", "m_api.h", methods, style, {}, BindMode::Static, {}),
                  makeSource("m", "M", "m_api.h", methods, style, {}, BindMode::Static));
    }
}

// Records reach event callbacks too — an event payload is as typed as a
// return value.
TEST(Records, TypedEventCallbacksTakeTheStruct)
{
    QJsonObject ev;
    ev["name"] = "statusChanged";
    ev["params"] = QJsonArray{param("s", "Status"), param("at", "qlonglong")};
    const QJsonArray events{ev};

    const QString h = makeHeader("info_module", "InfoModule", statusMethods(),
                                 ApiStyle::Qt, events, BindMode::Static, statusRecords());
    EXPECT_TRUE(h.contains("bool onStatusChanged(std::function<void(const Status& s, qlonglong at)> callback);"));

    const QString c = makeSource("info_module", "InfoModule", "info_module_api.h",
                                 statusMethods(), ApiStyle::Qt, events, BindMode::Static,
                                 statusRecords());
    EXPECT_TRUE(c.contains("recFromWire_Status(_args.at(0))"));
}
