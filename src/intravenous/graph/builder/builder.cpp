#include <intravenous/graph/builder.h>

#include <intravenous/channel_layout.h>
#include <intravenous/graph/builder/state.h>
#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/module/builder_session.h>

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace iv {
namespace {
GraphBuilderState& state(GraphBuilder& builder)
{
    return details::builder_graph_state(builder);
}

GraphBuilderState const& state(GraphBuilder const& builder)
{
    return details::builder_graph_state(const_cast<GraphBuilder&>(builder));
}

ReflectedNodeDescription materialize_node_build_request(
    details::BuilderSession* session,
    details::NodeBuildRequest const& request)
{
    if (!request.compiler_record) {
        throw std::invalid_argument("node build request has no compiler record");
    }
    auto storage = details::take_builder_node_config(
        session, request.config, request.config_size, request.config_alignment);
    auto relocations = details::capture_node_config(
        session,
        request.compiler_record->code_key,
        storage.get(),
        request.config_size);
    return details::materialize_node_description(
        request, std::move(storage), std::move(relocations));
}
} // namespace

namespace details {
NodeBundleHandle iv_builder_append_node(
    GraphBuilder& builder, NodeBuildRequest const& request)
{
    return state(builder).append_node_description(
        materialize_node_build_request(builder._session, request));
}

NodeBundleHandle iv_builder_append_tiled_node(
    GraphBuilder& builder, NodeBuildRequest const& request,
    ChannelLayout layout)
{
    auto description = materialize_node_build_request(builder._session, request);
    return state(builder).append_tiled_node_description(description, layout);
}

bool iv_builder_connect_unary_sample(
    NodeRef const& target, SamplePortRef source, std::string_view operation)
{
    auto const input_count = target.sample_input_count();
    auto const output_count = target.sample_output_count();
    if (input_count != 1) {
        error(
            std::string(operation) + " requires target to have exactly 1 input; got "
            + std::to_string(input_count) + " inputs on " + target.to_string());
    }

    target.connect_input(0, std::move(source));
    if (output_count > 1) {
        error(
            std::string(operation)
            + " requires target to have at most 1 output when used as an expression; got "
            + std::to_string(output_count) + " outputs on " + target.to_string());
    }
    return output_count == 1;
}

EventPortRef iv_builder_connect_unary_event(
    NodeRef const& target, EventPortRef source, std::string_view operation)
{
    auto const input_count = target.event_input_count();
    auto const output_count = target.event_output_count();
    if (input_count != 1) {
        error(
            std::string(operation) + " requires target to have exactly 1 event input; got "
            + std::to_string(input_count) + " event inputs on " + target.to_string());
    }

    target.connect_event_input(0, source);
    if (output_count == 0) {
        return source;
    }
    if (output_count != 1) {
        error(
            std::string(operation)
            + " requires target to have at most 1 event output when used as an expression; got "
            + std::to_string(output_count) + " event outputs on " + target.to_string());
    }
    return target.event_port(0);
}
} // namespace details

GraphBuilder::GraphBuilder()
    : GraphBuilder(details::iv_builder_session_create(), true)
{}

GraphBuilder::GraphBuilder(details::BuilderSession* session) noexcept
    : GraphBuilder(session, false)
{}

GraphBuilder::GraphBuilder(
    details::BuilderSession* session, bool owns_session) noexcept
    : _session(session)
    , _owns_session(owns_session)
{}

NodeRef GraphBuilder::registered_node(std::string_view id)
{
    return state(*this).append_registered_node(id);
}

std::optional<size_t> GraphBuilder::ensure_registered_sample_input(
    NodeBundleHandle handle, std::string_view name, ChannelLayout layout)
{
    return state(*this).ensure_registered_sample_input(handle, name, layout);
}

std::optional<size_t> GraphBuilder::ensure_registered_event_input(
    NodeBundleHandle handle, std::string_view name, EventTypeId type)
{
    return state(*this).ensure_registered_event_input(handle, name, type);
}

GraphBuilder::~GraphBuilder()
{
    if (_owns_session && _session)
        details::iv_builder_session_destroy(_session);
}

GraphBuilder::GraphBuilder(GraphBuilder&& other) noexcept
    : _session(std::exchange(other._session, nullptr))
    , _owns_session(std::exchange(other._owns_session, false))
{}

GraphBuilder& GraphBuilder::operator=(GraphBuilder&& other) noexcept
{
    if (this == &other) return *this;
    if (_owns_session && _session)
        details::iv_builder_session_destroy(_session);
    _session = std::exchange(other._session, nullptr);
    _owns_session = std::exchange(other._owns_session, false);
    return *this;
}

PublicSampleInputRef GraphBuilder::input() { return input(Sample{0.0f}); }
PublicSampleInputRef GraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max)
{
    return state(*this).input(value, min, max);
}
PublicEventInputRef GraphBuilder::event_input(EventTypeId type)
{
    return state(*this).event_input(type);
}
PublicSampleInputRef GraphBuilder::input_named(
    std::string_view name, Sample value,
    std::optional<Sample> min, std::optional<Sample> max)
{
    return state(*this).input_named(name, value, min, max);
}
PublicSampleInputRef GraphBuilder::input_named(
    std::string_view name, ChannelLayout layout, Sample value,
    std::optional<Sample> min, std::optional<Sample> max)
{
    return state(*this).input_named(name, layout, value, min, max);
}
PublicEventInputRef GraphBuilder::event_input_named(
    std::string_view name, EventTypeId type)
{
    return state(*this).event_input_named(name, type);
}

