#pragma once


#include <intravenous/node/address.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/node_event_protocol_types.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/devices/audio_device.h>

#include <optional>
#include <string>
#include <vector>

namespace iv {
    class SocketRpcAutosaveStateResultBuilder {
        int error_code = -32000;
        std::string error_message{};
        std::optional<bool> result{};

    public:
        void succeed(bool enabled);
        void fail(std::string message);
        void fail(int code, std::string message);
        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }
        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcSessionProjectStateResultBuilder {
        int error_code = -32000;
        std::string error_message{};
        std::optional<std::vector<ProjectCommand>> result{};

    public:
        void succeed(std::vector<ProjectCommand> value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcNodeEventProtocolResultBuilder {
        int error_code = -32000;
        std::string error_message{};
        std::optional<NodeEventProtocolSnapshot> result{};

    public:
        void succeed(NodeEventProtocolSnapshot value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

    struct SocketRpcNodeMessageDiagnostic {
        NodeAddress target{};
        std::string message{};
    };

    class SocketRpcNodeMessageResultBuilder {
        bool has_response_ = false;
        std::vector<SocketRpcNodeMessageDiagnostic> diagnostics_{};
        int error_code = -32000;
        std::string error_message{};

    public:
        void succeed(std::vector<SocketRpcNodeMessageDiagnostic> diagnostics);
        void fail(std::string message);
        void fail(int code, std::string message);
        [[nodiscard]] bool has_response() const noexcept { return has_response_; }
        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcAckResponseBuilder {
        int error_code = -32000;
        std::string error_message;
        bool has_response_ = false;

    public:
        void succeed() noexcept;
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept { return has_response_; }

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

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

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

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcVirtualNodeResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<VirtualNodeInfo> result;

    public:
        void succeed(VirtualNodeInfo value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcVirtualNodesResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<std::vector<VirtualNodeInfo>> result;

    public:
        void succeed(std::vector<VirtualNodeInfo> value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcCreateNodeInstanceResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<std::string> instance_id;

    public:
        void succeed(std::string created_instance_id);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept
        {
            return instance_id.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcIvPackageDefinitionsResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<std::vector<IvPackageInfo>> result;

    public:
        void succeed(std::vector<IvPackageInfo> value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcIvPackageResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<IvPackageInfo> result;

    public:
        void succeed(IvPackageInfo value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

    class SocketRpcNodeInstancesResultBuilder {
        int error_code = -32000;
        std::string error_message;
        std::optional<std::vector<IvModuleInstanceInfo>> result;

    public:
        void succeed(std::vector<IvModuleInstanceInfo> value);
        void fail(std::string message);
        void fail(int code, std::string message);

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

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

        [[nodiscard]] bool has_response() const noexcept
        {
            return result.has_value() || !error_message.empty();
        }

        [[nodiscard]] std::string build(int request_id) const;
    };

} // namespace iv
