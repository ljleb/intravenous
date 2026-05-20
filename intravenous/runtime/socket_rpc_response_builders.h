#pragma once

#include <string>

namespace iv {
    class SocketRpcAckResponseBuilder {
        int error_code = -32000;
        std::string error_message;

    public:
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcInitializeResultBuilder {
    public:
        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcGraphQueryResultBuilder {
    public:
        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcRegionQueryResultBuilder {
    public:
        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcLogicalNodeResultBuilder {
    public:
        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcLogicalNodesResultBuilder {
    public:
        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcLaneViewResultBuilder {
    public:
        [[nodiscard]] std::string build(int request_id) const;
    };
} // namespace iv
