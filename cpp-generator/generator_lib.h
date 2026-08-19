#ifndef GENERATOR_LIB_H
#define GENERATOR_LIB_H

#include <QString>
#include <QJsonArray>
#include <QStringList>
#include <QTextStream>
#include <QVector>
#include <QPair>

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

// Parse the `--api-style` flag out of a raw argument list. Both spellings are
// accepted (`--api-style lp` and `--api-style=lp`); absent means Qt. Returns
// false — having written a diagnostic to `err` — for a value the generator
// refuses, in which case `outStyle` is untouched and the caller must exit 1.
//
// Lives here, next to the enum, because BOTH CLI entry points need it: the
// umbrella mode in main.cpp and the plugin-introspection path. Two copies of this
// table is exactly how the surfaces drift apart.
//
// `std` was a third surface (std types over a QVariant/LogosAPIClient body).
// It is retired, and rejected LOUDLY rather than aliased to qt: a stale caller
// that still passes it wants std signatures, and silently handing it the Qt
// surface would only fail later, further from the cause.
bool parseApiStyleFlag(const QStringList& args, ApiStyle& outStyle, QTextStream& err);

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

// How the UMBRELLA binds its wrappers to a transport — the call ORIGIN, where
// BindMode above decides the call TARGET.
//   FromApi        — `explicit LogosModules(LogosAPI* api)`, each member built
//                    as `<dep>(api)` and each factory as `<Iface>(api, name)`.
//                    The origin is derived, inside the wrapper, from
//                    `api->moduleName()`. The historical shape, and the default.
//   ExplicitOrigin — `LogosModules()`, default-constructible, NO LogosAPI
//                    member and no `logos_api.h` include: each member is built
//                    as `<dep>(QStringLiteral("<this module>"))` and each
//                    factory as `<Iface>(QStringLiteral("<this module>"), name)`.
//
// Deliberately a parameter and NOT a third ApiStyle value. ApiStyle names the
// TYPE SURFACE, and is switched on by makeHeader / makeSource / returnTypeFor /
// paramTypeFor / toWireFor / fromWireFor; a "Qt types, explicit origin" enum
// value would oblige every one of those to answer a question about transport
// binding that has no bearing on the types they map — and the honest answer in
// each would be "same as Qt". The axis being added here is orthogonal to the
// type surface, so it gets its own name.
//
// ApiStyle::Lp IGNORES this: the Qt-free umbrella has only one binding (it is
// origin-bound by construction, which is what this brings to the Qt surface).
//
// The origin is the CONSUMING module's own name, from `metadata.json#name`. An
// empty one is not defaulted or inferred — the emitted header carries an
// `#error` instead, because a wrapper that cannot state its own identity would
// otherwise open a connection under a blank one.
enum class UmbrellaBinding { FromApi, ExplicitOrigin };

// Parse `--binding api|origin` out of a raw argument list (both `--binding
// origin` and `--binding=origin`). Absent means FromApi. Returns false — having
// written a diagnostic to `err` — for a value the generator refuses.
bool parseUmbrellaBindingFlag(const QStringList& args, UmbrellaBinding& outBinding, QTextStream& err);

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

// The umbrella (`logos_sdk.h` / `logos_sdk.cpp`) over a module's declared
// `metadata.json#dependencies` + interface dependencies: one `#include` and one
// `LogosModules` member per dep, so a module reaches its deps as
// `modules().<dep>`. `deps` is the raw metadata array — elements are read
// through dependencyNames() (metadata_dependencies.h), never element by
// element, so includes and members can never disagree about what it declares.
//
// ApiStyle::Lp emits the Qt-free umbrella: no LogosAPI member, each wrapper
// self-creates its lp_client on behalf of `originName` (the module being
// generated for), so the struct is default-constructible. Qt emits the
// LogosAPI-threading form, where `originName` is unused.
// `binding` is trailing and defaulted so every current caller keeps the
// LogosAPI-threading umbrella, unchanged. With ExplicitOrigin the Qt umbrella
// becomes default-constructible and drops its LogosAPI — matching the shape the
// Lp flavour already has, and pairing with the wrappers logos-qt-generator
// emits under `--backend consumer --binding origin`.
QString makeUmbrellaHeaderFromDeps(const QJsonArray& deps, const QStringList& interfaceNames, ApiStyle apiStyle = ApiStyle::Qt, const QString& originName = QString(), UmbrellaBinding binding = UmbrellaBinding::FromApi);
QString makeUmbrellaSourceFromDeps(const QJsonArray& deps, const QStringList& interfaceNames);

#endif // GENERATOR_LIB_H
