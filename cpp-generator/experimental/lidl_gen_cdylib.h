#ifndef LIDL_GEN_CDYLIB_H
#define LIDL_GEN_CDYLIB_H

#include "lidl_compat.h"
#include <QString>

// ---------------------------------------------------------------------------
// Cdylib authoring backend — the common module-impl C ABI seam.
//
// Emits, from a module's LIDL contract:
//   1. <name>_module_impl.cpp  — the Qt-FREE C-ABI export wrapper
//      (logos_module_impl.h symbols) around the universal C++ impl class.
//      Compiled into the module's cdylib together with the impl.
//   2. <name>_events_cdylib.cpp — typed `logos_events:` bodies marshalling
//      into nlohmann::json (the cdylib flavor of <name>_events.cpp).
//   3. <name>_cdylib_glue.h/.cpp — the UNIFORM Qt-plugin glue: a
//      LogosProviderObject + LogosProviderPlugin that forwards everything
//      to the cdylib's C ABI. Identical regardless of the module's source
//      language — the Rust SDK's exports plug into the same glue.
//
// Supported types are the std-convertible LIDL subset (tstr/bstr/int/uint/
// float64/bool + arrays thereof) plus LogosMap/LogosList and StdLogosResult
// returns. Types that map to Qt containers (map/any/named/optional, result
// parameters) are rejected at generation time — a cdylib impl is Qt-free by
// definition.
// ---------------------------------------------------------------------------

// Returns false (with *error filled) when the module uses types outside the
// cdylib-supported subset.
bool lidlCdylibSupported(const ModuleDecl& module, QString* error);

// The record structs a contract declares, plus their codec — a Qt-free header
// the author's impl class includes so it can name the structs directly.
// Empty of types (but still valid) when the contract declares no records.
QString lidlMakeTypesHeaderCdylib(const ModuleDecl& module);

QString lidlMakeModuleImplExports(const ModuleDecl& module,
                                  const QString& implClass,
                                  const QString& implHeader);

QString lidlMakeEventsSourceCdylib(const ModuleDecl& module,
                                   const QString& implClass,
                                   const QString& implHeader);


#endif // LIDL_GEN_CDYLIB_H
