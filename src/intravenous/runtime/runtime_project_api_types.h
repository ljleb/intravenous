#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/runtime/lane_view_service.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iv {
    struct SourcePosition {
        uint32_t line = 1;
        uint32_t column = 1;

        bool operator==(SourcePosition const &) const = default;
    };

    struct SourceRange {
        SourcePosition start{};
        SourcePosition end{};

        bool operator==(SourceRange const &) const = default;
    };

    enum class SourceRangeMatchMode {
        intersection,
        union_,
    };

    struct LiveSourceSpan {
        std::string file_path{};
        SourceRange range{};

        bool operator==(LiveSourceSpan const &) const = default;
    };

    struct LogicalNodeMemberInfo {
        size_t ordinal = 0;
        std::string backing_node_id{};
        std::string kind{};
        std::string type_identity{};
        std::vector<LogicalPortInfo> sample_inputs{};
        std::vector<LogicalPortInfo> sample_outputs{};
        std::vector<LogicalPortInfo> event_inputs{};
        std::vector<LogicalPortInfo> event_outputs{};
    };

    struct LogicalNodeInfo {
        std::string id{};
        std::string instance_id{};
        std::string kind{};
        std::string source_identity{};
        std::string type_identity{};
        std::vector<LiveSourceSpan> source_spans{};
        std::vector<LogicalPortInfo> sample_inputs{};
        std::vector<LogicalPortInfo> sample_outputs{};
        std::vector<LogicalPortInfo> event_inputs{};
        std::vector<LogicalPortInfo> event_outputs{};
        size_t member_count = 0;
        std::vector<LogicalNodeMemberInfo> members{};
    };

    // Source-annotated public inputs are not graph nodes, but the source
    // sidebar presents them using the same logical/member shape.
    struct PublicSampleInputInfo {
        std::string instance_id {};
        std::string source_identity {};
        std::vector<SourceInfo> source_infos {};
        std::string name {};
        Sample default_value = 0.0f;
        std::optional<Sample> min {};
        std::optional<Sample> max {};
        Sample current_value = 0.0f;
        std::string logical_state {};
        std::vector<size_t> member_ordinals {};
    };

    struct ProjectQueryResult {
        std::vector<LogicalNodeInfo> nodes{};
    };

    struct ProjectRegionQueryResult {
        std::vector<LiveSourceSpan> source_spans{};
    };

    struct ProjectMessageNotification {
        std::string level = "info";
        std::string message{};
        std::filesystem::path module_root{};
    };

    struct ProjectStatusNotification {
        std::string level = "info";
        std::string code{};
        std::string message{};
        std::filesystem::path module_root{};
        std::vector<std::string> created_node_ids{};
        std::vector<std::string> deleted_node_ids{};
    };

    struct ProjectLaneViewNotification {
        LaneViewResult lane_view{};
    };

    struct ProjectLogicalNodesNotification {
        std::vector<LogicalNodeInfo> nodes{};
        std::vector<std::string> replace_instance_ids{};
    };

    using ProjectNotification = std::variant<
        ProjectMessageNotification,
        ProjectStatusNotification,
        ProjectLaneViewNotification,
        ProjectLogicalNodesNotification>;
} // namespace iv
