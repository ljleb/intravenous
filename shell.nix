{ pkgs ? import <nixpkgs> { config.allowUnfreePredicate = pkg: builtins.elem (pkg.pname or "") [ "claude-code" ]; } }:

pkgs.mkShell {
  packages = with pkgs; [
    cmake
    ninja
    pkg-config
    clang
    clang-tools
    llvmPackages_latest.libllvm.dev
    llvmPackages_latest.clang-unwrapped.dev
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
  ];

  shellHook = ''
    export CC=clang
    export CXX=clang++
    export JUCE_DIR=${pkgs.juce}
    export IV_VST3_PATH="$HOME/vst"

    export PATH="$HOME/.local/bin:$PATH"

    echo "intravenous dev shell ready"
    echo "CC=$CC"
    echo "CXX=$CXX"
    echo "JUCE_DIR=$JUCE_DIR"
    echo "IV_VST3_PATH=$IV_VST3_PATH"
    echo "Configure with: cmake -S . -B build -DJUCE_DIR=$JUCE_DIR"
  '';
}
