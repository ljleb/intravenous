#include "real_authored.hpp"

#include <string_view>

consteval auto real_graph_metadata()
{
    iv::GraphBuilder builder;
    real_authored_entry(builder);
    return builder.build_metadata();
}

consteval std::size_t real_graph_virtual_node_count()
{
    auto const metadata = real_graph_metadata();
    return metadata.virtual_nodes.size();
}

consteval std::size_t real_graph_source_span_count()
{
    auto const metadata = real_graph_metadata();
    if (metadata.virtual_nodes.empty())
        return 0;
    return metadata.virtual_nodes.front().source_spans.size();
}

consteval bool real_graph_contains_plugin_source_info()
{
    auto const metadata = real_graph_metadata();
    if (real_graph_virtual_node_count() != 1
        || real_graph_source_span_count() != 1)
        return false;
    auto const& span = metadata.virtual_nodes.front().source_spans.front();
    return metadata.virtual_nodes.front().source_identity
            == "gcc-plugin-real-node"
        && std::string_view(span.file_path).ends_with("real_authored.hpp")
        && span.begin == 42
        && span.end == 42;
}

static_assert(real_graph_virtual_node_count() == 1);
static_assert(real_graph_source_span_count() == 1);
static_assert(real_graph_contains_plugin_source_info());
