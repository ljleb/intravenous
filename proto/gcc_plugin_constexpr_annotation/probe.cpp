struct ProbeBuilder {
    unsigned source_line = 0;
};

constexpr void iv_plugin_probe_mark(
    ProbeBuilder& builder,
    unsigned source_line)
{
    builder.source_line = source_line;
}

constexpr void configured_module_entry(ProbeBuilder& builder)
{
    iv_plugin_probe_mark(builder, 0); // IV_EXPECT_ANNOTATED_LINE
}

consteval unsigned compile_configured_module()
{
    ProbeBuilder builder;
    configured_module_entry(builder);
    return builder.source_line;
}

static_assert(compile_configured_module() == 14);
