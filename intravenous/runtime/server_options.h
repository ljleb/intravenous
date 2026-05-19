#pragma once

#include "filesystem_paths.h"

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace iv {
    inline std::filesystem::path default_server_socket_path(std::filesystem::path const& workspace_root)
    {
        auto const normalized = normalize_path(workspace_root).generic_string();
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : normalized) {
            hash ^= c;
            hash *= 1099511628211ull;
        }

        auto const dir = std::filesystem::temp_directory_path() / "intravenous";
        std::filesystem::create_directories(dir);
        std::ostringstream out;
        out << std::hex << hash;
        return dir / ("workspace-" + out.str() + ".sock");
    }

    struct RuntimeServerOptions {
        std::filesystem::path workspace_root;
        std::filesystem::path socket_path;

        static RuntimeServerOptions parse(int argc, char** argv)
        {
            RuntimeServerOptions options;
            for (int i = 2; i < argc; ++i) {
                std::string_view const arg = argv[i];
                if (arg == "--workspace-root" && i + 1 < argc) {
                    options.workspace_root = argv[++i];
                } else if (arg == "--socket-path" && i + 1 < argc) {
                    options.socket_path = argv[++i];
                } else {
                    throw std::runtime_error("unknown server argument: " + std::string(arg));
                }
            }

            if (options.workspace_root.empty()) {
                throw std::runtime_error("--server requires --workspace-root <path>");
            }
            if (options.socket_path.empty()) {
                options.socket_path = default_server_socket_path(options.workspace_root);
            }
            return options;
        }
    };
}