void GraphBuilder::outputs(std::initializer_list<NamedRef> refs)
{
    outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
}
void GraphBuilder::outputs(std::span<NamedRef const> refs)
{
    state(*this).outputs(refs);
}
void GraphBuilder::outputs(std::span<SampleOutputRequest const> refs)
{
    state(*this).outputs(refs);
}
void GraphBuilder::event_outputs(std::span<EventOutputRequest const> refs)
{
    state(*this).event_outputs(refs);
}

NodeRef GraphBuilder::author_runtime_binary_op(
    SamplePortRef lhs,
    SamplePortRef rhs,
    std::string_view op_name,
    details::NodeBuildRequest const& request)
{
    if (lhs.graph_builder != this || rhs.graph_builder != this) {
        details::error(std::string(op_name)
            + ": operands belong to different builders");
    }
    auto validate = [](SamplePortRef const& ref) {
        if (!is_valid_channel_type(ref.channel_type)
            || ref.channels().size() != channel_count(ref.channel_type)) {
            details::error("sample operand has an invalid channel layout");
        }
    };
    validate(lhs);
    validate(rhs);

    ChannelTypeId result_type = ChannelTypeId::mono;
    if (lhs.channel_type == rhs.channel_type) {
        result_type = lhs.channel_type;
    } else if (lhs.channel_type == ChannelTypeId::mono) {
        result_type = rhs.channel_type;
    } else if (rhs.channel_type == ChannelTypeId::mono) {
        result_type = lhs.channel_type;
    } else {
        details::error(std::string(op_name)
            + ": sample operands must have matching channel types, except that mono broadcasts");
    }

    auto handle = details::iv_builder_append_tiled_node(
        *this, request, {
            .channel_type = result_type,
            .sample_layout = SampleStreamLayout::planar,
        });
    NodeRef result(*this, handle);
    std::array<details::NodeCallSampleInput, 2> inputs{{
        {.source = std::move(lhs),
         .name = {},
         .input_ordinal = 0,
         .target = details::NodeCallInputTarget::explicit_ordinal},
        {.source = std::move(rhs),
         .name = {},
         .input_ordinal = 1,
         .target = details::NodeCallInputTarget::explicit_ordinal},
    }};
    result.apply_node_call(
        {.data = inputs.data(), .size = inputs.size()}, {});
    return result;
}

SamplePortRef GraphBuilder::lift_to_sample_port(SamplePortRef const& value)
{
    return state(*this).lift_to_sample_port(value);
}
SamplePortRef GraphBuilder::lift_to_sample_port(SamplePortRef&& value)
{
    return state(*this).lift_to_sample_port(std::move(value));
}
SamplePortRef GraphBuilder::lift_to_sample_port(NamedRef const& value)
{
    return state(*this).lift_to_sample_port(value);
}

