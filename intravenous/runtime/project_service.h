#pragma once

#include "graph/types.h"
#include "devices/audio_device.h"

#include <chrono>
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

    struct LiveSourceSpan {
        std::string file_path {};
        SourceRange range {};

        bool operator==(LiveSourceSpan const&) const = default;
    };

    struct LivePortInfo {
        std::string name {};
        std::string type {};
        bool connected = false;

        bool operator==(LivePortInfo const&) const = default;
    };

    struct LiveNodeInfo {
        std::string id {};
        std::string kind {};
        std::vector<LiveSourceSpan> source_spans {};
        std::vector<LivePortInfo> sample_inputs {};
        std::vector<LivePortInfo> sample_outputs {};
        std::vector<LivePortInfo> event_inputs {};
        std::vector<LivePortInfo> event_outputs {};
    };

    struct RuntimeProjectInitializeResult {
        std::filesystem::path module_root {};
        std::string module_id {};
        uint64_t execution_epoch = 0;
    };

    struct RuntimeProjectQueryResult {
        uint64_t execution_epoch = 0;
        std::vector<LiveNodeInfo> nodes {};
    };

    using AudioDeviceFactory = std::function<std::optional<LogicalAudioDevice>()>;

    class RuntimeProjectService {
        class Impl;
        std::unique_ptr<Impl> _impl;

    public:
        explicit RuntimeProjectService(
            std::filesystem::path workspace_root,
            std::filesystem::path discovery_start,
            std::vector<std::filesystem::path> extra_search_roots = {},
            AudioDeviceFactory audio_device_factory = {}
        );
        ~RuntimeProjectService();
        RuntimeProjectService(RuntimeProjectService&&) noexcept;
        RuntimeProjectService& operator=(RuntimeProjectService&&) noexcept;

        RuntimeProjectService(RuntimeProjectService const&) = delete;
        RuntimeProjectService& operator=(RuntimeProjectService const&) = delete;

        RuntimeProjectInitializeResult initialize();
        RuntimeProjectQueryResult query_by_spans(
            std::filesystem::path const& file_path,
            std::vector<SourceRange> const& ranges
        ) const;
        LiveNodeInfo get_node(uint64_t execution_epoch, std::string const& node_id) const;
        void request_shutdown();
    };
}
