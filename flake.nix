{
  description = "Intravenous development environment";

  inputs = {
    nixpkgs.url = "https://releases.nixos.org/nixpkgs/nixpkgs-26.11pre1038038.421eebfd0ec7/nixexprs.tar.xz";

    # GCC 16 is intentionally sourced independently from the rest of the
    # development environment. Intravenous uses GCC for production C++26
    # reflection compilation and keeps Clang/LLVM only for source rewriting.
    gcc-reflection-nixpkgs.url = "github:NixOS/nixpkgs/2c423e03bbafcff28bfadc6781a4a8257f205cb5";
  };

  outputs = { nixpkgs, gcc-reflection-nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs {
            inherit system;
            config.allowUnfreePredicate = pkg:
              builtins.elem (pkg.pname or "") [ "claude-code" ];
          };
          reflectionPkgs = import gcc-reflection-nixpkgs { inherit system; };
        in {
          default = pkgs.mkShell {
            packages = with pkgs; [
              llvmPackages_22.clang
              llvmPackages_22.clang-tools
              llvmPackages_22.libllvm.dev
              llvmPackages_22.clang-unwrapped.dev
              cmake
              ninja
              pkg-config
              vscode-extensions.vadimcn.vscode-lldb
              python3
              nodejs
              vsce
              juce
              claude-code

              # JUCE/Linux deps
              alsa-lib
              fontconfig
              freetype
              libGL
              xorg.libX11.dev
              xorg.libXrandr.dev
              xorg.libXinerama.dev
              xorg.libXext.dev
              xorg.libXcursor.dev
            ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
              reflectionPkgs.gcc16
            ];

            shellHook = ''
              ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
                export CC=${reflectionPkgs.gcc16}/bin/gcc
                export CXX=${reflectionPkgs.gcc16}/bin/g++
              ''}
              ${pkgs.lib.optionalString (!pkgs.stdenv.isLinux) ''
                export CC=clang
                export CXX=clang++
              ''}
              export IV_CLANG_CXX=${pkgs.llvmPackages_22.clang}/bin/clang++
              export JUCE_DIR=${pkgs.juce}
              export IV_VST3_PATH="$HOME/vst"

              export PATH="$HOME/.local/bin:$PATH"

              echo "intravenous dev shell ready"
              echo "CC=$CC"
              echo "CXX=$CXX"
              echo "IV_CLANG_CXX=$IV_CLANG_CXX"
              echo "JUCE_DIR=$JUCE_DIR"
              echo "IV_VST3_PATH=$IV_VST3_PATH"
              echo "Configure with: cmake -S . -B build -G Ninja -DJUCE_DIR=$JUCE_DIR"
            '';
          };
        });
    };
}
