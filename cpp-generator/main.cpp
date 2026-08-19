#include "plugin_introspect.h"
#include "generator_lib.h"
#include "lidl_to_json.h"
#include "experimental/lidl_gen_client.h"
#include "experimental/lidl_gen_cdylib.h"
#include "experimental/lidl_compat.h"
#include "experimental/impl_header_parser.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <QVector>

// ─── Umbrella mode (`--umbrella`, alias `--general-only`) ────────────────────
//
// Emits the umbrella — `logos_sdk.h` / `logos_sdk.cpp`, i.e. `struct
// LogosModules` — over a module's declared `metadata.json#dependencies` plus
// its interface dependencies, and the per-dependency / per-interface wrappers
// those aggregate.
//
// This is NOT a legacy mode, despite having lived in `plugin_introspect.cpp` until
// now: `LogosModuleContext::modules()` returns `LogosModules&`, so every
// `interface: "universal"` module that calls a declared dependency goes
// through it, and LogosModule.cmake runs it for every module build. Only the
// QPluginLoader-introspection path in `plugin_introspect.cpp` is legacy.
//
// `--general-only` is kept as an exact alias — it is what LogosModule.cmake,
// buildPlugin.nix and buildHeaders.nix all pass today — so there is ONE
// implementation of the mode and no second copy to drift.

// A single interface to generate a bound wrapper for. `path` is already
// resolved (nix resolves local `${src}/file` and remote `${input}/file`
// store paths and passes them via --interface; the generator never touches
// flake inputs). `implClass` is required for `.h` files, empty for `.lidl`.
struct InterfaceSpec {
    QString name;       // interface identifier → class/file name + bind_<name>
    QString path;       // resolved path to the .lidl / .h definition
    QString implClass;  // class inside a .h whose API defines the interface
};

// Parse all `<flag> <name>=<path>[=<impl_class>]` (or `<flag>=<name>=...`)
// occurrences. Names and store paths contain no '=', so splitting on the
// first two '=' is unambiguous. Used for both `--interface` (runtime-bound
// wrappers) and `--dep` (name-baked wrappers generated from a dep's LIDL).
static QVector<InterfaceSpec> parseSpecFlags(const QStringList& args, const QString& flag)
{
    const QString flagEq = flag + "=";
    QVector<InterfaceSpec> specs;
    for (int i = 0; i < args.size(); ++i) {
        QString value;
        if (args.at(i) == flag && i + 1 < args.size()) {
            value = args.at(i + 1);
        } else if (args.at(i).startsWith(flagEq)) {
            value = args.at(i).section('=', 1);
        } else {
            continue;
        }
        const int firstEq = value.indexOf('=');
        if (firstEq <= 0) continue;  // need at least name=path
        InterfaceSpec spec;
        spec.name = value.left(firstEq);
        const int secondEq = value.indexOf('=', firstEq + 1);
        if (secondEq < 0) {
            spec.path = value.mid(firstEq + 1);
        } else {
            spec.path = value.mid(firstEq + 1, secondEq - firstEq - 1);
            spec.implClass = value.mid(secondEq + 1);
        }
        specs.append(spec);
    }
    return specs;
}

