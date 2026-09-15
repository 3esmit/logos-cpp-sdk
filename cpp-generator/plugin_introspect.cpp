#include <QCoreApplication>
#include <QPluginLoader>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMetaMethod>
#include <QByteArray>
#include <QTextStream>
#include <QDir>
#include <QByteArrayList>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtGlobal>
#include "logos_provider_interface.h"
#include "generator_lib.h"
#include "metadata_dependencies.h"
#include "experimental/lidl_compat.h"
#include "lidl_to_json.h"   // ModuleDecl -> the JSON surface generator_lib consumes

// The `.lidl` sidecar a module ships beside its built plugin
// (`<lib>/share/logos/<name>.lidl`) — its CONTRACT, parsed into the three JSON
// arrays generator_lib's makeHeader / makeSource consume.
//
// ─── Why the METHODS come from here and not from the plugin ──────────────
//
// This used to load events (and records) only; the methods came from the
// plugin's published `getMethods()`. That made the wrapper's whole type
// surface depend on the VOCABULARY a module happens to publish its metadata
// in, and generator_lib is keyed on flat type NAMES with a QVariant fallback
// (mapParamType / mapReturnType) — so a module that spells its metadata any
// way this emitter does not recognise gets a wrapper of QVariant / LogosMap
// with no diagnostic at all. It is a machine reader of a human-facing
// listing, and it degrades silently.
//
// It measurably broke: when the cdylib backend started publishing the LIDL
// contract vocabulary (`tstr`, `[uint]`, `? tstr`) instead of Qt type names,
// every `interface: "universal"` module's lp wrapper turned into LogosMap.
// Teaching this reader a second vocabulary is not a fix — `int` means a
// 32-bit Qt int in one and a 64-bit LIDL integer in the other, so a merged
// table silently mistypes every integer, and the reader cannot tell from the
// string which table it is holding.
//
// The contract has no such ambiguity: it is a TypeExpr tree, and
// lidl_to_json is the one place it is flattened. So when a module publishes a
// contract, that is what the wrapper is generated from — which also makes
// this path emit byte-identical output to `--general-only --dep
// <name>=<name>.lidl` (main.cpp's generateInterfaceWrappers), the path
// buildHeaders.nix already takes under cross-compilation and for the whole Qt
// surface. Introspection is what is left over for a module that publishes NO
// contract (a handcrafted Qt plugin), where the QMetaObject's Qt type names
// are the only description of its API that exists.
//
// A sidecar that is present but unreadable or malformed is FATAL. Returning
// empty and carrying on is what let a broken sidecar ship a wrapper with no
// typed event accessors and (now) no typed methods — the same silently-empty
// shape generate-module-headers.sh exists to refuse.
static bool loadContractFromLidl(const QString& lidlPath, QTextStream& err,
                                 QJsonArray* outMethods, QJsonArray* outEvents,
                                 QJsonArray* outRecords)
{
    QFile f(lidlPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        err << "Failed to open contract sidecar: " << lidlPath << "\n";
        return false;
    }
    QString source = QString::fromUtf8(f.readAll());
    f.close();

    LidlParseResult pr = lidlParse(source);
    if (pr.hasError()) {
        err << lidlPath << ":" << pr.errorLine << ":" << pr.errorColumn
            << ": " << pr.error << "\n";
        return false;
    }

    ModuleDecl mod = pr.module;
    {
        QString recErr;
        if (!lidlCheckRecords(mod, &recErr)) {
            err << lidlPath << ": " << recErr << "\n";
            return false;
        }
    }
    {
        // Consumers see name()/version()/lidl() on every dependency. Added
        // after parsing: the published .lidl is the canonical authored API,
        // while the provider derives the same built-ins from the same frontend
        // (main.cpp's --backend cdylib path).
        QString idErr;
        if (!lidlInjectIdentity(mod, &idErr)) {
            err << lidlPath << ": " << idErr << "\n";
            return false;
        }
    }

    noteOptionalPositionalSlots(mod, lidlPath, err);
    if (outMethods) *outMethods = moduleMethodsToJson(mod);
    if (outEvents)  *outEvents  = moduleEventsToJson(mod);
    if (outRecords) *outRecords = moduleRecordsToJson(mod);
    return true;
}

