#include "real_authored.hpp"

#include <ranges>
#include <string_view>

consteval bool production_plugin_annotates_real_graph()
{
    iv::GraphBuilder builder;
    real_authored_entry(builder);
    auto const metadata = builder.build_metadata();
    if (metadata.virtual_nodes.size() != 1
        || metadata.public_sample_outputs.size() != 1)
        return false;

    auto const& node = metadata.virtual_nodes.front();
    if (!std::string_view(node.source_identity).ends_with("@constant")
        || node.source_spans.size() < 2)
        return false;
    if (!std::ranges::any_of(node.source_spans, [](auto const& span) {
            return std::string_view(span.file_path).ends_with(
                       "real_authored.hpp")
                && span.end - span.begin == 8;
        }))
        return false;

    auto const& output = metadata.public_sample_outputs.front();
    return output.source_infos.size() == 1
        && std::string_view(output.source_infos.front().span.file_path)
            .ends_with("real_authored.hpp")
        && output.source_infos.front().span.end
            - output.source_infos.front().span.begin == 8;
}

static_assert(production_plugin_annotates_real_graph());