// Parse an interface definition file into a ModuleDecl. `.lidl` parses
// directly; `.h`/`.hpp` go through the impl-header parser, which needs a
// metadata.json — we feed it a synthetic one carrying only the interface
// name so the consumer's identity and events are NOT pulled in (the
// interface's events come solely from the file's own `logos_events:` block).
static bool parseInterfaceFile(const InterfaceSpec& spec, const QString& genDirPath,
                               ModuleDecl& outMod, QTextStream& err)
{
    QFileInfo fi(spec.path);
    if (!fi.exists()) {
        err << "Interface file not found for '" << spec.name << "': " << spec.path << "\n";
        return false;
    }
    const QString ext = fi.suffix().toLower();
    if (ext == "lidl") {
        QFile f(spec.path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            err << "Failed to open interface file: " << spec.path << "\n";
            return false;
        }
        const QString src = QString::fromUtf8(f.readAll());
        f.close();
        LidlParseResult pr = lidlParse(src);
        if (pr.hasError()) {
            err << spec.path << ":" << pr.errorLine << ":" << pr.errorColumn
                << ": " << pr.error << "\n";
            return false;
        }
        outMod = pr.module;
        return true;
    }
    if (ext == "h" || ext == "hpp") {
        if (spec.implClass.isEmpty()) {
            err << "Interface '" << spec.name << "' is a C++ header but no impl_class was given "
                << "(metadata.json interface_dependencies entry needs \"impl_class\")\n";
            return false;
        }
        // Synthetic minimal metadata: name only, no events — keeps the
        // consumer's identity/events out of the interface.
        const QString synthMeta = QDir(genDirPath).filePath("." + spec.name + "_iface_meta.json");
        {
            QFile mf(synthMeta);
            if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                err << "Failed to write temporary interface metadata: " << synthMeta << "\n";
                return false;
            }
            mf.write(QString("{\"name\":\"%1\"}").arg(spec.name).toUtf8());
            mf.close();
        }
        ImplParseResult pr = parseImplHeader(spec.path, spec.implClass, synthMeta, err);
        QFile::remove(synthMeta);
        if (pr.hasError()) {
            err << "Error parsing interface header " << spec.path << ": " << pr.error << "\n";
            return false;
        }
        outMod = pr.module;
        return true;
    }
    err << "Unsupported interface file type for '" << spec.name << "': " << spec.path
        << " (expected .lidl or .h)\n";
    return false;
}

// Generate a wrapper (`<name>_api.{h,cpp}`) per spec from its definition file.
// The wrapper class is named from the spec `name` (PascalCase), NOT the
// definition file's internal module name, so it matches the `#include` the
// umbrella header emits. `bindMode` picks the wrapper flavour:
//   Bound  — interface dependency: ctor takes a runtime module name; exposed
//            via a `bind_<name>(...)` factory on the umbrella.
//   Static — concrete dependency: the module name is baked in; exposed as a
//            `<name>` member on the umbrella (byte-identical to the wrapper the
//            dep's prebuilt headers used to ship).
static bool generateInterfaceWrappers(const QVector<InterfaceSpec>& ifaces,
                                      const QString& genDirPath, ApiStyle apiStyle,
                                      QTextStream& out, QTextStream& err,
                                      BindMode bindMode = BindMode::Bound)
{
    for (const InterfaceSpec& spec : ifaces) {
        ModuleDecl mod;
        if (!parseInterfaceFile(spec, genDirPath, mod, err)) return false;

        {
            QString recErr;
            if (!lidlCheckRecords(mod, &recErr)) {
                err << spec.path << ": " << recErr << "\n";
                return false;
            }
        }

        noteOptionalPositionalSlots(mod, spec.path, err);

        const QString className = toPascalCase(spec.name);
        const QJsonArray methods = moduleMethodsToJson(mod);
        const QJsonArray events  = moduleEventsToJson(mod);
        const QJsonArray records = moduleRecordsToJson(mod);
        const QString headerRel = spec.name + "_api.h";
        const QString sourceRel = spec.name + "_api.cpp";

        const QString header = makeHeader(spec.name, className, methods, apiStyle, events, bindMode, records);
        const QString source = makeSource(spec.name, className, headerRel, methods, apiStyle, events, bindMode, records);

        {
            QFile f(QDir(genDirPath).filePath(headerRel));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                err << "Failed to write wrapper header: " << headerRel << "\n";
                return false;
            }
            f.write(header.toUtf8());
        }
        {
            QFile f(QDir(genDirPath).filePath(sourceRel));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                err << "Failed to write wrapper source: " << sourceRel << "\n";
                return false;
            }
            f.write(source.toUtf8());
        }
        out << "Generated " << (bindMode == BindMode::Bound ? "bound interface" : "dependency")
            << " wrapper: " << headerRel << " (class " << className << ", "
            << methods.size() << " methods, " << events.size() << " events)\n";
    }
    out.flush();
    return true;
}

