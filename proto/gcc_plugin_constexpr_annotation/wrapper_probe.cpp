#include "configured.hpp"

#include <string_view>

consteval ProbeNodeRef compile_configured_header()
{
    ProbeGraphBuilder builder;
    return configured_header_entry(builder);
}

constexpr auto compiled = compile_configured_header();
static_assert(compiled.value == 42);
static_assert(std::string_view(compiled.source_file).ends_with("configured.hpp"));
static_assert(compiled.source_begin_line == 38);
static_assert(compiled.source_end_line == 38);
static_assert(compiled.source_begin_column != 0);
static_assert(compiled.source_end_column > compiled.source_begin_column);
