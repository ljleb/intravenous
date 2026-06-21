#pragma once

#include <intravenous/runtime/lane_view_service.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/devices/audio_device.h>

#include <optional>
#include <string>
#include <vector>

namespace iv {
    class SocketRpcAckResponseBuilder {
        int error_code = -32000;
        std::string error_message;

    public:
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcGraphQueryResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<ProjectQueryResult> result;

    public:
        void succeed(ProjectQueryResult value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcRegionQueryResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<ProjectRegionQueryResult> result;

    public:
        void succeed(ProjectRegionQueryResult value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcLogicalNodeResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<LogicalNodeInfo> result;

    public:
        void succeed(LogicalNodeInfo value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcLogicalNodesResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<std::vector<LogicalNodeInfo>> result;

    public:
        void succeed(std::vector<LogicalNodeInfo> value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcCreateIvModuleInstanceResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<std::string> instance_id;

    public:
        void succeed(std::string created_instance_id);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcAudioDevicesResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<AudioDevicesSnapshot> result;

    public:
        void succeed(AudioDevicesSnapshot value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcLaneViewResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<LaneViewResult> result;

    public:
        void succeed(LaneViewResult value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] std::string build(int request_id) const;
    };
} // namespace iv
