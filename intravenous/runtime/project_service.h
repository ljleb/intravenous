#pragma once

#include "devices/audio_device.h"
#include "graph/build_types.h"
#include "runtime/iv_modules.h"
#include "module/dependency.h"
#include "runtime/project_service_events.h"
#include "runtime/runtime_project_api_types.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iv {
struct SourceTextLineMap {
    std::string text;
    std::vector<size_t> line_offsets;

    static SourceTextLineMap from_file(std::filesystem::path const &path);
    size_t offset_for(SourcePosition position) const;
    SourcePosition position_for(size_t offset) const;
};

struct LoadedGraphIntrospectionIndex {
    std::filesystem::path module_root;
    std::string module_id;
    std::vector<IntrospectionLogicalNode> logical_nodes;
    std::unordered_map<std::string, size_t> logical_node_index_by_id;
};

class NodeExecutor;

class RuntimeProjectService {
    std::unique_ptr<RuntimeIvModules> iv_modules;

    mutable std::mutex mutex;
    mutable std::unordered_map<std::string, SourceTextLineMap> source_text_cache;
    std::optional<LoadedGraphIntrospectionIndex> graph_index;

    void emit_notification(RuntimeProjectNotification notification);
    SourceTextLineMap const &
    source_text_for(std::string const &normalized_path) const;
    void invalidate_source_text(std::string const &normalized_path);
    void invalidate_source_texts(std::span<ModuleDependency const> dependencies);
    std::pair<uint32_t, uint32_t>
    byte_range_for(std::string const &normalized_path, SourceRange const &range) const;
    LiveSourceSpan to_live_span(SourceSpan const &span) const;
    LogicalNodeInfo to_logical_node(IntrospectionLogicalNode const &node) const;

public:
    explicit RuntimeProjectService(
        std::unique_ptr<RuntimeIvModules> iv_modules = {});
    ~RuntimeProjectService();
    RuntimeProjectService(RuntimeProjectService &&) = delete;
    RuntimeProjectService &operator=(RuntimeProjectService &&) = delete;

    RuntimeProjectService(RuntimeProjectService const &) = delete;
    RuntimeProjectService &operator=(RuntimeProjectService const &) = delete;

    RuntimeProjectInitializeResult initialize();
    void handle_iv_modules_definitions_changed(
        RuntimeIvModuleDefinitionsChanged const &diff);
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
