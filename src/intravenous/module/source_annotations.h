#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace iv {
class GraphBuilder;

struct PublicOutputSourceSpan {
    std::string_view file_path;
    std::uint32_t begin;
    std::uint32_t end;
};

namespace details {
// Source instrumentation in a module only passes these POD-ish views. String
// construction, deduplication, and graph metadata mutation stay in the
// precompiled builder library.
extern "C" void iv_builder_annotate_public_output_source_spans(
    GraphBuilder*, bool event, PublicOutputSourceSpan const*, std::size_t);
extern "C" void iv_builder_annotate_public_output_source_span(
    GraphBuilder*, bool event, std::size_t ordinal, char const* file_path,
    std::uint32_t begin, std::uint32_t end);
} // namespace details
} // namespace iv
