# Common build configuration shared across all packages
{ pkgs }:

{
  pname = "logos-cpp-sdk";
  version = "0.2.0";
  
  # Common native build inputs
  nativeBuildInputs = [ 
    pkgs.cmake 
    pkgs.ninja 
    pkgs.pkg-config
  ]
  ++ pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows) pkgs.qt6.wrapQtAppsNoGuiHook;
  
  # Common runtime dependencies
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.boost                # Boost.Asio for plain-C++ TCP transports
    pkgs.openssl              # TLS for TcpSsl
    pkgs.nlohmann_json        # Wire message JSON codec
  ];

  # Subset of buildInputs that is safe to propagate to downstream
  # consumers via `propagatedBuildInputs`. Excludes Qt: qtbase's
  # setup-hook fires `qtPreHook` which errors unless
  # `wrapQtAppsHook` (or the no-GUI variant) was sourced first, and
  # propagation order through nixpkgs (propagatedNativeBuildInputs vs
  # propagatedBuildInputs) can't reliably guarantee that ordering. So
  # consumers must list `qtbase` + `wrapQtAppsNoGuiHook` themselves;
  # this set carries the rest (OpenSSL, Boost, nlohmann_json) so they
  # don't have to retype it just to satisfy `find_dependency(...)`
  # inside the SDK's CMake Config or to link against the SDK's
  # transitively-required symbols (e.g.
  # `boost::asio::ssl::host_name_verification`).
  propagatedBuildInputs = [
    pkgs.boost
    pkgs.openssl
    pkgs.nlohmann_json
  ];
  
  # Common CMake flags
  cmakeFlags = [ "-GNinja" ]
    # Qt's host TOOLS (repc, moc, qmltyperegistrar) live in separate packages
    # that must RUN on the build machine; logos-nix's Windows overlay exposes
    # the flags pointing Qt at them. Absent -- so empty -- on native builds.
    ++ (pkgs.logosQtCrossCmakeFlags or [ ]);
  
  # Metadata
  meta = with pkgs.lib; {
    description = "Logos C++ SDK Library and Code Generator";
    platforms = platforms.unix ++ platforms.windows;
  };
}

