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
#include <QtGlobal>
#include "logos_provider_interface.h"
#include "generator_lib.h"
#include "metadata_dependencies.h"
#include "experimental/lidl_compat.h"
#include "lidl_to_json.h"   // ModuleDecl -> the JSON surface generator_lib consumes

// Load events from a `.lidl` sidecar shipped alongside a module's
// pre-built headers. Returns a JSON array of
//   { name, params: [ { name, type } ] }
// using Qt-typed type names — same shape generator_lib's makeHeader /
// makeSource already consume for methods.
static QJsonArray loadEventsFromLidl(const QString& lidlPath, QTextStream& err,
                                     QJsonArray* outRecords = nullptr)
{
    QJsonArray result;
    QFile f(lidlPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        err << "Failed to open events sidecar: " << lidlPath << "\n";
        return result;
    }
    QString source = QString::fromUtf8(f.readAll());
    f.close();

    LidlParseResult pr = lidlParse(source);
    if (pr.hasError()) {
        err << lidlPath << ":" << pr.errorLine << ":" << pr.errorColumn
            << ": " << pr.error << "\n";
        return result;
    }

    noteOptionalPositionalSlots(pr.module, lidlPath, err);
    if (outRecords) *outRecords = moduleRecordsToJson(pr.module);
    return moduleEventsToJson(pr.module);
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

static int generateFromPlugin(const QString& pluginInputPath, const QString& outputDir, ApiStyle apiStyle, const QJsonArray& events, QTextStream& out, QTextStream& err, const QJsonArray& records = {})
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

    QJsonArray methods;
    LogosProviderPlugin* providerPlugin = qobject_cast<LogosProviderPlugin*>(instance);
    if (providerPlugin) {
        LogosProviderObject* provider = providerPlugin->createProviderObject();
        if (provider) {
            methods = provider->getMethods();
            out << "Detected new-API plugin (LogosProviderPlugin), using getMethods() — "
                << methods.size() << " methods\n";
            delete provider;
        } else {
            err << "LogosProviderPlugin::createProviderObject() returned null\n";
        }
    } else {
        methods = enumerateMethods(instance);
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
    // doesn't need to know which style was picked. `events` (loaded
    // from a sibling `.lidl` sidecar via --events-from) adds typed
    // `on<EventName>(callback)` accessors next to the existing methods.
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

    // --events-from <path>: load typed event prototypes from a LIDL
    // sidecar shipped alongside a dep's pre-built headers. When set,
    // the consumer wrapper (<name>_api.{h,cpp}) gains typed
    // `on<EventName>(callback)` accessors next to the existing
    // generic `onEvent(name, callback)` channel.
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
        if (!evPath.isEmpty() && QFileInfo(evPath).exists()) {
            eventsFromSidecar = loadEventsFromLidl(evPath, err, &recordsFromSidecar);
        }
    }

    QString argPath = args.at(1);
    return generateFromPlugin(argPath, outputDir, apiStyle, eventsFromSidecar, out, err, recordsFromSidecar);
}
