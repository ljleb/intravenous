#pragma once

struct ProbeNodeRef {
    unsigned value = 0;
    char const* source_file = "";
    unsigned source_begin_line = 0;
    unsigned source_begin_column = 0;
    unsigned source_end_line = 0;
    unsigned source_end_column = 0;
};

struct ProbeGraphBuilder {
    constexpr ProbeNodeRef iv_plugin_probe_node(unsigned value)
    {
        return {.value = value};
    }
};

template<class Ref>
constexpr Ref iv_plugin_probe_annotate(
    Ref ref,
    char const* source_file,
    unsigned source_begin_line,
    unsigned source_begin_column,
    unsigned source_end_line,
    unsigned source_end_column)
{
    ref.source_file = source_file;
    ref.source_begin_line = source_begin_line;
    ref.source_begin_column = source_begin_column;
    ref.source_end_line = source_end_line;
    ref.source_end_column = source_end_column;
    return ref;
}

constexpr ProbeNodeRef authored_header_entry(ProbeGraphBuilder& builder)
{
    return builder.iv_plugin_probe_node(42); // IV_EXPECT_WRAPPED_LINE
}
