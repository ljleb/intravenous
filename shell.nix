{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  packages = with pkgs; [
    cmake
    ninja
    pkg-config
    clang
    clang-tools
    llvmPackages_latest.libllvm.dev
    llvmPackages_latest.clang-unwrapped.dev
    python3
    juce

    # JUCE/Linux deps
    alsa-lib
    fontconfig
    freetype
    libGL
  ];

  shellHook = ''
    export CC=clang
    export CXX=clang++
    export JUCE_DIR=${pkgs.juce}
    export IV_VST3_PATH="$HOME/vst"

    echo "intravenous dev shell ready"
    echo "CC=$CC"
    echo "CXX=$CXX"
    echo "JUCE_DIR=$JUCE_DIR"
    echo "IV_VST3_PATH=$IV_VST3_PATH"
    echo "Configure with: cmake -S . -B build -DJUCE_DIR=$JUCE_DIR"
  '';
}
