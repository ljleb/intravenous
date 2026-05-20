#pragma once

#include "devices/audio_device.h"
#include "graph/build_types.h"
#include "module/dependency.h"
#include "runtime/config.h"
#include "runtime/lane_graph.h"
#include "runtime/lane_view_service.h"
#include "runtime/runtime_project_api_types.h"
#include "runtime/timeline.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iv {
class SocketRpcServer;

struct SourceTextLineMap {
    std::string text;
    std::vector<size_t> line_offsets;

    static SourceTextLineMap from_file(std::filesystem::path const &path);
    size_t offset_for(SourcePosition position) const;
    SourcePosition position_for(size_t offset) const;
};

using AudioDeviceFactory = std::function<std::optional<LogicalAudioDevice>()>;

struct LoadedGraphIntrospectionIndex {
    std::filesystem::path module_root;
    std::string module_id;
    std::vector<IntrospectionLogicalNode> logical_nodes;
    std::unordered_map<std::string, size_t> logical_node_index_by_id;
};

class NodeExecutor;

class RuntimeProjectService {
    Timeline &timeline;
    SocketRpcServer *server = nullptr;
    std::filesystem::path workspace_root;
    std::filesystem::path discovery_start;
    std::vector<std::filesystem::path> extra_search_roots;
    AudioDeviceFactory audio_device_factory;

    mutable std::mutex mutex;
    mutable std::unordered_map<std::string, SourceTextLineMap> source_text_cache;
    std::condition_variable initialized_cv;

    std::optional<RuntimeProjectConfig> config;
    std::optional<LoadedGraphIntrospectionIndex> graph_index;
    LaneViewService lane_views;
    std::exception_ptr pending_exception;
    bool initialized = false;
    bool shutdown_requested = false;

    std::optional<std::jthread> runtime_thread;
    NodeExecutor *executor_state = nullptr;

    void emit_notification(RuntimeProjectNotification notification);
    void emit_message(std::string level, std::string message);
    void emit_status(
        std::string code,
        std::string level,
        std::string message,
        std::filesystem::path module_root = {},
        std::vector<std::string> created_node_ids = {},
        std::vector<std::string> deleted_node_ids = {});
    void rethrow_if_failed() const;
    SourceTextLineMap const &
    source_text_for(std::string const &normalized_path) const;
    void invalidate_source_text(std::string const &normalized_path);
    void invalidate_source_texts(std::span<ModuleDependency const> dependencies);
    std::pair<uint32_t, uint32_t>
    byte_range_for(std::string const &normalized_path, SourceRange const &range) const;
    LiveSourceSpan to_live_span(SourceSpan const &span) const;
    LogicalNodeInfo to_logical_node(IntrospectionLogicalNode const &node) const;
    LaneQueryResult query_lanes_locked(
        LaneQueryFilter const &filter,
        std::optional<size_t> start_index = std::nullopt,
        std::optional<size_t> visible_lane_count = std::nullopt) const;
    void configure_lane_views();
    void run_runtime();

public:
    explicit RuntimeProjectService(
        Timeline &timeline, std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots = {},
        AudioDeviceFactory audio_device_factory = {});
    explicit RuntimeProjectService(
        Timeline &timeline, SocketRpcServer &server,
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots = {},
        AudioDeviceFactory audio_device_factory = {});
    ~RuntimeProjectService();
    RuntimeProjectService(RuntimeProjectService &&) = delete;
    RuntimeProjectService &operator=(RuntimeProjectService &&) = delete;

    RuntimeProjectService(RuntimeProjectService const &) = delete;
    RuntimeProjectService &operator=(RuntimeProjectService const &) = delete;

    RuntimeProjectInitializeResult initialize();
    RuntimeProjectQueryResult
    query_by_spans(
        std::filesystem::path const &file_path,
        std::vector<SourceRange> const &ranges,
        SourceRangeMatchMode match_mode = SourceRangeMatchMode::intersection) const;
    RuntimeProjectRegionQueryResult
    query_active_regions(std::filesystem::path const &file_path) const;
    LogicalNodeInfo get_logical_node(std::string const &node_id) const;
    std::vector<LogicalNodeInfo>
    get_logical_nodes(std::vector<std::string> const &node_ids) const;
    LaneViewResult open_lane_view(LaneViewRequest request);
    LaneViewResult update_lane_view(LaneViewRequest request);
    void close_lane_view(std::string const &view_id);
    void set_sample_input_value(
        std::string const &node_id, size_t input_ordinal,
        Sample value,
        std::optional<size_t> member_ordinal = std::nullopt);
    void clear_sample_input_value_override(
        std::string const &node_id,
        size_t member_ordinal,
        size_t input_ordinal);
    void request_shutdown();
};
} // namespace iv
