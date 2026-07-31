#ifndef GENERATOR_LIB_H
#define GENERATOR_LIB_H

#include <QString>
#include <QJsonArray>
#include <QTextStream>
#include <QVector>
#include <QPair>

struct ParsedMethod {
    QString returnType;
    QString name;
    QVector<QPair<QString, QString>> params; // (type, name)
    QString description; // doc comment adjacent to the LOGOS_METHOD declaration
};

// Which type surface to expose on the generated per-module wrapper.
// Each module's build picks ONE — there's no composite output. Default
// is Qt for backward compatibility; `interface: "universal"` modules
// flip to Lp via the -DLOGOS_API_STYLE=lp CMake flag the module builder
// threads through.
// Qt — legacy Qt-typed surface (QString/QVariant…), body via LogosAPIClient.
// Lp  — std-typed surface AND a Qt-free body: the wrapper calls the
//       logos-protocol C ABI (lp_*) directly via logos::LpClient, so the
//       module's translation units never include Qt or link qt-sdk. This is
//       the path that lets a cdylib module do outbound typed calls/event
//       subscriptions while staying Qt-free (Qt confined to the QRO transport
//       inside logos-protocol + the generated plugin glue).
//
// A third flavour, Std, used to sit between the two: the std-typed surface
// with a body that still bridged through QVariant + LogosAPIClient. Nothing
// selected it any more (universal modules go straight to Lp), so it was
// retired; `--api-style=std` is now a hard error rather than a silent alias.
enum class ApiStyle { Qt, Lp };

// Whether the generated wrapper targets ONE fixed module (the historical
// behaviour) or binds to a module name chosen at runtime.
//   Static — the module name is baked into the ctor + every remote call,
//            so `<Class>(LogosAPI*)` always talks to that one module.
//            This is what name-baked dependency wrappers use.
//   Bound  — the ctor takes `(LogosAPI*, const QString& moduleName)` and
//            stores it in `m_moduleName`; every remote call routes through
//            that member. This is what *interface* wrappers use: one
//            interface, bound to a concrete module name at runtime.
// Default is Static so existing callers and their generated output are
// byte-for-byte unchanged.
enum class BindMode { Static, Bound };

QString toPascalCase(const QString& name);
QString normalizeType(QString t);
QString mapParamType(const QString& qtType);
QString mapReturnType(const QString& qtType);
QString toQVariantConversion(const QString& type, const QString& argExpr);

// Incoming provider argument -> the declared type, via the canonical codec
// (logos::qtArgFromVariant). `path` names the slot in the diagnostic a
// rejection carries, e.g. "arg0". See the definition for why this is separate
// from toQVariantConversion.
QString toProviderArgDecode(const QString& type, const QString& argExpr,
                            const QString& path);

// makeHeader / makeSource emit the single `<Class>` wrapper for a
// module. When `apiStyle == Qt`, the output is the legacy Qt-typed
// surface (QString / QStringList / QVariantList / QVariantMap / int /
// LogosResult) with a body that calls LogosAPIClient. When
// `apiStyle == Lp`, they delegate to makeHeaderLp / makeSourceLp below,
// which emit the std-typed, Qt-free surface. The class name is always
// `<Module>` either way; the two styles are mutually exclusive.
//
// `events` carries typed event prototypes loaded from a `.lidl`
// sidecar via --events-from. Each entry is
//   { "name": "<event>", "params": [ { "name": "...", "type": "<QtTypeName>" } ] }
// (Qt-typed names — same surface methods come through). When non-empty,
// the wrapper also gets one `on<EventName>(callback)` accessor per
// event next to the existing generic `onEvent(name, callback)` channel.
// The accessor signature uses the apiStyle's type surface for the
// callback's argument types.
//
// `bindMode` selects a fixed-module wrapper (Static, default) or a
// runtime-bound interface wrapper (Bound) — see BindMode above. In Bound
// mode `moduleName` is used only for the class/file naming the caller
// already decided; the emitted code never bakes it into a call.
//
// `records` carries the contract's `type Foo { ... }` declarations, as
//   [ { "name": "Foo", "fields": [ { "name": "...", "type": "<QtTypeName>" } ] } ]
// Each becomes a struct NESTED in the wrapper class (`<Class>::Foo`, so two
// deps may both declare a `Status`), and every method / event that mentions
// one is typed with it instead of falling back to QVariant / LogosMap. Field
// types use the same Qt type-name spelling as methods, so a field can name
// another record, `QList<Record>`, or `QMap<QString, Record>`. Empty (the
// default, and what the metaobject-introspection path passes) leaves the
// generated output exactly as it was.
QString makeHeader(const QString& moduleName, const QString& className, const QJsonArray& methods, ApiStyle apiStyle = ApiStyle::Qt, const QJsonArray& events = {}, BindMode bindMode = BindMode::Static, const QJsonArray& records = {});
QString makeSource(const QString& moduleName, const QString& className, const QString& headerBaseName, const QJsonArray& methods, ApiStyle apiStyle = ApiStyle::Qt, const QJsonArray& events = {}, BindMode bindMode = BindMode::Static, const QJsonArray& records = {});

// Qt-free (ApiStyle::Lp) wrapper emission. A std-typed surface
// (std::string / std::vector<std::string> / LogosMap / LogosList / int64_t /
// StdLogosResult) whose generated body calls the logos-protocol C ABI through
// logos::LpClient instead of LogosAPIClient — no Qt in the wrapper's TU.
// makeHeader/makeSource dispatch here when apiStyle == ApiStyle::Lp.
QString makeHeaderLp(const QString& moduleName, const QString& className, const QJsonArray& methods, const QJsonArray& events = {}, BindMode bindMode = BindMode::Static, const QJsonArray& records = {});
QString makeSourceLp(const QString& moduleName, const QString& className, const QString& headerBaseName, const QJsonArray& methods, const QJsonArray& events = {}, BindMode bindMode = BindMode::Static, const QJsonArray& records = {});
QVector<ParsedMethod> parseProviderHeader(const QString& headerPath, QTextStream& err);

#endif // GENERATOR_LIB_H
