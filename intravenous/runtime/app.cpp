#include "runtime/app.h"

#include "compat.h"
#include "juce/vst_runtime.h"
#include "runtime/handlers.h"
#include "runtime/server_options.h"
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
            auto const options = RuntimeServerOptions::parse(argc, argv);
            SocketRpcServer server(options.workspace_root, options.socket_path);
            RuntimeProjectService service(
                timeline,
                server,
                options.workspace_root,
                std::filesystem::current_path(),
                {}
            );
            std::function<void()> shutdown = [&]() {
                service.request_shutdown();
                server.request_shutdown();
            };
            shutdown_callback = &shutdown;
            install_shutdown_handlers(request_shutdown);

            server.start();
            std::cout << "Intravenous server listening on " << server.socket_path().string() << '\n';
            server.wait();
            service.request_shutdown();
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
