#include "legacy/legacy_main.h"
#include "experimental/lidl_gen_client.h"
#include "experimental/lidl_gen_cdylib.h"
#include "experimental/lidl_compat.h"
#include "experimental/impl_header_parser.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

int main(int argc, char* argv[])
{
    // Check for --lidl / --from-header / --header-to-lidl mode before
    // initializing QCoreApplication, since legacy_main creates its own.
    bool hasLidl = false;
    bool hasFromHeader = false;
    bool hasHeaderToLidl = false;
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromUtf8(argv[i]);
        if (arg == "--lidl") hasLidl = true;
        if (arg == "--from-header") hasFromHeader = true;
        if (arg == "--header-to-lidl") hasHeaderToLidl = true;
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
        // `@/abs/path`. Matches the legacy_main path handling.
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
                err << "Error: --from-header requires --backend <qt>\n";
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
                err << "Error: Qt glue generation moved to logos-qt-generator "
                       "(logos-qt-sdk). Use it for --backend qt; this tool "
                       "keeps the Qt-free outputs (--header-to-lidl emits the "
                       ".lidl sidecar).\n";
                return 6;
            }

            err << "Error: --from-header supports --backend cdylib (Qt glue: logos-qt-generator)\n";
            return 1;
        }

        // --lidl mode
        const int lidlIdx = args.indexOf("--lidl");
        if (lidlIdx + 1 >= args.size()) {
            err << "Usage: " << QFileInfo(app.applicationFilePath()).fileName()
                << " --lidl /path/to/module.lidl [--output-dir /path] [--module-only]\n"
                << "       " << QFileInfo(app.applicationFilePath()).fileName()
                << " --lidl /path/to/module.lidl --backend qt --impl-class Class --impl-header header.h [--output-dir /path]\n"
                << "       " << QFileInfo(app.applicationFilePath()).fileName()
                << " --lidl /path/to/module.lidl --backend cdylib [--output-dir /path]   (glue-only: C exports come from the module's own language backend)\n"
                << "       " << QFileInfo(app.applicationFilePath()).fileName()
                << " --from-header src/impl.h --backend qt --impl-class Class --metadata metadata.json [--output-dir /path]\n";
            return 1;
        }
        QString lidlPath = args.at(lidlIdx + 1);

        // Provider glue mode: --backend qt --impl-class X --impl-header Y
        const int backendIdx = args.indexOf("--backend");
        if (backendIdx != -1) {
            if (backendIdx + 1 >= args.size()) {
                err << "Error: --backend requires an argument (e.g., qt)\n";
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
                           "logos-qt-generator (logos-qt-sdk); this tool emits "
                           "the Qt-free C-ABI export wrapper, which requires "
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

            if (implClassIdx == -1 || implClassIdx + 1 >= args.size()) {
                err << "Error: --backend " << backend << " requires --impl-class <ClassName>\n";
                return 1;
            }
            if (implHeaderIdx == -1 || implHeaderIdx + 1 >= args.size()) {
                err << "Error: --backend " << backend << " requires --impl-header <header.h>\n";
                return 1;
            }

            QString implClass = args.at(implClassIdx + 1);
            QString implHeader = args.at(implHeaderIdx + 1);

            if (backend == "qt") {
                err << "Error: Qt glue generation moved to logos-qt-generator "
                       "(logos-qt-sdk).\n";
                return 6;
            }

            err << "Error: unsupported backend '" << backend << "' (supported: cdylib)\n";
            return 1;
        }

        // Client stub mode (default)
        bool moduleOnly = args.contains("--module-only");
        return lidlGenerateClientStubs(lidlPath, outputDir, moduleOnly, out, err);
    }

    return legacy_main(argc, argv);
}
