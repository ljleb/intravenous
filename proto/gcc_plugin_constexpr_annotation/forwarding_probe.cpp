#include "authored.hpp"

#include <string_view>

consteval ProbeNodeRef compile_direct_forwarding_annotation()
{
    ProbeGraphBuilder builder;
    return iv_plugin_probe_annotate(
        builder.iv_plugin_probe_node(42),
        "authored.hpp",
        38,
        12,
        38,
        43);
}

constexpr auto directly_compiled = compile_direct_forwarding_annotation();
static_assert(directly_compiled.value == 42);
static_assert(
    std::string_view(directly_compiled.source_file) == "authored.hpp");
