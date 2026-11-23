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
    enum class SymbolicPortBinding : std::uint8_t {
        interface,
        none,
        symbolic_input,
        symbolic_output,
        mixed,
    };

    struct SymbolicContinuousPortInfo {
        std::string name {};
        SymbolicPortConnectivity connectivity =
            SymbolicPortConnectivity::disconnected;
        size_t index = 0;
        std::vector<ContinuousValueIntrospectionMetadata> value_types {};
        std::optional<ChannelTypeId> channel_type {};
        SymbolicPortBinding binding = SymbolicPortBinding::none;

        bool operator==(SymbolicContinuousPortInfo const&) const = default;
    };

    struct SymbolicEventPortInfo {
        std::string name {};
        SymbolicPortConnectivity connectivity =
            SymbolicPortConnectivity::disconnected;
        size_t index = 0;
        EventTypeId type = EventTypeId::empty;
        SymbolicPortBinding binding = SymbolicPortBinding::none;

        bool operator==(SymbolicEventPortInfo const&) const = default;
    };

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

    struct SymbolicNodeMemberInfo {
        size_t index = 0;
        std::string backing_node_id{};
        std::string kind{};
        std::string type_identity{};
        std::string definition_id{};
        std::vector<SymbolicContinuousPortInfo> continuous_inputs{};
        std::vector<SymbolicContinuousPortInfo> continuous_outputs{};
        std::vector<SymbolicEventPortInfo> event_inputs{};
        std::vector<SymbolicEventPortInfo> event_outputs{};
    };

    struct SymbolicNodeInfo {
        std::string id{};
        std::string instance_id{};
        std::string kind{};
        std::string source_identity{};
        std::string type_identity{};
        std::string definition_id{};
        std::vector<LiveSourceSpan> source_spans{};
        std::vector<SymbolicContinuousPortInfo> continuous_inputs{};
        std::vector<SymbolicContinuousPortInfo> continuous_outputs{};
        std::vector<SymbolicEventPortInfo> event_inputs{};
        std::vector<SymbolicEventPortInfo> event_outputs{};
        size_t member_count = 0;
        std::vector<SymbolicNodeMemberInfo> members{};
    };

    struct ProjectQueryResult {
        std::vector<SymbolicNodeInfo> nodes{};
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

    // One asynchronous manual-control compilation job. Times use the server's
    // steady clock and end at preparation or executor acceptance, not at audio
    // activation. Predictive jobs can wait for a gesture after preparation.
    struct ProjectControlCompileStatus {
        std::uint64_t request_id = 0;
        std::uint64_t project_generation = 0;
        std::string phase{};
        bool dynamic = false;
        bool predictive = false;
        std::vector<std::string> inputs{};
        std::uint64_t elapsed_us = 0;
        std::uint64_t queue_us = 0;
        std::uint64_t preparation_us = 0;
        std::uint64_t stage_us = 0;
        std::string message{};
    };

    struct ProjectSymbolicNodesNotification {
        std::vector<SymbolicNodeInfo> nodes{};
        std::vector<std::string> replace_instance_ids{};
    };

    using ProjectNotification = std::variant<
        ProjectMessageNotification,
        ProjectStatusNotification,
        ProjectSymbolicNodesNotification>;
} // namespace iv
