{
  description = "Intravenous development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvm = pkgs.llvmPackages_23;
          commonPackages = with pkgs; [
            cmake
            ninja
            pkg-config
            juce
            llvm.clang
            llvm.clang-tools
            llvm.llvm
            (lib.getDev llvm.llvm)
            llvm.libclang
            (lib.getDev llvm.clang-unwrapped)
            alsa-lib
            fontconfig
            freetype
            libGL
            libx11.dev
            libxrandr.dev
            libxinerama.dev
            libxext.dev
            libxcursor.dev
          ];
          devOnlyPackages = with pkgs; [
            vscode-extensions.vadimcn.vscode-lldb
            nodejs
            vsce
            perf
            flamegraph
            valgrind
            gdb
          ];
          shell = packages: pkgs.mkShell {
            inherit packages;
            shellHook = ''
              export CC=${llvm.clang}/bin/clang
              export CXX=${llvm.clang}/bin/clang++
              export JUCE_DIR=${pkgs.juce}
              export IV_VST3_PATH="$HOME/vst"
              export CMAKE_PREFIX_PATH="${pkgs.lib.getDev llvm.llvm}:${pkgs.lib.getDev llvm.clang-unwrapped}:''${CMAKE_PREFIX_PATH:-}"
              export PATH="$HOME/.local/bin:$PATH"

              echo "intravenous Clang/LLVM dev shell ready"
              echo "CC=$CC"
              echo "CXX=$CXX"
              echo "JUCE_DIR=$JUCE_DIR"
              echo "Configure with: cmake -S . -B build -G Ninja -DJUCE_DIR=$JUCE_DIR"
            '';
          };
        in {
          default = shell (commonPackages ++ devOnlyPackages);
          ci = shell commonPackages;
        });
    };
}
