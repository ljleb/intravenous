{
  description = "Intravenous development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let pkgs = import nixpkgs { inherit system; };
        in {
          ivctl = pkgs.stdenv.mkDerivation {
            pname = "ivctl";
            version = "0.1.0";
            src = ./src/intravenous/build_cache;
            outputs = [ "out" "dev" ];
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
            buildInputs = [ pkgs.nlohmann_json ];
            postInstall = ''
              install -Dm755 iv-dev-cache "$dev/bin/iv-dev-cache"
            '';
            meta.mainProgram = "ivctl";
            meta.outputsToInstall = [ "out" ];
          };
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvm = pkgs.llvmPackages_23;
          commonPackages = with pkgs; [
            cmake
            ninja
            python3
            pkg-config
            juce
            llvm.clang
            llvm.clang-tools
            llvm.lld
            llvm.llvm
            (lib.getDev llvm.llvm)
            llvm.libclang
            (lib.getDev llvm.clang-unwrapped)
            alsa-lib
            libpulseaudio
            fontconfig
            freetype
            libGL
            libx11.dev
            libxfixes.dev
            libxrandr.dev
            libxinerama.dev
            libxext.dev
            libxcursor.dev
          ];
          devOnlyPackages = with pkgs; [
            vscode-extensions.vadimcn.vscode-lldb
            nodejs
            python3
            perf
            flamegraph
            valgrind
            gdb
            zip
          ];
          shell = packages: pkgs.mkShell {
            inherit packages;
            shellHook = ''
              export IV_DEV_NIXPKGS=${nixpkgs.outPath}
              export CC=${llvm.clang}/bin/clang
              export CXX=${llvm.clang}/bin/clang++
              export IV_PRESENTATION_WASM_COMPILER=${llvm.clang-unwrapped}/bin/clang
              export IV_PRESENTATION_WASM_LINKER=${llvm.lld}/bin/wasm-ld
              export JUCE_DIR=${pkgs.juce}
              export IV_VST3_PATH="$HOME/vst"
              export CMAKE_PREFIX_PATH="${pkgs.lib.getDev llvm.llvm}:${pkgs.lib.getDev llvm.clang-unwrapped}:${pkgs.lib.getLib pkgs.libpulseaudio}:''${CMAKE_PREFIX_PATH:-}"
              export PATH="${self.packages.${system}.ivctl}/bin:${self.packages.${system}.ivctl.dev}/bin:$HOME/.local/bin:$PATH"

              echo "intravenous Clang/LLVM dev shell ready"
              echo "CC=$CC"
              echo "CXX=$CXX"
              echo "JUCE_DIR=$JUCE_DIR"
              echo 'Build with: ./scripts/build_server.sh'
            '';
          };
        in {
          default = shell (commonPackages ++ devOnlyPackages);
          ci = shell commonPackages;
        });
    };
}
