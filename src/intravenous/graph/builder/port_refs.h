#pragma once

#include "../node.h"

#include <cstddef>
#include <string>

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

    struct EventPortRef {
        GraphBuilder* graph_builder {};
        size_t node_index {};
        size_t output_port {};

        EventPortRef() = default;
        explicit EventPortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        std::string to_string() const;
    };
}
