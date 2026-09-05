{
  description = "Intravenous development environment";

  inputs = {
    nixpkgs.url = "https://releases.nixos.org/nixpkgs/nixpkgs-26.11pre1038038.421eebfd0ec7/nixexprs.tar.xz";

    # GCC 16 is intentionally sourced independently from the rest of the
    # development environment for C++26 reflection and the GCC plugin API.
    gcc-reflection-nixpkgs.url = "github:NixOS/nixpkgs/2c423e03bbafcff28bfadc6781a4a8257f205cb5";

    # Reflection-capable clang/clangd fork used only by the development shell.
    clang-reflection-nixpkgs.url = "github:cadkin/nixpkgs/p2996";
  };

  outputs = { nixpkgs, gcc-reflection-nixpkgs, clang-reflection-nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          reflectionPkgs = import gcc-reflection-nixpkgs { inherit system; };

          # Everything required to configure, compile, and test Intravenous.
          ciPackages = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.juce
            reflectionPkgs.gcc16
            reflectionPkgs.gmp.dev
            reflectionPkgs.mpfr.dev
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            # JUCE/Linux dependencies.
            pkgs.alsa-lib
            pkgs.fontconfig
            pkgs.freetype
            pkgs.libGL
            pkgs.libx11.dev
            pkgs.libxrandr.dev
            pkgs.libxinerama.dev
            pkgs.libxext.dev
            pkgs.libxcursor.dev
          ];

          # Interactive/editor/debugging tools that CI does not need.
          clangReflectionPkgs = import clang-reflection-nixpkgs { inherit system; };
          clangd-p2996 = pkgs.writeShellScriptBin "clangd-p2996" ''
            export CPLUS_INCLUDE_PATH="${clangReflectionPkgs.llvmPackages_p2996.libcxx.dev}/include/c++/v1..."
            exec ${clangReflectionPkgs.llvmPackages_p2996.clang-tools}/bin/clangd \
              --query-driver=${reflectionPkgs.gcc16}/bin/g++ \
              "$@"
          '';
          devOnlyPackages = [
            clangd-p2996
            pkgs.vscode-extensions.vadimcn.vscode-lldb
            pkgs.nodejs
            pkgs.vsce
            pkgs.valgrind
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            pkgs.linuxPackages.perf
            pkgs.flamegraph
          ];

          shellEnvironment = {
            CC = "${reflectionPkgs.gcc16}/bin/gcc";
            CXX = "${reflectionPkgs.gcc16}/bin/g++";
            JUCE_DIR = "${pkgs.juce}";
          };
        in {
          ci = pkgs.mkShell (shellEnvironment // {
            packages = ciPackages;
          });

          default = pkgs.mkShell (shellEnvironment // {
            packages = ciPackages ++ devOnlyPackages;

            shellHook = ''
              export IV_VST3_PATH="$HOME/vst"
              export PATH="$HOME/.local/bin:$PATH"

              echo "intravenous dev shell ready"
              echo "CC=$CC"
              echo "CXX=$CXX"
              echo "JUCE_DIR=$JUCE_DIR"
              echo "IV_VST3_PATH=$IV_VST3_PATH"
              echo "Configure with: cmake -S . -B build -G Ninja -DJUCE_DIR=$JUCE_DIR"
            '';
          });
        });
    };
}
