#pragma once

#include <intravenous/dsl.h>

template<class Ref>
constexpr Ref iv_plugin_graph_annotate(
    Ref ref,
    char const* source_file,
    unsigned source_begin_line,
    unsigned,
    unsigned source_end_line,
    unsigned)
{
    iv::_annotate_node_source_info(
        ref,
        "gcc-plugin-real-node",
        source_file,
        source_begin_line,
        source_end_line);
    return ref;
}

template<class Ref>
constexpr void iv_plugin_graph_annotate_initialized(
    Ref* ref,
    char const* source_file,
    unsigned source_begin_line,
    unsigned,
    unsigned source_end_line,
    unsigned)
{
    iv::_annotate_node_source_info(
        *ref,
        "gcc-plugin-real-node",
        source_file,
        source_begin_line,
        source_end_line);
}

constexpr void real_authored_entry(iv::GraphBuilder& builder)
{
    auto constant = builder.node<iv::Constant>(iv::Sample{0.25f});
    builder.outputs(constant);
}
