#pragma once

#include <intravenous/graph/node.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace iv {
    class GraphBuilder;

    struct SamplePortRef {
        GraphBuilder* graph_builder{};
        size_t node_index{};
        size_t output_port{};

        SamplePortRef() = default;
        SamplePortRef(SamplePortRef const&) = default;
        SamplePortRef(SamplePortRef&&) noexcept = default;
        explicit SamplePortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        SamplePortRef& operator=(SamplePortRef const&) = default;
        SamplePortRef& operator=(SamplePortRef&& rhs) = default;
        SamplePortRef _clone_handle() const;

        SamplePortRef detach(size_t loop_extra_latency = 1) const;
        std::string to_string() const;
    };

    struct PublicSampleInputRef {
        SamplePortRef port {};

        PublicSampleInputRef() = default;
        explicit PublicSampleInputRef(SamplePortRef port_) : port(std::move(port_)) {}

        operator SamplePortRef() const { return port; }
        operator PortId() const { return static_cast<PortId>(port); }

        void _annotate_source_info(
            std::string_view declaration_identity,
            std::string_view file_path,
            uint32_t begin,
            uint32_t end) const;
    };

    struct EventPortRef {
        GraphBuilder* graph_builder {};
        size_t node_index {};
        size_t output_port {};

        EventPortRef() = default;
        explicit EventPortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        std::string to_string() const;
    };

    struct PublicEventInputRef {
        EventPortRef port {};
        PublicEventInputRef() = default;
        explicit PublicEventInputRef(EventPortRef port_) : port(std::move(port_)) {}
        operator EventPortRef() const { return port; }
        operator PortId() const { return static_cast<PortId>(port); }
        void _annotate_source_info(
            std::string_view declaration_identity, std::string_view file_path,
            uint32_t begin, uint32_t end) const;
    };
}
