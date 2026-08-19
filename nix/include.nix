# Installs the Qt-free base SDK headers in the source-export layout
# ($out/include/cpp/...). The transport/protocol sources ship from
# logos-protocol and the Qt developer layer from logos-qt-sdk; build
# systems that need those add the respective include roots.
{ pkgs, common, src, logos-protocol }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-headers";
  # qtbase\'s setup hook errors in qtPreHook unless a wrapper hook ran or
  # this is set; the wrapper hooks are absent on Windows (they cannot even
  # evaluate for a mingw host) and would skip a PE anyway.
  dontWrapQtApps = true;
  version = common.version;

  inherit src;
  inherit (common) meta;

  dontBuild = true;
  dontConfigure = true;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/include/cpp

    # logos_lp_client.h MUST sit in the same directory as the headers it
    # includes (logos_result.h, logos_json.h). A cdylib module's generated
    # dep wrapper includes "logos_lp_client.h" via the include/cpp source-
    # export root, and a quoted include resolves siblings relative to the
    # including file. If logos_lp_client.h lived only at the top-level
    # include/ (the CMake-export layout) while logos_result.h is reached via
    # include/cpp/, a single TU would pull logos_result.h through two
    # distinct realpaths and #pragma once could not dedup them
    # (redefinition of StdLogosResult). Ship every std header in BOTH roots.
    for file in logos_module_context.h logos_json.h logos_result.h logos_lp_client.h logos_async_result.h logos_host_services.h logos_host_core.h; do
      cp cpp/$file $out/include/cpp/
      cp cpp/$file $out/include/
    done

    runHook postInstall
  '';
}