// The mode proper. `progName` is only used in the usage diagnostic.
static int runUmbrellaMode(const QStringList& args, const QString& progName,
                           QTextStream& out, QTextStream& err)
{
    // Some build drivers pass paths as `@/abs/path`.
    auto stripAt = [](QString p) { if (p.startsWith('@')) p.remove(0, 1); return p; };

    QString outputDir;
    const int outDirIdx = args.indexOf("--output-dir");
    if (outDirIdx != -1 && outDirIdx + 1 < args.size()) {
        outputDir = stripAt(args.at(outDirIdx + 1));
    }

    // `--api-style qt|lp` — the one parser, shared with runPluginIntrospectMode's plugin
    // path (generator_lib.h, next to the ApiStyle enum).
    ApiStyle apiStyle = ApiStyle::Qt;
    if (!parseApiStyleFlag(args, apiStyle, err)) return 1;

    // `--binding api|origin` — the umbrella's transport binding (generator_lib.h,
    // next to the UmbrellaBinding enum).
    UmbrellaBinding binding = UmbrellaBinding::FromApi;
    if (!parseUmbrellaBindingFlag(args, binding, err)) return 1;

    // With the Qt surface, `origin` means the per-dependency wrappers are
    // logos-qt-generator's (`--backend consumer --binding origin`) and this
    // run emits the UMBRELLA ONLY. The wrapper emitter reached below is the
    // legacy Qt one, whose every constructor takes a LogosAPI — writing those
    // next to an origin-bound umbrella would put two mutually incompatible
    // wrapper flavours in one output directory, and the umbrella's members
    // would not compile against them. Skipping is the honest outcome, and it
    // is said out loud rather than inferred from an empty directory.
    //
    // Interface NAMES are still collected below, and still drive the
    // `bind_<name>(...)` factories; only the wrapper FILES are skipped.
    const bool skipWrappers =
        (apiStyle == ApiStyle::Qt && binding == UmbrellaBinding::ExplicitOrigin);

    const int metaIdx = args.indexOf("--metadata");
    if (metaIdx == -1 || metaIdx + 1 >= args.size()) {
        err << "Usage: " << progName
            << " --metadata /absolute/path/to/metadata.json --umbrella (or --general-only)"
               " [--output-dir /path/to/output] [--api-style qt|lp] [--binding api|origin]"
               " [--interface <name>=<file.lidl|file.h>[=<ImplClass>]]"
               " [--dep <name>=<file.lidl>]\n";
        return 1;
    }
    const QString metaPathArg = stripAt(args.at(metaIdx + 1));
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

    // `LogosModules` exposes ONLY the modules listed in
    // `metadata.json#dependencies` — apps that need to manage the core use
    // liblogos' C API directly.
    const QString genDirPath = outputDir.isEmpty()
        ? QDir::current().filePath("logos-cpp-sdk/cpp/generated")
        : outputDir;
    QDir().mkpath(genDirPath);

    // Collect interface dependencies. Primary source: --interface flags (nix
    // resolves both local `${src}/file` and remote `${input}/file` store paths
    // and passes them here, so the generator never touches flake inputs).
    // Fallback: self-resolve LOCAL interface_dependencies entries (those
    // without an `input`) from metadata.json, relative to the metadata dir —
    // covers non-nix / source-tree builds. Flags win on collision.
    // Dedup --interface flags by name and drop malformed specs: a repeated
    // interface name would emit duplicate #include "<name>_api.h" /
    // bind_<name>(...) into logos_sdk.h and fail to compile, and an empty
    // name/path can only fail later in a less actionable way.
    QVector<InterfaceSpec> ifaceSpecs;
    QSet<QString> haveIface;
    for (const InterfaceSpec& sp : parseSpecFlags(args, "--interface")) {
        if (sp.name.isEmpty() || sp.path.isEmpty()) {
            err << "Ignoring malformed --interface spec (empty name or path)\n";
            continue;
        }
        if (haveIface.contains(sp.name)) {
            err << "Ignoring duplicate --interface '" << sp.name << "'\n";
            continue;
        }
        haveIface.insert(sp.name);
        ifaceSpecs.append(sp);
    }

    const QString metaDir = QFileInfo(metaResolvedPath).absolutePath();
    const QJsonArray ifaceDeps = obj.value("interface_dependencies").toArray();
    for (const QJsonValue& v : ifaceDeps) {
        if (!v.isObject()) continue;
        const QJsonObject eo = v.toObject();
        const QString name = eo.value("name").toString();
        if (name.isEmpty() || haveIface.contains(name)) continue;
        // Entries with an `input` reference another repo (flake input); only
        // nix can resolve those, via a --interface flag. If we reach here
        // without a matching flag, skip.
        if (eo.contains("input")) {
            err << "Note: interface '" << name << "' has an 'input' (cross-repo) "
                << "but no --interface flag was passed; skipping (nix supplies the path).\n";
            continue;
        }
        const QString file = eo.value("file").toString();
        if (file.isEmpty()) continue;
        InterfaceSpec spec;
        spec.name = name;
        spec.path = QDir(metaDir).filePath(file);
        spec.implClass = eo.value("impl_class").toString();
        ifaceSpecs.append(spec);
        haveIface.insert(name);
    }

    // Generate one bound wrapper (<name>_api.{h,cpp}) per interface.
    if (!ifaceSpecs.isEmpty() && !skipWrappers) {
        if (!generateInterfaceWrappers(ifaceSpecs, genDirPath, apiStyle, out, err)) {
            return 9;
        }
    } else if (!ifaceSpecs.isEmpty()) {
        err << "Note: --binding origin — emitting the umbrella only. The "
            << ifaceSpecs.size() << " interface wrapper(s) must come from "
            << "logos-qt-generator --backend consumer --bind bound --binding origin.\n";
    }

    // Concrete dependencies generated from their published LIDL
    // (`--dep <name>=<lidl>`). Same backend as interfaces but BindMode::Static
    // — the module name is baked in and the dep is exposed as a `<dep>` MEMBER
    // (the umbrella already emits it from `dependencies`, so no umbrella
    // change). nix passes `--dep` only for deps that publish a `lidl` output;
    // deps without one fall back to the header-copy path and are NOT passed
    // here. Dedup vs each other and vs interface names.
    QVector<InterfaceSpec> depSpecs;
    QSet<QString> haveDep;
    for (const InterfaceSpec& sp : parseSpecFlags(args, "--dep")) {
        if (sp.name.isEmpty() || sp.path.isEmpty()) {
            err << "Ignoring malformed --dep spec (empty name or path)\n";
            continue;
        }
        if (haveIface.contains(sp.name)) {
            err << "Ignoring --dep '" << sp.name << "' (name already used by an interface)\n";
            continue;
        }
        if (haveDep.contains(sp.name)) {
            err << "Ignoring duplicate --dep '" << sp.name << "'\n";
            continue;
        }
        haveDep.insert(sp.name);
        depSpecs.append(sp);
    }
    if (!depSpecs.isEmpty() && !skipWrappers) {
        if (!generateInterfaceWrappers(depSpecs, genDirPath, apiStyle, out, err, BindMode::Static)) {
            return 9;
        }
    } else if (!depSpecs.isEmpty()) {
        err << "Note: --binding origin — emitting the umbrella only. The "
            << depSpecs.size() << " dependency wrapper(s) must come from "
            << "logos-qt-generator --backend consumer --bind static --binding origin.\n";
    }

    QStringList interfaceNames;
    for (const InterfaceSpec& sp : ifaceSpecs) interfaceNames.append(sp.name);

    // The umbrella itself. Emission lives in generator_lib next to the
    // per-module wrapper emitters, so the aggregate can be asserted on without
    // a filesystem (tests/generator/test_make_umbrella.cpp); this only writes
    // what those return. For the Lp (Qt-free) flavor the umbrella bakes this
    // module's name as the lp_client origin.
    const QString originName = obj.value("name").toString();
    // The origin is this module's OWN name, and with `--binding origin` it is
    // the only thing standing between a generated wrapper and calling out under
    // somebody else's identity. Refuse at the CLI as well as in the emitter
    // (which writes an `#error`): failing here names the metadata file, which
    // is where the fix is.
    if (binding == UmbrellaBinding::ExplicitOrigin && originName.isEmpty()) {
        err << "--binding origin needs the consuming module's own name, and "
            << metaResolvedPath << " declares no \"name\". The origin is asserted, "
            << "never derived from a caller.\n";
        return 6;
    }
    const QDir genDir(genDirPath);
    {
        QFile outFile(genDir.filePath("logos_sdk.h"));
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            err << "Failed to write umbrella header: " << outFile.fileName() << "\n";
            return 7;
        }
        outFile.write(makeUmbrellaHeaderFromDeps(deps, interfaceNames, apiStyle, originName, binding).toUtf8());
        outFile.close();
    }
    {
        QFile outFile(genDir.filePath("logos_sdk.cpp"));
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            err << "Failed to write umbrella source: " << outFile.fileName() << "\n";
            return 8;
        }
        outFile.write(makeUmbrellaSourceFromDeps(deps, interfaceNames).toUtf8());
        outFile.close();
    }

    out << "Generated logos_sdk.h and logos_sdk.cpp\n";
    out.flush();
    return 0;
}

