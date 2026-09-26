#pragma once

#include <intravenous/graph/build_types.h>

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

    struct VirtualNodeMemberInfo {
        size_t index = 0;
        std::string backing_node_id{};
        std::string kind{};
        std::string type_identity{};
        std::vector<VirtualPortInfo> sample_inputs{};
        std::vector<VirtualPortInfo> sample_outputs{};
        std::vector<VirtualPortInfo> event_inputs{};
        std::vector<VirtualPortInfo> event_outputs{};
    };

    struct VirtualNodeInfo {
        std::string id{};
        std::string instance_id{};
        std::string kind{};
        std::string source_identity{};
        std::string type_identity{};
        std::vector<LiveSourceSpan> source_spans{};
        std::vector<VirtualPortInfo> sample_inputs{};
        std::vector<VirtualPortInfo> sample_outputs{};
        std::vector<VirtualPortInfo> event_inputs{};
        std::vector<VirtualPortInfo> event_outputs{};
        size_t member_count = 0;
        std::vector<VirtualNodeMemberInfo> members{};
    };

    struct ProjectQueryResult {
        std::vector<VirtualNodeInfo> nodes{};
    };

    struct ProjectRegionQueryResult {
        std::vector<LiveSourceSpan> source_spans{};
    };

    struct ProjectMessageNotification {
        std::string level = "info";
        std::string message{};
        std::filesystem::path package_root{};
    };

    struct ProjectStatusNotification {
        std::string level = "info";
        std::string code{};
        std::string message{};
        std::filesystem::path package_root{};
        std::vector<std::string> created_node_ids{};
        std::vector<std::string> deleted_node_ids{};
    };

    struct ProjectVirtualNodesNotification {
        std::vector<VirtualNodeInfo> nodes{};
        std::vector<std::string> replace_instance_ids{};
    };

    using ProjectNotification = std::variant<
        ProjectMessageNotification,
        ProjectStatusNotification,
        ProjectVirtualNodesNotification>;
} // namespace iv
