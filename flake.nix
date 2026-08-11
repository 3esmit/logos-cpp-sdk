{
  description = "Logos C++ SDK";

  inputs.logos-nix.url = "github:logos-co/logos-nix";
  inputs.nixpkgs.follows = "logos-nix/nixpkgs";
  # The protocol layer (transports, token exchange, lp_* C ABI). Follows our
  # logos-nix so both repos resolve the identical nixpkgs/Qt pin — the QRO
  # wire is Qt-version-sensitive.
  inputs.logos-protocol.url = "github:logos-co/logos-protocol";
  inputs.logos-protocol.inputs.logos-nix.follows = "logos-nix";
  # The canonical, language-neutral LIDL frontend (lexer/parser/AST/serializer/
  # validator) the code generator links. Follows our logos-nix so it resolves
  # the identical nixpkgs pin.
  inputs.logos-lidl.url = "github:logos-co/logos-lidl";
  inputs.logos-lidl.inputs.logos-nix.follows = "logos-nix";

  outputs = { self, nixpkgs, logos-nix, logos-protocol, logos-lidl }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Adds the "x86_64-windows" pseudo-system; a cross derivation's `system`
      # is its BUILD platform, so it evaluates anywhere and realises on Linux.
      forAllTargets = logos-nix.lib.forAllTargets;

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
      });
    in
    {
      packages = forAllTargets ({ pkgs, ... }: 
        let
          # Common configuration
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
          
          # Individual package components
          bin = import ./nix/bin.nix { inherit pkgs common src logos-protocol; logos-lidl = logos-lidl.packages.${pkgs.system}.logos-lidl; };
          lib = import ./nix/lib.nix { inherit pkgs common src logos-protocol; };
          include = import ./nix/include.nix { inherit pkgs common src logos-protocol; };
          tests = import ./nix/tests.nix { inherit pkgs common src logos-protocol; logos-lidl = logos-lidl.packages.${pkgs.system}.logos-lidl; };
          
          # Combined SDK package. We re-declare propagatedBuildInputs on
          # the join so downstream Nix derivations that depend on the
          # combined `sdk` (rather than the nested `lib`) still inherit
          # OpenSSL / Boost / nlohmann_json — symlinkJoin doesn't
          # forward propagation from its `paths` attribute. Qt is
          # excluded for the same setup-hook ordering reason as in
          # `nix/lib.nix`; consumers must list qt6.qtbase +
          # qt6.wrapQtAppsNoGuiHook themselves.
          sdk = pkgs.symlinkJoin {
            name = "logos-cpp-sdk";
            paths = [ bin lib include ];
            propagatedBuildInputs = common.propagatedBuildInputs;
          };
        in
        {
          # Individual outputs
          logos-cpp-bin = bin;
          logos-cpp-lib = lib;
          logos-cpp-include = include;
          inherit tests;
          
          # Combined outputs (for backward compatibility)
          logos-cpp-sdk = sdk;
          cpp-generator = bin;  # Alias for backward compatibility
          
          # Default package
          default = sdk;
        }
      );

      checks = forAllSystems ({ pkgs }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
          tests = import ./nix/tests.nix { inherit pkgs common src logos-protocol; logos-lidl = logos-lidl.packages.${pkgs.system}.logos-lidl; };
        in
        {
          inherit tests;
        }
      );

      devShells = forAllSystems ({ pkgs }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.gtest
            pkgs.boost
            pkgs.openssl
            pkgs.nlohmann_json
          ];
        };
      });
    };
}