// Is this published type name spelled in the LIDL CONTRACT vocabulary rather
// than in Qt type names?
//
// The two vocabularies are not distinguishable in general — `int` and `bool`
// are words in both, at different widths — which is exactly why this emitter
// must not try to read both. But it does not have to: those overlapping words
// are all in mapParamType/mapReturnType's known table, so they never reach the
// fallback. What reaches the fallback and is UNAMBIGUOUS is the LIDL half that
// Qt has no word for at all: the primitive names below, and any container or
// optional, which start with a character no C++ type name starts with.
//
// Deliberately NOT a second type table. The answer is only ever used to REFUSE
// — see below — so a false negative degrades to the old behaviour and a false
// positive is impossible: no Qt type is called `tstr`, and none begins with
// `[`, `{` or `?`.
static bool looksLikeLidlSpelling(const QString& raw)
{
    const QString t = raw.trimmed();
    if (t.isEmpty()) return false;
    if (t.startsWith('[') || t.startsWith('{') || t.startsWith('?')) return true;
    static const QSet<QString> unambiguous = {
        QStringLiteral("tstr"),  QStringLiteral("bstr"), QStringLiteral("uint"),
        QStringLiteral("float64"), QStringLiteral("result"), QStringLiteral("any"),
    };
    return unambiguous.contains(t);
}

// Every LIDL-spelled type name in a published listing, as "method: type" for a
// diagnostic. Empty when the listing is in Qt names, which is the only
// vocabulary this emitter can read.
static QStringList lidlSpelledSlots(const QJsonArray& methods)
{
    QStringList out;
    for (const QJsonValue& mv : methods) {
        if (!mv.isObject()) continue;
        const QJsonObject mo = mv.toObject();
        const QString name = mo.value("name").toString();
        const QString ret = mo.value("returnType").toString();
        if (looksLikeLidlSpelling(ret))
            out << (name + "() -> " + ret);
        for (const QJsonValue& pv : mo.value("parameters").toArray()) {
            const QString pt = pv.toObject().value("type").toString();
            if (looksLikeLidlSpelling(pt))
                out << (name + "(" + pv.toObject().value("name").toString() + ": " + pt + ")");
        }
    }
    return out;
}

// The interface/dependency wrapper machinery (InterfaceSpec, parseSpecFlags,
// parseInterfaceFile, generateInterfaceWrappers) moved to ../main.cpp with the
// umbrella mode it exclusively serves — see the "Umbrella mode" block there.

static QJsonArray enumerateMethods(QObject* moduleInstance)
{
    QJsonArray methodsArray;

    if (!moduleInstance) {
        return methodsArray;
    }

    const QMetaObject* metaObject = moduleInstance->metaObject();

    for (int i = 0; i < metaObject->methodCount(); ++i) {
        QMetaMethod method = metaObject->method(i);

        if (method.enclosingMetaObject() != metaObject) {
            continue;
        }

        QJsonObject methodObj;
        methodObj["signature"] = QString::fromUtf8(method.methodSignature());
        methodObj["name"] = QString::fromUtf8(method.name());
        methodObj["returnType"] = QString::fromUtf8(method.typeName());
        bool isInvokable = method.isValid() && (method.methodType() == QMetaMethod::Method || method.methodType() == QMetaMethod::Slot);
        methodObj["isInvokable"] = isInvokable;

        if (method.parameterCount() > 0) {
            QJsonArray params;
            for (int p = 0; p < method.parameterCount(); ++p) {
                QJsonObject paramObj;
                paramObj["type"] = QString::fromUtf8(method.parameterTypeName(p));
                QByteArrayList paramNames = method.parameterNames();
                if (p < paramNames.size() && !paramNames.at(p).isEmpty()) {
                    paramObj["name"] = QString::fromUtf8(paramNames.at(p));
                } else {
                    paramObj["name"] = QString("param%1").arg(p);
                }
                params.append(paramObj);
            }
            methodObj["parameters"] = params;
        }

        methodsArray.append(methodObj);
    }

    return methodsArray;
}

// toPascalCase, normalizeType, mapParamType, mapReturnType -> generator_lib.h/cpp

