#include "runtime/app.h"

#include "compat.h"
#include "juce/vst_runtime.h"
#include "runtime/handlers.h"
#include "runtime/socket_rpc_server.h"
#include "runtime/timeline.h"

#include <filesystem>
#include <functional>
#include <iostream>

namespace iv {
    namespace {
        std::function<void()>* shutdown_callback = nullptr;

        void request_shutdown()
        {
            if (shutdown_callback) {
                (*shutdown_callback)();
            }
        }

        int run_server_mode(Timeline& timeline, int argc, char** argv)
        {
            std::filesystem::path workspace_root;
            std::filesystem::path socket_path;

            for (int i = 2; i < argc; ++i) {
                std::string_view const arg = argv[i];
                if (arg == "--workspace-root" && i + 1 < argc) {
                    workspace_root = argv[++i];
                } else if (arg == "--socket-path" && i + 1 < argc) {
                    socket_path = argv[++i];
                } else {
                    throw std::runtime_error("unknown server argument: " + std::string(arg));
                }
            }

            if (workspace_root.empty()) {
                throw std::runtime_error("--server requires --workspace-root <path>");
            }

            SocketRpcServer server(timeline, workspace_root, std::filesystem::current_path(), socket_path);
            std::function<void()> shutdown = [&]() {
                server.request_shutdown();
            };
            shutdown_callback = &shutdown;
            install_shutdown_handlers(request_shutdown);

            server.start();
            std::cout << "Intravenous server listening on " << server.socket_path().string() << '\n';
            server.wait();
            shutdown_callback = nullptr;
            return 0;
        }
    }

    int run_intravenous_cli(int argc, char** argv)
    {
        install_crash_handlers();
#if IV_ENABLE_JUCE_VST
        warmup_juce_vst_scan_cache();
#endif
        Timeline timeline;

        if (argc >= 2 && std::string_view(argv[1]) == "--server") {
            return run_server_mode(timeline, argc, argv);
        }
        throw std::runtime_error("intravenous runs as a server; use --server --workspace-root <path>");
    }
}
