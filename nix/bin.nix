# Builds the logos-cpp-generator binary
{ pkgs, common, src, logos-protocol, logos-lidl }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-generator";
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
    
    # Install generator binary
    mkdir -p $out/bin
    if [ -f build-generator/bin/logos-cpp-generator ]; then
      cp build-generator/bin/logos-cpp-generator $out/bin/
    fi

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