void GraphBuilder::connect_sample_input(NodeBundlePortId target, SamplePortRef source)
{
    state(*this).connect_sample_input(target, std::move(source));
}
void GraphBuilder::connect_sample_input(
    NodeBundlePortId target, std::span<SamplePortRef const> sources)
{
    state(*this).connect_sample_input(target, sources);
}
void GraphBuilder::connect_event_input(NodeBundlePortId target, EventPortRef source)
{
    state(*this).connect_event_input(target, std::move(source));
}
bool GraphBuilder::sample_input_is_connected(NodeBundlePortId target) const
{
    return state(*this).sample_input_is_connected(target);
}
bool GraphBuilder::event_input_is_connected(NodeBundlePortId target) const
{
    return state(*this).event_input_is_connected(target);
}
void GraphBuilder::connect_sample_output(NodeBundlePortId source, NodeRef const& target)
{
    state(*this).connect_sample_output(source, target);
}
EventPortRef GraphBuilder::event_output(NodeBundlePortId source) const
{
    return state(*this).event_output(source);
}
size_t GraphBuilder::sample_port_index(
    NodeBundleHandle handle, bool inputs, std::string_view name) const
{
    return state(*this).sample_port_index(handle, inputs, name);
}
size_t GraphBuilder::event_port_index(
    NodeBundleHandle handle, bool inputs, std::string_view name) const
{
    return state(*this).event_port_index(handle, inputs, name);
}
size_t GraphBuilder::sample_input_count(NodeBundleHandle handle) const
{
    return state(*this).sample_input_count(handle);
}
size_t GraphBuilder::sample_output_count(NodeBundleHandle handle) const
{
    return state(*this).sample_output_count(handle);
}
size_t GraphBuilder::event_input_count(NodeBundleHandle handle) const
{
    return state(*this).event_input_count(handle);
}
size_t GraphBuilder::event_output_count(NodeBundleHandle handle) const
{
    return state(*this).event_output_count(handle);
}

InputConfig GraphBuilder::sample_input_config(
    NodeBundleHandle handle, size_t port) const
{
    return state(*this).sample_input_config(handle, port);
}

EventInputConfig GraphBuilder::event_input_config(
    NodeBundleHandle handle, size_t port) const
{
    return state(*this).event_input_config(handle, port);
}
NodeBundleHandle GraphBuilder::tiled_member(
    NodeBundleHandle handle, size_t channel) const
{
    return state(*this).tiled_member(handle, channel);
}
NodePorts const& GraphBuilder::typed_ports(NodeBundleHandle handle) const
{
    return state(*this).typed_ports(handle);
}
SamplePortRef GraphBuilder::sample_port_from_output(NodeBundlePortId port)
{
    return state(*this).sample_port_from_output(port);
}
EventPortRef GraphBuilder::event_port_from_output(NodeBundlePortId port) const
{
    return state(*this).event_port_from_output(port);
}
SamplePortRef GraphBuilder::make_sample_port(
    ChannelTypeId type, std::span<SampleOutputChannelId const> channels)
{
    return state(*this).make_sample_port(type, channels);
}
std::span<SampleOutputChannelId const>
GraphBuilder::sample_port_channels(SamplePortRef const& ref) const
{
    return state(*this).sample_port_channels(ref);
}
EventPortRef GraphBuilder::make_event_port(
    EventTypeId type, std::span<EventOutputPortId const> sources)
{
    return state(*this).make_event_port(type, sources);
}
std::span<EventOutputPortId const>
GraphBuilder::event_port_sources(EventPortRef const& ref) const
{
    return state(*this).event_port_sources(ref);
}
SamplePortRef GraphBuilder::detach_sample_port(
    SamplePortRef const& source, size_t latency)
{
    return state(*this).detach_sample_port(source, latency);
}
void GraphBuilder::apply_ttl(NodeBundleHandle handle, size_t samples)
{
    state(*this).apply_ttl(handle, samples);
}
void GraphBuilder::annotate_node(
    NodeBundleHandle handle, std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end)
{
    state(*this).annotate_node(handle, id, file, begin, end);
}
void GraphBuilder::annotate_public_sample_input_source_info(
    PublicSampleInputRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end)
{
    state(*this).annotate_public_sample_input_source_info(ref, id, file, begin, end);
}
void GraphBuilder::annotate_public_event_input_source_info(
    PublicEventInputRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end)
{
    state(*this).annotate_public_event_input_source_info(ref, id, file, begin, end);
}
void GraphBuilder::annotate_sample_port_source_info(
    SamplePortRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end)
{
    state(*this).annotate_sample_port_source_info(ref, id, file, begin, end);
}
void GraphBuilder::annotate_event_port_source_info(
    EventPortRef const& ref, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end)
{
    state(*this).annotate_event_port_source_info(ref, id, file, begin, end);
}
void GraphBuilder::annotate_public_sample_output_source_info(
    std::span<SourceInfo const> infos)
{
    state(*this).annotate_public_sample_output_source_info(infos);
}
void GraphBuilder::annotate_public_event_output_source_info(
    std::span<SourceInfo const> infos)
{
    state(*this).annotate_public_event_output_source_info(infos);
}
void GraphBuilder::annotate_public_sample_output_source_info(
    size_t ordinal, SourceInfo info)
{
    state(*this).annotate_public_sample_output_source_info(ordinal, std::move(info));
}
void GraphBuilder::annotate_public_event_output_source_info(
    size_t ordinal, SourceInfo info)
{
    state(*this).annotate_public_event_output_source_info(ordinal, std::move(info));
}

