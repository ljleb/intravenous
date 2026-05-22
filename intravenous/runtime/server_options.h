#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace iv {
    struct RuntimeServerOptions {
        std::filesystem::path workspace_root;
        int rpc_fd = -1;

        static RuntimeServerOptions parse(int argc, char** argv)
        {
            RuntimeServerOptions options;
            for (int i = 2; i < argc; ++i) {
                std::string_view const arg = argv[i];
                if (arg == "--workspace-root" && i + 1 < argc) {
                    options.workspace_root = argv[++i];
                } else if (arg == "--rpc-fd" && i + 1 < argc) {
                    char *end = nullptr;
                    long const value = std::strtol(argv[++i], &end, 10);
                    if (end == nullptr || *end != '\0' || value < 0) {
                        throw std::runtime_error("--rpc-fd must be a non-negative integer");
                    }
                    options.rpc_fd = static_cast<int>(value);
                } else {
                    throw std::runtime_error("unknown server argument: " + std::string(arg));
                }
            }

            if (options.workspace_root.empty()) {
                throw std::runtime_error("--server requires --workspace-root <path>");
            }
            if (options.rpc_fd < 0) {
                throw std::runtime_error("--server requires --rpc-fd <fd>");
            }
            return options;
        }
    };
}
