cmake -S . -B build         -G Ninja -DJUCE_DIR="D:\Program Files\JUCE"
cmake -S . -B build-release -G Ninja -DJUCE_DIR="D:\Program Files\JUCE" -DCMAKE_BUILD_TYPE=RelWithDebInfo