int main(int argc, char* argv[])
{
    // Check for --lidl / --from-header / --header-to-lidl mode before
    // initializing QCoreApplication, since runPluginIntrospectMode creates its own.
    bool hasLidl = false;
    bool hasFromHeader = false;
    bool hasHeaderToLidl = false;
    bool hasUmbrella = false;
    bool hasGeneralOnly = false;
    bool hasMetadata = false;
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromUtf8(argv[i]);
        if (arg == "--lidl") hasLidl = true;
        if (arg == "--from-header") hasFromHeader = true;
        if (arg == "--header-to-lidl") hasHeaderToLidl = true;
        if (arg == "--umbrella") hasUmbrella = true;
        if (arg == "--general-only") hasGeneralOnly = true;
        if (arg == "--metadata") hasMetadata = true;
    }

    // Umbrella mode. `--general-only` routes here too — ONE implementation,
    // no second copy in plugin_introspect.cpp to drift — but only in the shape
    // runPluginIntrospectMode ever honoured it: inside the `--metadata` branch. Without
    // `--metadata` the flag was never a mode at all (it fell through to the
    // plugin path and reported the flag itself as a missing plugin file), so
    // that case still falls through, unchanged.
    if (hasUmbrella || (hasGeneralOnly && hasMetadata)) {
        QCoreApplication app(argc, argv);
        QTextStream err(stderr);
        QTextStream out(stdout);
        return runUmbrellaMode(app.arguments(),
                               QFileInfo(app.applicationFilePath()).fileName(),
                               out, err);
    }

    // --header-to-lidl: the C++ frontend of the source -> LIDL -> bindings
    // pipeline. Parse an impl header and emit ONLY its LIDL contract (no Qt
    // glue / dispatch), so a module can publish a cheap `lidl` artifact that
    // consumers (any language) turn into bindings without building the module.
    if (hasHeaderToLidl) {
        QCoreApplication app(argc, argv);
        QTextStream err(stderr);
        QTextStream out(stdout);
        const QStringList args = app.arguments();

        // Strip a leading '@' from path arguments — some build drivers pass
        // `@/abs/path`. Matches the runPluginIntrospectMode path handling.
        auto stripAt = [](QString p) { if (p.startsWith('@')) p.remove(0, 1); return p; };

        const int idx = args.indexOf("--header-to-lidl");
        if (idx + 1 >= args.size()) {
            err << "Error: --header-to-lidl requires a path to the impl header\n";
            return 1;
        }
        const QString headerPath = stripAt(args.at(idx + 1));

        const int implClassIdx = args.indexOf("--impl-class");
        if (implClassIdx == -1 || implClassIdx + 1 >= args.size()) {
            err << "Error: --header-to-lidl requires --impl-class <ClassName>\n";
            return 1;
        }
        const QString implClass = args.at(implClassIdx + 1);

        const int metadataIdx = args.indexOf("--metadata");
        if (metadataIdx == -1 || metadataIdx + 1 >= args.size()) {
            err << "Error: --header-to-lidl requires --metadata <metadata.json>\n";
            return 1;
        }
        const QString metadataPath = stripAt(args.at(metadataIdx + 1));

        ImplParseResult pr = parseImplHeader(headerPath, implClass, metadataPath, err);
        if (pr.hasError()) {
            err << "Error parsing impl header: " << pr.error << "\n";
            return 4;
        }
        const ModuleDecl& mod = pr.module;

        // Output path: explicit -o/--output <file>, else <output-dir>/<name>.lidl,
        // else <name>.lidl in the CWD.
        QString outPath;
        const int oIdx = args.indexOf("-o");
        const int outputIdx = args.indexOf("--output");
        const int outDirIdx = args.indexOf("--output-dir");
        if (oIdx != -1 && oIdx + 1 < args.size()) {
            outPath = stripAt(args.at(oIdx + 1));
        } else if (outputIdx != -1 && outputIdx + 1 < args.size()) {
            outPath = stripAt(args.at(outputIdx + 1));
        } else if (outDirIdx != -1 && outDirIdx + 1 < args.size()) {
            const QString d = stripAt(args.at(outDirIdx + 1));
            QDir().mkpath(d);
            outPath = QDir(d).filePath(qs(mod.name) + ".lidl");
        } else {
            outPath = qs(mod.name) + ".lidl";
        }

        QFile f(outPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            err << "Failed to write LIDL: " << outPath << "\n";
            return 5;
        }
        f.write(lidlSerialize(mod).toUtf8());
        f.close();
        out << "Generated LIDL: " << outPath << " (" << mod.methods.size()
            << " methods, " << mod.events.size() << " events)\n";
        out.flush();
        return 0;
    }

    if (hasLidl || hasFromHeader) {
        QCoreApplication app(argc, argv);
        QTextStream err(stderr);
        QTextStream out(stdout);
        const QStringList args = app.arguments();

        QString outputDir;
        const int outDirIdx = args.indexOf("--output-dir");
        if (outDirIdx != -1 && outDirIdx + 1 < args.size()) {
            outputDir = args.at(outDirIdx + 1);
        }

        // --from-header mode: parse C++ impl header directly (no .lidl needed)
        if (hasFromHeader) {
            const int fromHeaderIdx = args.indexOf("--from-header");
            if (fromHeaderIdx + 1 >= args.size()) {
                err << "Error: --from-header requires a path to the impl header\n";
                return 1;
            }
            QString headerPath = args.at(fromHeaderIdx + 1);

            const int implClassIdx = args.indexOf("--impl-class");
            if (implClassIdx == -1 || implClassIdx + 1 >= args.size()) {
                err << "Error: --from-header requires --impl-class <ClassName>\n";
                return 1;
            }
            QString implClass = args.at(implClassIdx + 1);

            const int metadataIdx = args.indexOf("--metadata");
            if (metadataIdx == -1 || metadataIdx + 1 >= args.size()) {
                err << "Error: --from-header requires --metadata <metadata.json>\n";
                return 1;
            }
            QString metadataPath = args.at(metadataIdx + 1);

            // --impl-header: the include path for generated code (defaults to header filename)
            QString implHeader;
            const int implHeaderIdx = args.indexOf("--impl-header");
            if (implHeaderIdx != -1 && implHeaderIdx + 1 < args.size()) {
                implHeader = args.at(implHeaderIdx + 1);
            } else {
                implHeader = QFileInfo(headerPath).fileName();
            }

            const int backendIdx = args.indexOf("--backend");
            if (backendIdx == -1 || backendIdx + 1 >= args.size()) {
                err << "Error: --from-header requires --backend cdylib\n";
                return 1;
            }
            QString backend = args.at(backendIdx + 1);

            // Parse the impl header
            ImplParseResult pr = parseImplHeader(headerPath, implClass, metadataPath, err);
            if (pr.hasError()) {
                err << "Error parsing impl header: " << pr.error << "\n";
                return 4;
            }

            const ModuleDecl& mod = pr.module;
            QString genDirPath = outputDir.isEmpty()
                ? QDir::current().filePath("generated")
                : outputDir;
            QDir().mkpath(genDirPath);

            if (backend == "cdylib") {
                // Cdylib authoring: the common module-impl C ABI exports +
                // the uniform Qt-plugin glue (language-agnostic, forwards to
                // the C symbols). See logos_module_impl.h in logos-protocol.
                QString cdErr;
                if (!lidlCdylibSupported(mod, &cdErr)) {
                    err << "Error: module not cdylib-eligible: " << cdErr << "\n";
                    return 10;
                }
                struct Out { QString file; QString content; };
                QList<Out> outs;
                // Always emitted, records or not: the exports TU and the events
                // sidecar both reference the generated codec, and a module with
                // no `type` decls still has containers to encode.
                outs.append({qs(mod.name) + "_types.h", lidlMakeTypesHeaderCdylib(mod)});
                outs.append({qs(mod.name) + "_module_impl.cpp",
                             lidlMakeModuleImplExports(mod, implClass, implHeader)});
                if (!mod.events.empty())
                    outs.append({qs(mod.name) + "_events_cdylib.cpp",
                                 lidlMakeEventsSourceCdylib(mod, implClass, implHeader)});
                outs.append({qs(mod.name) + ".lidl", lidlSerialize(mod)});
                for (const Out& o : outs) {
                    const QString abs = QDir(genDirPath).filePath(o.file);
                    QFile f(abs);
                    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                        err << "Failed to write: " << abs << "\n";
                        return 11;
                    }
                    f.write(o.content.toUtf8());
                    out << "Generated: " << abs << "\n";
                }
                out.flush();
                return 0;
            }

            if (backend == "qt") {
                err << "Error: --backend qt was removed. A module is a plain "
                       "shared library: emit the module-impl C ABI with "
                       "--backend cdylib, then turn that into a Qt plugin with "
                       "logos-qt-host-generator --backend cdylib (logos-plugin-qt). "
                       "This tool keeps the Qt-free outputs (--header-to-lidl "
                       "emits the .lidl sidecar).\n";
                return 6;
            }

            err << "Error: --from-header supports --backend cdylib (Qt plugin packaging: logos-qt-host-generator)\n";
            return 1;
        }

        // --lidl mode
        const int lidlIdx = args.indexOf("--lidl");
        if (lidlIdx + 1 >= args.size()) {
            err << "Usage: " << QFileInfo(app.applicationFilePath()).fileName()
                << " --lidl /path/to/module.lidl [--output-dir /path] [--module-only]\n"
                << "       " << QFileInfo(app.applicationFilePath()).fileName()
                << " --lidl /path/to/module.lidl --backend cdylib [--output-dir /path]   (glue-only: C exports come from the module's own language backend)\n"
                << "       " << QFileInfo(app.applicationFilePath()).fileName()
                << " --from-header src/impl.h --backend cdylib --metadata metadata.json [--output-dir /path]\n";
            return 1;
        }
        QString lidlPath = args.at(lidlIdx + 1);

        // Backend dispatch. `qt` is refused above: a module is a plain shared
        // library, and Qt-plugin packaging is logos-qt-host-generator's job.
        const int backendIdx = args.indexOf("--backend");
        if (backendIdx != -1) {
            if (backendIdx + 1 >= args.size()) {
                err << "Error: --backend requires an argument (supported: cdylib)\n";
                return 1;
            }
            QString backend = args.at(backendIdx + 1);

            const int implClassIdx = args.indexOf("--impl-class");
            const int implHeaderIdx = args.indexOf("--impl-header");

            // Cdylib-from-LIDL (contract-first):
            //   - with no --impl-class: GLUE-ONLY — the C exports come from
            //     the module's own language backend (e.g. the Rust SDK's
            //     lidl-gen --provider); the glue only knows the C symbols.
            //   - with --impl-class/--impl-header: the FULL set — the C-ABI
            //     export wrapper around the named (hand-written, Qt-free)
            //     C++ impl class, plus the same uniform glue. The contract
            //     stays the .lidl; the author just implements the class.
            if (backend == "cdylib") {
                QFile f(lidlPath);
                if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    err << "Error: cannot read " << lidlPath << "\n";
                    return 1;
                }
                LidlParseResult pr = lidlParse(QString::fromUtf8(f.readAll()));
                if (pr.hasError()) {
                    err << "Error parsing " << lidlPath << ": " << pr.error
                        << " (line " << pr.errorLine << ")\n";
                    return 4;
                }
                const ModuleDecl& mod = pr.module;
                QString cdErr;
                if (!lidlCdylibSupported(mod, &cdErr)) {
                    err << "Error: module not cdylib-eligible: " << cdErr << "\n";
                    return 10;
                }
                QString genDirPath = outputDir.isEmpty()
                    ? QDir::current().filePath("generated")
                    : outputDir;
                QDir().mkpath(genDirPath);
                struct Out { QString file; QString content; };
                QList<Out> outs;
                if (implClassIdx != -1) {
                    if (implClassIdx + 1 >= args.size()) {
                        err << "Error: --impl-class requires a class name\n";
                        return 1;
                    }
                    const QString implClass = args.at(implClassIdx + 1);
                    QString implHeader;
                    if (implHeaderIdx != -1 && implHeaderIdx + 1 < args.size())
                        implHeader = args.at(implHeaderIdx + 1);
                    else
                        implHeader = qs(mod.name) + "_impl.h";
                    outs.append({qs(mod.name) + "_types.h", lidlMakeTypesHeaderCdylib(mod)});
                    outs.append({qs(mod.name) + "_module_impl.cpp",
                                 lidlMakeModuleImplExports(mod, implClass, implHeader)});
                    if (!mod.events.empty())
                        outs.append({qs(mod.name) + "_events_cdylib.cpp",
                                     lidlMakeEventsSourceCdylib(mod, implClass, implHeader)});
                } else {
                    err << "Error: the uniform cdylib Qt glue is generated by "
                           "logos-qt-host-generator --backend cdylib "
                           "(logos-plugin-qt); this tool emits the Qt-free "
                           "C-ABI export wrapper, which requires "
                           "--impl-class.\n";
                    return 12;
                }
                for (const Out& o : outs) {
                    const QString abs = QDir(genDirPath).filePath(o.file);
                    QFile of(abs);
                    if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                        err << "Failed to write: " << abs << "\n";
                        return 11;
                    }
                    of.write(o.content.toUtf8());
                    out << "Generated: " << abs << "\n";
                }
                out.flush();
                return 0;
            }

            // The backend is not cdylib (that branch returned above), so the
            // only thing left to do is refuse. This must come BEFORE any
            // --impl-class / --impl-header validation: those flags cannot
            // rescue a removed backend, and reporting them first told a user
            // typing `--backend qt` that qt would work if they passed one more
            // flag.
            if (backend == "qt") {
                err << "Error: --backend qt was removed. Qt-PLUGIN (provider) glue "
                       "generation moved to logos-qt-host-generator --backend cdylib "
                       "(logos-plugin-qt), on top of the C ABI this tool emits with "
                       "--backend cdylib. logos-qt-generator (logos-qt-sdk) owns only "
                       "--backend consumer and --backend ui, and refuses this flag too.\n";
                return 6;
            }

            err << "Error: unsupported backend '" << backend << "' (supported: cdylib)\n";
            return 1;
        }

        // Client stub mode (default)
        bool moduleOnly = args.contains("--module-only");
        return lidlGenerateClientStubs(lidlPath, outputDir, moduleOnly, out, err);
    }

    return runPluginIntrospectMode(argc, argv);
}
