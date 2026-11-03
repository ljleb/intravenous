#include <intravenous/module/loader.h>

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr
            << "usage: iv_package_prebuild <source-root> <artifact-root> <intravenous-source-root>\n";
        return 2;
    }

    try {
        iv::ModuleLoader loader(argv[3]);
        auto const artifact = loader.compile_package(iv::PackageBuildRequest{
            .package_root = std::filesystem::path(argv[1]),
            .provenance = iv::PackageProvenance::built_in,
            .artifact_root = std::filesystem::path(argv[2]),
        });
        std::cout << artifact.generic_string() << '\n';
        return 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
