#include <intravenous/module/source_annotations.h>

#include <intravenous/graph/builder.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv::details {
namespace {
SourceInfo public_output_source_info(PublicOutputSourceSpan span)
{
    auto declaration_identity = std::string("public-output:");
    declaration_identity += span.file_path;
    declaration_identity += ':';
    declaration_identity += std::to_string(span.begin);
    declaration_identity += ':';
    declaration_identity += std::to_string(span.end);
    return {
        .declaration_identity = std::move(declaration_identity),
        .span = {
            .file_path = std::string(span.file_path),
            .begin = span.begin,
            .end = span.end,
        },
    };
}
} // namespace

extern "C" void iv_builder_annotate_public_output_source_spans(
    GraphBuilder* builder,
    bool event,
    PublicOutputSourceSpan const* spans,
    std::size_t span_count)
{
    if (!builder || (span_count != 0 && !spans))
        throw std::invalid_argument("invalid public-output source spans");

    std::vector<SourceInfo> infos;
    infos.reserve(span_count);
    for (std::size_t index = 0; index < span_count; ++index)
        infos.push_back(public_output_source_info(spans[index]));
    if (event)
        builder->annotate_public_event_output_source_info(infos);
    else
        builder->annotate_public_sample_output_source_info(infos);
}

extern "C" void iv_builder_annotate_public_output_source_span(
    GraphBuilder* builder,
    bool event,
    std::size_t ordinal,
    char const* file_path,
    std::uint32_t begin,
    std::uint32_t end)
{
    if (!file_path)
        throw std::invalid_argument("public-output source path is null");
    auto const info = public_output_source_info({file_path, begin, end});
    if (!builder)
        throw std::invalid_argument("public-output source has no graph builder");
    if (event)
        builder->annotate_public_event_output_source_info(ordinal, info);
    else
        builder->annotate_public_sample_output_source_info(ordinal, info);
}
} // namespace iv::details
