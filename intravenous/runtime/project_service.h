#pragma once

#include "graph/build_types.h"
#include "devices/audio_device.h"
#include "runtime/timeline.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iv {
    struct SourcePosition {
        uint32_t line = 1;
        uint32_t column = 1;

        bool operator==(SourcePosition const&) const = default;
    };

    struct SourceRange {
        SourcePosition start {};
        SourcePosition end {};

        bool operator==(SourceRange const&) const = default;
    };

    enum class SourceRangeMatchMode {
        intersection,
        union_,
    };

    struct LiveSourceSpan {
        std::string file_path {};
        SourceRange range {};

        bool operator==(LiveSourceSpan const&) const = default;
    };

    struct LogicalNodeInfo {
        struct Member {
            size_t ordinal = 0;
            std::string backing_node_id {};
            std::string kind {};
            std::string type_identity {};
            std::vector<LogicalPortInfo> sample_inputs {};
            std::vector<LogicalPortInfo> sample_outputs {};
            std::vector<LogicalPortInfo> event_inputs {};
            std::vector<LogicalPortInfo> event_outputs {};
        };

        std::string id {};
        std::string kind {};
        std::string source_identity {};
        std::string type_identity {};
        std::vector<LiveSourceSpan> source_spans {};
        std::vector<LogicalPortInfo> sample_inputs {};
        std::vector<LogicalPortInfo> sample_outputs {};
        std::vector<LogicalPortInfo> event_inputs {};
        std::vector<LogicalPortInfo> event_outputs {};
        size_t member_count = 0;
        std::vector<Member> members {};
    };

    struct RuntimeProjectInitializeResult {
        std::filesystem::path module_root {};
        std::string module_id {};
        uint64_t execution_epoch = 0;
    };

    struct RuntimeProjectQueryResult {
        uint64_t execution_epoch = 0;
        std::vector<LogicalNodeInfo> nodes {};
    };

    struct RuntimeProjectRegionQueryResult {
        uint64_t execution_epoch = 0;
        std::vector<LiveSourceSpan> source_spans {};
    };

    enum class RuntimeProjectNotificationKind {
        message,
        status,
    };

    struct RuntimeProjectNotification {
        RuntimeProjectNotificationKind kind = RuntimeProjectNotificationKind::message;
        std::string level = "info";
        std::string code {};
        std::string message {};
        std::filesystem::path module_root {};
        uint64_t execution_epoch = 0;
        std::vector<std::string> created_node_ids {};
        std::vector<std::string> deleted_node_ids {};
    };

    using AudioDeviceFactory = std::function<std::optional<LogicalAudioDevice>()>;
    using RuntimeProjectNotificationSink = std::function<void(RuntimeProjectNotification const&)>;

    class RuntimeProjectService {
        class Impl;
        std::unique_ptr<Impl> _impl;

    public:
        explicit RuntimeProjectService(
            Timeline& timeline,
            std::filesystem::path workspace_root,
            std::filesystem::path discovery_start,
            std::vector<std::filesystem::path> extra_search_roots = {},
            AudioDeviceFactory audio_device_factory = {},
            RuntimeProjectNotificationSink notification_sink = {}
        );
        ~RuntimeProjectService();
        RuntimeProjectService(RuntimeProjectService&&) noexcept;
        RuntimeProjectService& operator=(RuntimeProjectService&&) noexcept;

        RuntimeProjectService(RuntimeProjectService const&) = delete;
        RuntimeProjectService& operator=(RuntimeProjectService const&) = delete;

        RuntimeProjectInitializeResult initialize();
        RuntimeProjectQueryResult query_by_spans(
            std::filesystem::path const& file_path,
            std::vector<SourceRange> const& ranges,
            SourceRangeMatchMode match_mode = SourceRangeMatchMode::intersection
        ) const;
        RuntimeProjectRegionQueryResult query_active_regions(
            std::filesystem::path const& file_path
        ) const;
        LogicalNodeInfo get_logical_node(uint64_t execution_epoch, std::string const& node_id) const;
        std::vector<LogicalNodeInfo> get_logical_nodes(uint64_t execution_epoch, std::vector<std::string> const& node_ids) const;
        void set_sample_input_value(
            uint64_t execution_epoch,
            std::string const& node_id,
            size_t input_ordinal,
            Sample value,
            std::optional<size_t> member_ordinal = std::nullopt
        );
        void clear_sample_input_value_override(
            uint64_t execution_epoch,
            std::string const& node_id,
            size_t member_ordinal,
            size_t input_ordinal
        );
        void request_shutdown();
    };
}
