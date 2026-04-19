#pragma once

#include "runtime/project_service.h"
#include "runtime/timeline.h"

#include <chrono>
#include <filesystem>
#include <memory>

namespace iv {
    class SocketRpcServer {
        class Impl;
        std::unique_ptr<Impl> _impl;

    public:
        explicit SocketRpcServer(
            Timeline& timeline,
            std::filesystem::path workspace_root,
            std::filesystem::path discovery_start,
            std::filesystem::path socket_path = {},
            AudioDeviceFactory audio_device_factory = {}
        );
        ~SocketRpcServer();
        SocketRpcServer(SocketRpcServer&&) noexcept;
        SocketRpcServer& operator=(SocketRpcServer&&) noexcept;

        SocketRpcServer(SocketRpcServer const&) = delete;
        SocketRpcServer& operator=(SocketRpcServer const&) = delete;

        void start();
        bool wait_until_ready(std::chrono::milliseconds timeout) const;
        void wait();
        std::filesystem::path socket_path() const;
        void request_shutdown();
    };
}
