#pragma once

#include <intravenous/graph/builder.h>
#include <intravenous/module/abi.h>

#include <concepts>
#include <type_traits>

namespace iv::details {
template<auto Main>
consteval Graph generated_module_graph_value()
{
    static_assert(std::invocable<decltype(Main), GraphBuilder&>,
        "iv_module.json main must name void(GraphBuilder&)");
    static_assert(std::same_as<std::invoke_result_t<decltype(Main), GraphBuilder&>, void>,
        "iv_module.json main must name void(GraphBuilder&)");

    GraphBuilder builder;
    Main(builder);
    return builder.build_execution_root_node().graph;
}

template<auto Main>
WeakTypeErasedNode generated_module_graph() noexcept
{
    static constexpr Graph graph = generated_module_graph_value<Main>();
    return WeakTypeErasedNode(graph);
}

template<auto Main>
consteval StaticGraphIntrospectionMetadata generated_module_metadata_value()
{
    static_assert(std::invocable<decltype(Main), GraphBuilder&>,
        "iv_module.json main must name void(GraphBuilder&)");
    static_assert(std::same_as<std::invoke_result_t<decltype(Main), GraphBuilder&>, void>,
        "iv_module.json main must name void(GraphBuilder&)");

    GraphBuilder builder;
    Main(builder);
    return define_static_metadata(builder.build_metadata());
}

template<auto Main>
StaticGraphIntrospectionMetadata generated_module_metadata() noexcept
{
    static constexpr auto metadata = generated_module_metadata_value<Main>();
    return metadata;
}
} // namespace iv::details
