# Builds the logos-cpp-generator binary
{ pkgs, common, src, logos-protocol, logos-lidl }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-generator";
  # qtbase\'s setup hook errors in qtPreHook unless a wrapper hook ran or
  # this is set; the wrapper hooks are absent on Windows (they cannot even
  # evaluate for a mingw host) and would skip a PE anyway.
  dontWrapQtApps = true;
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs cmakeFlags meta;
  # logos-lidl provides the canonical LIDL frontend the generator links
  # (find_package(logos-lidl) in cpp-generator/CMakeLists.txt).
  buildInputs = common.buildInputs ++ [ logos-lidl ];
  
  # Skip default configure phase since we do it in buildPhase
  dontUseCmakeConfigure = true;
  
  buildPhase = ''
    runHook preBuild
    
    # Build generator
    mkdir -p build-generator
    cd build-generator
    cmake ../cpp-generator -GNinja -DLOGOS_PROTOCOL_ROOT=${logos-protocol} $cmakeFlags
    ninja
    cd ..
    
    runHook postBuild
  '';
  
  installPhase = ''
    runHook preInstall
    
    # Install generator binary.
    #
    # Probe both names and FAIL if neither is there. The unsuffixed-only test
    # this replaces had no else-branch, so a mingw build (which produces
    # logos-cpp-generator.exe) copied nothing, succeeded, and shipped an EMPTY
    # $out/bin -- the failure then surfaced in whichever consumer tried to run
    # the generator, nowhere near the cause.
    mkdir -p $out/bin
    _gen=""
    for _cand in build-generator/bin/logos-cpp-generator build-generator/bin/logos-cpp-generator.exe; do
      if [ -f "$_cand" ]; then _gen="$_cand"; break; fi
    done
    if [ -z "$_gen" ]; then
      echo "Error: logos-cpp-generator was not produced by the build" >&2
      echo "Contents of build-generator/bin:" >&2
      ls -la build-generator/bin 2>&1 >&2 || echo "  (no such directory)" >&2
      exit 1
    fi
    cp "$_gen" $out/bin/

    # Shared C++/Qt codegen backend helpers for logos-qt-sdk's
    # logos-qt-generator: the Qt type-name mapping (lidl_emit_common), the
    # C++ impl-header source parser, and the compat shim that bridges them
    # onto the canonical logos-lidl AST. The frontend itself (lexer/parser/
    # AST/serializer/validator) is NOT distributed here — both generators
    # link logos-lidl for it.
    mkdir -p $out/share/lidl-frontend
    cp cpp-generator/experimental/lidl_compat.h \
       cpp-generator/experimental/impl_header_parser.h cpp-generator/experimental/impl_header_parser.cpp \
       cpp-generator/experimental/lidl_emit_common.h cpp-generator/experimental/lidl_emit_common.cpp \
       cpp-generator/metadata_dependencies.h \
       $out/share/lidl-frontend/

    runHook postInstall
  '';
}

