#ifndef LIDL_GEN_CLIENT_H
#define LIDL_GEN_CLIENT_H

#include "lidl_compat.h"
#include "../generator_lib.h"  // BindMode
#include <QString>
#include <QTextStream>

// Map a LIDL TypeExpr to the Qt type string used in generated client stubs
QString lidlTypeToQt(const TypeExpr& te);

// Convert "my_module" to "MyModule"
QString lidlToPascalCase(const QString& name);

// Generate the client API header (.h) from a ModuleDecl.
// `bindMode == Bound` emits an interface wrapper whose ctor takes the
// target module name at runtime (see BindMode in generator_lib.h); the
// default Static keeps the historical fixed-module wrapper.
QString lidlMakeHeader(const ModuleDecl& module, BindMode bindMode = BindMode::Static);

// Generate the client API source (.cpp) from a ModuleDecl
QString lidlMakeSource(const ModuleDecl& module, BindMode bindMode = BindMode::Static);

// Generate metadata.json content from a ModuleDecl
QString lidlGenerateMetadataJson(const ModuleDecl& module);

// Full pipeline: parse .lidl file, generate client stubs + metadata.json + optional umbrella
// Returns 0 on success, non-zero on error
int lidlGenerateClientStubs(const QString& lidlPath, const QString& outputDir,
                            bool moduleOnly, QTextStream& out, QTextStream& err);

#endif // LIDL_GEN_CLIENT_H
