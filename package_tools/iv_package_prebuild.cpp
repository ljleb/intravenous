#include <intravenous/module/loader.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 5) {
        std::cerr
            << "usage: iv_package_prebuild <source-root> <artifact-root> <intravenous-source-root>"
               " [artifact-path-file]\n";
        return 2;
    }

    try {
        iv::ModuleLoader loader(argv[3]);
        auto const artifact = loader.compile_package(iv::PackageBuildRequest{
            .package_root = std::filesystem::path(argv[1]),
            .provenance = iv::PackageProvenance::built_in,
            .artifact_root = std::filesystem::path(argv[2]),
        });
        if (argc == 5) {
            std::ofstream artifact_path(argv[4]);
            artifact_path << artifact.path.generic_string();
            artifact_path.close();
            if (!artifact_path) {
                throw std::runtime_error(
                    "cannot write IV package artifact path to '" + std::string(argv[4]) + "'");
            }
        } else {
            std::cout << artifact.path.generic_string() << '\n';
        }
        return 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
