#pragma once

#include <intravenous/graph/authored_graph.hpp>

#include <memory>
#include <utility>

namespace iv {

// The production authoring graph is now a runtime value: reflected node
// descriptions own dynamically allocated configuration and cannot be frozen
// into a constexpr object. Tests that need to lower the same authored graph
// more than once keep it here and obtain an independent copy for each pass.
class AuthoredGraphTestView {
public:
    AuthoredGraphTestView() = default;

    explicit AuthoredGraphTestView(AuthoredGraph graph)
        : _graph(std::make_shared<AuthoredGraph>(std::move(graph)))
    {}

    AuthoredGraph copy() const
    {
        return *_graph;
    }

private:
    std::shared_ptr<AuthoredGraph const> _graph {};
};

inline AuthoredGraphTestView freeze_authored_graph_for_test(AuthoredGraph graph)
{
    return AuthoredGraphTestView(std::move(graph));
}

inline AuthoredGraph thaw_authored_graph_for_test(AuthoredGraphTestView const& view)
{
    return view.copy();
}

} // namespace iv