// makeHeader -> generator_lib.h/cpp

// makeSource -> generator_lib.h/cpp

// `contractMethods` is the module's own contract, when it ships one; empty
// when it does not. Non-empty wins over whatever the plugin publishes — see
// loadContractFromLidl for why the published metadata is not a type source.
static int generateFromPlugin(const QString& pluginInputPath, const QString& outputDir, ApiStyle apiStyle, const QJsonArray& events, QTextStream& out, QTextStream& err, const QJsonArray& records = {}, const QJsonArray& contractMethods = {})
{
    QFileInfo fi(pluginInputPath);
    if (!fi.exists()) {
        err << "Plugin file does not exist: " << pluginInputPath << "\n";
        return 2;
    }

    QString resolvedPath = fi.canonicalFilePath();
    if (resolvedPath.isEmpty()) {
        resolvedPath = fi.absoluteFilePath();
    }

    QString genDirPath = outputDir.isEmpty() ? QDir::current().filePath("logos-cpp-sdk/cpp/generated") : outputDir;
    QDir().mkpath(genDirPath);

    QPluginLoader loader(resolvedPath);
    if (!loader.load()) {
        err << "Failed to load plugin at " << resolvedPath << ": " << loader.errorString() << "\n";
        return 3;
    }
    QObject* instance = loader.instance();
    if (!instance) {
        err << "Plugin loaded but no instance could be created for " << resolvedPath << "\n";
        loader.unload();
        return 4;
    }

    QString moduleName;
    {
        QJsonObject md = loader.metaData();
        QJsonObject meta = md.value("MetaData").toObject();
        moduleName = meta.value("name").toString();
        if (moduleName.isEmpty()) {
            moduleName = QFileInfo(resolvedPath).baseName();
        }
    }

    // What the PLUGIN says about itself. Still read even when a contract is
    // present: loading the plugin is the dlopen check this path exists to
    // perform (exit 3 on an SDK/ABI skew), and comparing the two name sets is
    // the only place a stale sidecar can be noticed at all.
    QJsonArray publishedMethods;
    LogosProviderPlugin* providerPlugin = qobject_cast<LogosProviderPlugin*>(instance);
    if (providerPlugin) {
        LogosProviderObject* provider = providerPlugin->createProviderObject();
        if (provider) {
            publishedMethods = provider->getMethods();
            out << "Detected new-API plugin (LogosProviderPlugin), using getMethods() — "
                << publishedMethods.size() << " methods\n";
            delete provider;
        } else {
            err << "LogosProviderPlugin::createProviderObject() returned null\n";
        }
    } else {
        publishedMethods = enumerateMethods(instance);
    }

    // Contract-first. The published listing is a HUMAN-facing description
    // whose vocabulary is the publisher's choice; the contract is the typed
    // one. Only a module that ships no contract is described by its plugin.
    QJsonArray methods = publishedMethods;
    if (contractMethods.isEmpty()) {
        // No contract, so the listing is the only description there is — and
        // it has to be one this emitter can READ. It is keyed on Qt type names
        // with a QVariant fallback, so a listing in the LIDL contract
        // vocabulary produces a wrapper of QVariant / LogosMap that compiles
        // and has lost every type. That is not hypothetical: it is what
        // happened to every `interface: "universal"` module when the cdylib
        // backend switched its published metadata to LIDL names.
        //
        // REFUSED, not warned. The build systems always pass --events-from for
        // a module that ships a contract, so reaching here with LIDL names
        // means either a hand-run invocation that omitted the flag (the shape
        // the docs used to suggest) or a caller that lost it — and in both
        // cases the wrapper would be silently untyped. The message names the
        // flag, because the fix is always the same one file.
        const QStringList lidlSlots = lidlSpelledSlots(publishedMethods);
        if (!lidlSlots.isEmpty()) {
            err << "Error: '" << moduleName << "' publishes its metadata in the LIDL\n"
                << "       contract vocabulary, and no --events-from contract was given.\n"
                << "       Offending slots (up to 8): [" << lidlSlots.mid(0, 8).join(", ")
                << "]\n"
                << "       This emitter reads Qt type names and falls back to QVariant\n"
                << "       (LogosMap on the lp surface) for anything else, so generating\n"
                << "       from this listing would emit a wrapper that compiles and has\n"
                << "       lost every type — with no diagnostic anywhere downstream.\n"
                << "       Pass --events-from <path>/share/logos/" << moduleName
                << ".lidl, the contract\n"
                << "       the module installs beside its plugin. buildHeaders.nix does\n"
                << "       this automatically; a hand-run invocation has to say it.\n";
            loader.unload();
            return 7;
        }
    } else {
        methods = contractMethods;
        out << "Using the module's LIDL contract for the method surface — "
            << methods.size() << " methods (the plugin's published listing is a "
            << "description, not a type source)\n";

        // A stale sidecar is the one way this can now be wrong, and it is
        // otherwise invisible: the wrapper would compile and simply not have
        // the method. Reported, not fatal — the two sets legitimately differ
        // for a plugin whose QMetaObject carries Qt-only slots.
        // INVOKABLE entries only, on both sides. A cdylib publishes its
        // events into the same array, tagged `"type": "event"` and with no
        // `isInvokable` — makeHeader/makeSourceLp already skip those, and
        // counting them here would report a divergence for every module that
        // declares an event.
        auto namesOf = [](const QJsonArray& a) {
            QSet<QString> n;
            for (const QJsonValue& v : a) {
                if (!v.isObject()) continue;
                const QJsonObject o = v.toObject();
                if (!o.value("isInvokable").toBool()) continue;
                n.insert(o.value("name").toString());
            }
            return n;
        };
        const QSet<QString> fromContract = namesOf(contractMethods);
        const QSet<QString> fromPlugin = namesOf(publishedMethods);
        const QStringList onlyContract = QStringList(QList<QString>((fromContract - fromPlugin).begin(), (fromContract - fromPlugin).end()));
        const QStringList onlyPlugin = QStringList(QList<QString>((fromPlugin - fromContract).begin(), (fromPlugin - fromContract).end()));
        if (!onlyContract.isEmpty() || !onlyPlugin.isEmpty()) {
            err << "Note: the contract and the built plugin list different methods for '"
                << moduleName << "'.";
            if (!onlyContract.isEmpty())
                err << " Contract only: [" << onlyContract.join(", ") << "].";
            if (!onlyPlugin.isEmpty())
                err << " Plugin only: [" << onlyPlugin.join(", ") << "].";
            err << " The wrapper follows the CONTRACT; a method listed only by the "
                   "plugin is not reachable through it.\n";
        }
    }

    QString className = toPascalCase(moduleName);
    QString headerRel = QString("%1_api.h").arg(moduleName);
    QString sourceRel = QString("%1_api.cpp").arg(moduleName);
    QString headerAbs = QDir(genDirPath).filePath(headerRel);
    QString sourceAbs = QDir(genDirPath).filePath(sourceRel);

    // Single per-module wrapper file pair. apiStyle decides the
    // signature shape: Qt-typed for legacy / handcrafted callers
    // (default), std-typed and Qt-free when the consuming module's build
    // passed --api-style=lp (typically because it's `interface:
    // "universal"` or `"cdylib"`).
    // Both produce the same filename and class name, so the umbrella
    // doesn't need to know which style was picked. `methods`, `events` and
    // `records` all come from the same sibling `.lidl` sidecar
    // (--events-from) when the module ships one — that is one contract in,
    // one wrapper out, and it is what makes this path agree with
    // `--general-only --dep <name>=<name>.lidl`.
    QString header = makeHeader(moduleName, className, methods, apiStyle, events, BindMode::Static, records);
    QString source = makeSource(moduleName, className, headerRel, methods, apiStyle, events, BindMode::Static, records);

    {
        QFile f(headerAbs);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            err << "Failed to write header: " << headerAbs << "\n";
            loader.unload();
            return 5;
        }
        f.write(header.toUtf8());
        f.close();
    }
    {
        QFile f(sourceAbs);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            err << "Failed to write source: " << sourceAbs << "\n";
            loader.unload();
            return 6;
        }
        f.write(source.toUtf8());
        f.close();
    }

    out << "Generated: " << QDir(genDirPath).filePath(headerRel) << " and " << QDir(genDirPath).filePath(sourceRel) << "\n";
    out.flush();

    loader.unload();
    return 0;
}

