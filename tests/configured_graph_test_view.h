#pragma once

#include <intravenous/graph/configured_graph.hpp>

#include <memory>
#include <utility>

namespace iv {

// The production configuration graph is now a runtime value: reflected node
// descriptions own dynamically allocated configuration and cannot be frozen
// into a constexpr object. Tests that need to lower the same configured graph
// more than once keep it here and obtain an independent copy for each pass.
class ConfiguredGraphTestView {
public:
    ConfiguredGraphTestView() = default;

    explicit ConfiguredGraphTestView(ConfiguredGraph graph)
        : _graph(std::make_shared<ConfiguredGraph>(std::move(graph)))
    {}

    ConfiguredGraph copy() const
    {
        return *_graph;
    }

private:
    std::shared_ptr<ConfiguredGraph const> _graph {};
};

inline ConfiguredGraphTestView freeze_configured_graph_for_test(ConfiguredGraph graph)
{
    return ConfiguredGraphTestView(std::move(graph));
}

inline ConfiguredGraph thaw_configured_graph_for_test(ConfiguredGraphTestView const& view)
{
    return view.copy();
}

} // namespace iv