NodeRef GraphBuilder::embed_child(GraphBuilder& child, std::string_view kind)
{
    auto& graph = state(*this);
    auto& child_graph = state(child);
    return graph.embed_subgraph(child_graph, kind);
}

details::SubgraphBuildScope* GraphBuilder::begin_subgraph()
{
    return state(*this).begin_subgraph();
}

NodeRef GraphBuilder::finish_subgraph(
    details::SubgraphBuildScope* scope, std::string_view kind)
{
    auto& graph = state(*this);
    try {
        auto result = graph.finish_subgraph(*scope, kind);
        graph.abandon_subgraph(scope);
        return result;
    } catch (...) {
        graph.abandon_subgraph(scope);
        throw;
    }
}

void GraphBuilder::abandon_subgraph(
    details::SubgraphBuildScope* scope) noexcept
{
    if (!scope) return;
    state(*this).abandon_subgraph(scope);
}

PublicSampleInputRef GraphBuilder::subgraph_input(
    details::SubgraphBuildScope* scope, std::string_view name,
    ChannelLayout layout, Sample value, std::optional<Sample> min,
    std::optional<Sample> max)
{
    if (!scope) details::error("subgraph build scope is no longer active");
    return state(*this).subgraph_input(*scope, name, layout, value, min, max);
}

PublicEventInputRef GraphBuilder::subgraph_event_input(
    details::SubgraphBuildScope* scope, std::string_view name,
    EventTypeId type)
{
    if (!scope) details::error("subgraph build scope is no longer active");
    return state(*this).subgraph_event_input(*scope, name, type);
}

void GraphBuilder::subgraph_outputs(
    details::SubgraphBuildScope* scope,
    std::span<SampleOutputRequest const> refs)
{
    if (!scope) details::error("subgraph build scope is no longer active");
    state(*this).subgraph_outputs(*scope, refs);
}

void GraphBuilder::subgraph_outputs(
    details::SubgraphBuildScope* scope, std::span<NamedRef const> refs)
{
    if (!scope) details::error("subgraph build scope is no longer active");
    state(*this).subgraph_outputs(*scope, refs);
}

void GraphBuilder::subgraph_event_outputs(
    details::SubgraphBuildScope* scope,
    std::span<EventOutputRequest const> refs)
{
    if (!scope) details::error("subgraph build scope is no longer active");
    state(*this).subgraph_event_outputs(*scope, refs);
}

AuthoredGraph GraphBuilder::finish() const &
{
    return state(*this).finish();
}
AuthoredGraph GraphBuilder::finish() &&
{
    if (!_session) throw std::logic_error("cannot finish an empty GraphBuilder");
    return details::take_built_graph(_session);
}

} // namespace iv