int runPluginIntrospectMode(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QTextStream err(stderr);
    QTextStream out(stdout);

    const QStringList args = app.arguments();
    
    // Parse --output-dir option
    QString outputDir;
    const int outDirIdx = args.indexOf("--output-dir");
    if (outDirIdx != -1 && outDirIdx + 1 < args.size()) {
        outputDir = args.at(outDirIdx + 1);
        if (outputDir.startsWith('@')) {
            outputDir.remove(0, 1);
        }
    }

    // `--module-only` is accepted and ignored. The only thing it ever
    // suppressed was the directory-scraping umbrella pair above, which is gone
    // — generator_lib's deps-driven makeUmbrella*FromDeps is the sole umbrella
    // emitter now. generate-module-headers.sh:60 always passes the flag, so it
    // stays tolerated rather than rejected.

    // `--general-only` (the umbrella) is NOT handled here any more: ../main.cpp
    // intercepts it, together with its new `--umbrella` spelling, and runs the
    // one non-legacy implementation. It can only reach runPluginIntrospectMode when it was
    // passed WITHOUT --metadata, which was never a mode — the plugin path
    // below reports it as a missing plugin file, exactly as before.

    // `--api-style qt|lp` — the type surface the generated `<Module>` wrapper
    // exposes. The parser lives in generator_lib next to the ApiStyle enum
    // because ../main.cpp's umbrella mode needs the identical answer; a second
    // copy here is how the two surfaces would drift.
    ApiStyle apiStyle = ApiStyle::Qt;
    if (!parseApiStyleFlag(args, apiStyle, err)) {
        return 1;
    }

    // Support: extract dependencies from a metadata.json file
    {
        const int metaIdx = args.indexOf("--metadata");
        if (metaIdx != -1) {
            if (metaIdx + 1 >= args.size()) {
                err << "Usage: " << QFileInfo(app.applicationFilePath()).fileName() << " --metadata /absolute/path/to/metadata.json [--output-dir /path/to/output] [--module-only] [--general-only]\n";
                return 1;
            }
            QString metaPathArg = args.at(metaIdx + 1);
            if (metaPathArg.startsWith('@')) {
                metaPathArg.remove(0, 1);
            }
            QFileInfo mfi(metaPathArg);
            if (!mfi.exists()) {
                err << "Metadata file does not exist: " << metaPathArg << "\n";
                return 2;
            }
            QString metaResolvedPath = mfi.canonicalFilePath();
            if (metaResolvedPath.isEmpty()) {
                metaResolvedPath = mfi.absoluteFilePath();
            }
            QFile mf(metaResolvedPath);
            if (!mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                err << "Failed to open metadata file: " << metaResolvedPath << "\n";
                return 3;
            }
            const QByteArray jsonData = mf.readAll();
            mf.close();
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(jsonData, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                err << "Invalid metadata JSON in " << metaResolvedPath << ": " << parseError.errorString() << "\n";
                return 4;
            }
            const QJsonObject obj = doc.object();
            const QJsonArray deps = obj.value("dependencies").toArray();

            // `--module-dir` (walk a directory of BUILT plugins and introspect
            // one per dependency) was removed. Every consumer wrapper is now
            // generated from a contract — `--dep <name>=<file.lidl>` inside the
            // `--general-only` branch above, or the single-plugin path below
            // that buildHeaders.nix drives with an explicit plugin argument.
            // Nothing built a modules_dir for this mode any more, and the
            // per-dependency introspection it did is the same "load the .so and
            // read its QMetaObject" step that cannot run under cross-compilation
            // at all.
            //
            // REFUSE it explicitly rather than ignoring it: silently falling
            // through to the dependency LISTING below would exit 0 having
            // generated nothing, which is precisely the shape that lets a stale
            // caller look green while shipping a module with no typed API.
            if (args.contains("--module-dir")) {
                err << "Error: --module-dir was removed. It generated a consumer wrapper per\n"
                    << "       dependency by loading each dependency's BUILT plugin from a\n"
                    << "       modules directory.\n"
                    << "       Use --general-only with one --dep <name>=<path/to/<name>.lidl>\n"
                    << "       per dependency: the wrapper comes from the contract, so no\n"
                    << "       dependency has to be built (and it works under cross-compilation).\n";
                return 2;
            }

            for (const QString& depName : dependencyNames(deps)) {
                out << depName << "\n";
            }
            out.flush();
            return 0;
        }
    }

    // `--provider-header` (the LOGOS_METHOD-marked provider dispatch, i.e.
    // `interface: "provider"`) was removed: every provider now goes through the
    // module-impl C ABI. REFUSE it explicitly rather than letting it fall
    // through — the plugin-path branch below would otherwise read the flag
    // itself as a plugin path and report "Plugin file does not exist:
    // --provider-header", which reads like a missing file rather than a
    // retired mode.
    if (args.contains("--provider-header")) {
        err << "Error: --provider-header was removed. The provider dispatch it generated\n"
            << "       (interface: \"provider\", LOGOS_METHOD markers) is no longer supported.\n"
            << "       Use interface: \"universal\": write a plain src/<name>_impl.h and the\n"
            << "       contract is derived from it automatically.\n";
        return 2;
    }

    if (args.size() < 2) {
        err << "Usage: " << QFileInfo(app.applicationFilePath()).fileName() << " /absolute/path/to/plugin [--output-dir /path/to/output] [--module-only] [--events-from /path/to/<name>.lidl]\n";
        err << "   or:  " << QFileInfo(app.applicationFilePath()).fileName() << " --metadata /absolute/path/to/metadata.json [--output-dir /path/to/output] [--module-only]\n";
        err << "   or:  " << QFileInfo(app.applicationFilePath()).fileName() << " --metadata /absolute/path/to/metadata.json --umbrella (or --general-only) [--output-dir /path/to/output] [--api-style qt|lp] [--interface n=p] [--dep n=p.lidl]\n";
        return 1;
    }

    // --events-from <path>: the module's `.lidl` CONTRACT, shipped beside its
    // built plugin. The flag keeps its name — generate-module-headers.sh and
    // buildHeaders.nix in logos-plugin-qt pass it, and logos-plugin-qt's
    // test-header-generator-guard asserts the spelling — but the file it
    // names has always been the whole contract, and everything the wrapper is
    // generated from now comes out of it: the typed methods, the typed
    // `on<EventName>(callback)` accessors, and the record structs.
    //
    // Absent (a handcrafted Qt module publishes no contract) means the
    // wrapper is generated from the plugin's QMetaObject, exactly as before.
    // NAMED BUT MISSING is a refusal rather than a fallback: silently
    // introspecting instead would emit a wrapper that compiles and is wrong
    // in a way nothing downstream can see.
    QJsonArray methodsFromSidecar;
    QJsonArray eventsFromSidecar;
    QJsonArray recordsFromSidecar;
    {
        const int evIdx = args.indexOf("--events-from");
        QString evPath;
        if (evIdx != -1 && evIdx + 1 < args.size()) {
            evPath = args.at(evIdx + 1);
        } else {
            for (const QString& a : args) {
                if (a.startsWith("--events-from=")) {
                    evPath = a.section('=', 1);
                    break;
                }
            }
        }
        if (!evPath.isEmpty()) {
            if (!QFileInfo(evPath).exists()) {
                err << "Error: --events-from names a contract that does not exist: "
                    << evPath << "\n"
                    << "       The wrapper's methods, events and records all come from\n"
                    << "       this file. Generating from the plugin's published metadata\n"
                    << "       instead would emit a wrapper of untyped QVariant / LogosMap\n"
                    << "       that compiles and silently loses every type.\n";
                return 2;
            }
            if (!loadContractFromLidl(evPath, err, &methodsFromSidecar,
                                      &eventsFromSidecar, &recordsFromSidecar)) {
                return 4;
            }
        }
    }

    QString argPath = args.at(1);
    return generateFromPlugin(argPath, outputDir, apiStyle, eventsFromSidecar, out, err,
                              recordsFromSidecar, methodsFromSidecar);
}
