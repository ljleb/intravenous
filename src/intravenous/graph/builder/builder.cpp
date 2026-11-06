#include <intravenous/graph/builder.h>

#include <intravenous/channel_layout.h>
#include <intravenous/graph/builder/state.h>
#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/module/builder_session.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace iv {
namespace details {
void iv_builder_prepare_node_source_identity(
    GraphBuilder* builder,
    std::string_view source_identity,
    std::string_view authored_name)
try {
    if (!builder) {
        throw std::invalid_argument(
            "node source identity requires a GraphBuilder");
    }
    prepare_builder_node_source_identity(
        builder->_session, source_identity, authored_name);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure((builder ? builder->_builder_session() : nullptr), std::current_exception(), {});
    throw;
}
} // namespace details

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
    auto description = details::materialize_node_description(
        request, std::move(storage), std::move(relocations));
    description.state_structures_storage =
        details::copy_builder_node_state_structures(
            session, request.compiler_record->code_key);
    description.operations.runtime.state_structures =
        description.state_structures_storage
            ? description.state_structures_storage.get()
            : nullptr;
    return description;
}
} // namespace

namespace details {
NodeBundleHandle iv_builder_append_node(
    GraphBuilder& builder, NodeBuildRequest const& request)
try {
    auto paths = claim_builder_concrete_node_paths(builder._session);
    if (paths.size() != 1) {
        throw std::logic_error("a concrete node did not produce one semantic path");
    }
    if (auto replacement = resolve_builder_concrete_node_replacement(
            builder._session, builder, paths.front())) {
        iv_builder_discard_node_config(
            builder._session, const_cast<void*>(request.config));
        record_builder_node_message_path(
            builder._session, *replacement, paths.front());
        return *replacement;
    }
    auto description = materialize_node_build_request(builder._session, request);
    register_builder_concrete_node_message_handlers(
        builder._session,
        request.compiler_record->code_key,
        description.node_storage,
        paths.front());
    auto const handle = state(builder).append_node_description(
        std::move(description));
    record_builder_node_message_path(
        builder._session, handle, paths.front());
    return handle;
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(builder._builder_session(), std::current_exception(), {});
    throw;
}

NodeBundleHandle iv_builder_append_tiled_node(
    GraphBuilder& builder, NodeBuildRequest const& request,
    ChannelLayout layout)
try {
    auto paths = claim_builder_concrete_node_paths(builder._session, layout);
    if (paths.size() != channel_count(layout.channel_type)) {
        throw std::logic_error(
            "a tiled concrete node produced the wrong semantic path count");
    }

    std::vector<std::optional<NodeBundleHandle>> replacements;
    replacements.reserve(paths.size());
    bool has_replacement = false;
    for (auto const& path : paths) {
        replacements.push_back(resolve_builder_concrete_node_replacement(
            builder._session, builder, path));
        has_replacement = has_replacement || replacements.back().has_value();
    }
    auto const all_replaced = std::ranges::all_of(
        replacements,
        [](auto const& replacement) { return replacement.has_value(); });
    if (all_replaced) {
        iv_builder_discard_node_config(
            builder._session, const_cast<void*>(request.config));
        std::vector<NodeBundleHandle> members;
        members.reserve(replacements.size());
        for (auto const& replacement : replacements) {
            members.push_back(*replacement);
        }
        auto const handle = state(builder).append_tiled_node_bundles(
            members, layout);
        auto parent_path = paths.front();
        parent_path.pop_back();
        record_builder_node_message_path(
            builder._session, handle, parent_path);
        return handle;
    }

    auto description = materialize_node_build_request(builder._session, request);
    if (!has_replacement) {
        for (auto const& path : paths) {
            register_builder_concrete_node_message_handlers(
                builder._session,
                request.compiler_record->code_key,
                description.node_storage,
                path);
        }
        auto const handle = state(builder).append_tiled_node_description(
            description, layout);
        auto parent_path = paths.front();
        parent_path.pop_back();
        record_builder_node_message_path(
            builder._session, handle, parent_path);
        return handle;
    }

    std::vector<NodeBundleHandle> members;
    members.reserve(paths.size());
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (replacements[index]) {
            members.push_back(*replacements[index]);
            continue;
        }
        register_builder_concrete_node_message_handlers(
            builder._session,
            request.compiler_record->code_key,
            description.node_storage,
            paths[index]);
        members.push_back(state(builder).append_node_description(description));
    }
    auto const handle = state(builder).append_tiled_node_bundles(
        members, layout);
    auto parent_path = paths.front();
    parent_path.pop_back();
    record_builder_node_message_path(
        builder._session, handle, parent_path);
    return handle;
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(builder._builder_session(), std::current_exception(), {});
    throw;
}

NodeBundleHandle iv_builder_append_tiled_node_bundles(
    GraphBuilder& builder,
    std::span<NodeBundleHandle const> members,
    ChannelLayout layout)
try {
    return state(builder).append_tiled_node_bundles(members, layout);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(builder._builder_session(), std::current_exception(), {});
    throw;
}

bool iv_builder_connect_unary_sample(
    NodeRef const& target, SamplePortRef source, std::string_view operation)
try {
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
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(target._builder_session(), std::current_exception(), {});
    throw;
}

EventPortRef iv_builder_connect_unary_event(
    NodeRef const& target, EventPortRef source, std::string_view operation)
try {
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
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(target._builder_session(), std::current_exception(), {});
    throw;
}
} // namespace details

// ---- session ref operations ---------------------------------------------

namespace details {
namespace {
GraphBuilderState& session(BuilderSession* value)
{
    return builder_session_state(value);
}
} // namespace

SamplePortRef iv_builder_require_sample_port(
    BuilderSession* value, SamplePortRef const& port)
try {
    return session(value).require_sample_port(port);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_connect_sample_input(
    BuilderSession* value, NodeBundlePortId target, SamplePortRef const& source)
try {
    session(value).connect_sample_input(target, source);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_connect_sample_inputs(
    BuilderSession* value, NodeBundlePortId target,
    std::span<SamplePortRef const> sources)
try {
    session(value).connect_sample_input(target, sources);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_connect_event_input(
    BuilderSession* value, NodeBundlePortId target, EventPortRef const& source)
try {
    session(value).connect_event_input(target, session(value).require_event_port(source));
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
bool iv_builder_sample_input_is_connected(BuilderSession* value, NodeBundlePortId target)
try {
    return session(value).sample_input_is_connected(target);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
bool iv_builder_event_input_is_connected(BuilderSession* value, NodeBundlePortId target)
try {
    return session(value).event_input_is_connected(target);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
std::size_t iv_builder_sample_port_index(
    BuilderSession* value, NodeBundleHandle handle, bool inputs, std::string_view name)
try {
    return session(value).sample_port_index(handle, inputs, name);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
std::size_t iv_builder_event_port_index(
    BuilderSession* value, NodeBundleHandle handle, bool inputs, std::string_view name)
try {
    return session(value).event_port_index(handle, inputs, name);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
std::size_t iv_builder_sample_input_count(BuilderSession* value, NodeBundleHandle handle)
try {
    return session(value).sample_input_count(handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
std::size_t iv_builder_sample_output_count(BuilderSession* value, NodeBundleHandle handle)
try {
    return session(value).sample_output_count(handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
std::size_t iv_builder_event_input_count(BuilderSession* value, NodeBundleHandle handle)
try {
    return session(value).event_input_count(handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
std::size_t iv_builder_event_output_count(BuilderSession* value, NodeBundleHandle handle)
try {
    return session(value).event_output_count(handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
NodeBundlePortId iv_builder_input_port_at(
    BuilderSession* value, NodeBundleHandle handle, std::size_t position)
try {
    return session(value).input_port_at(handle, position);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
NodePorts const& iv_builder_typed_ports(BuilderSession* value, NodeBundleHandle handle)
try {
    return session(value).typed_ports(handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
NodeBundleHandle iv_builder_tiled_member(
    BuilderSession* value, NodeBundleHandle handle, std::size_t channel)
try {
    return session(value).tiled_member(handle, channel);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
SamplePortRef iv_builder_sample_port_from_output(
    BuilderSession* value, NodeBundlePortId port)
try {
    return session(value).sample_port_from_output(port);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
EventPortRef iv_builder_event_port_from_output(
    BuilderSession* value, NodeBundlePortId port)
try {
    return session(value).event_port_from_output(port);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
SamplePortRef iv_builder_make_sample_port(
    BuilderSession* value, ChannelTypeId type,
    std::span<SampleOutputChannelId const> channels)
try {
    return session(value).make_sample_port(type, channels);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
SamplePortRef iv_builder_make_tiled_sample_port(
    BuilderSession* value, ChannelTypeId type, std::span<SamplePortRef const> members)
try {
    return session(value).make_tiled_sample_port(type, members);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
SamplePortRef iv_builder_select_sample_port_channel(
    SamplePortRef const& port, std::size_t channel)
try {
    return session(port.session).select_sample_port_channel(port, channel);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}
std::span<SampleOutputChannelId const> iv_builder_sample_port_channels(
    SamplePortRef const& port)
try {
    return session(port.session).sample_port_channels(port);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}
EventPortRef iv_builder_make_event_port(
    BuilderSession* value, EventTypeId type, std::span<EventOutputPortId const> sources)
try {
    return session(value).make_event_port(type, sources);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
EventPortRef iv_builder_merge_event_ports(
    BuilderSession* value, std::span<EventPortRef const> operands)
try {
    return session(value).make_event_port(operands);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
std::span<EventOutputPortId const> iv_builder_event_port_sources(EventPortRef const& port)
try {
    return session(port.session).event_port_sources(port);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}
SamplePortRef iv_builder_detach_sample_port(
    SamplePortRef const& port, std::size_t latency, std::optional<Sample> initial_value)
try {
    return session(port.session).detach_sample_port(port, latency, initial_value);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}
EventPortRef iv_builder_detach_event_port(EventPortRef const& port, std::size_t latency)
try {
    return session(port.session).detach_event_port(port, latency);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}
void iv_builder_apply_ttl(BuilderSession* value, NodeBundleHandle handle, std::size_t samples)
try {
    session(value).apply_ttl(handle, samples);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_annotate_node(
    BuilderSession* value, NodeBundleHandle handle, std::string_view id,
    std::string_view file, std::uint32_t begin, std::uint32_t end)
try {
    session(value).annotate_node(handle, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_annotate_node_input(
    BuilderSession* value, NodeBundleHandle handle, PortKind kind,
    std::string_view port_name, std::string_view id, std::string_view file,
    std::uint32_t begin, std::uint32_t end)
try {
    session(value).annotate_node_input_source_info(
        handle, kind, port_name, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_annotate_sample_port(
    SamplePortRef const& port, std::string_view id, std::string_view file,
    std::uint32_t begin, std::uint32_t end)
try {
    session(port.session).annotate_sample_port_source_info(port, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}
void iv_builder_annotate_event_port(
    EventPortRef const& port, std::string_view id, std::string_view file,
    std::uint32_t begin, std::uint32_t end)
try {
    session(port.session).annotate_event_port_source_info(port, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}
void iv_builder_annotate_public_sample_input(
    SamplePortRef const& port, std::string_view id, std::string_view file,
    std::uint32_t begin, std::uint32_t end)
try {
    session(port.session).annotate_public_sample_input_source_info(
        port, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}
void iv_builder_annotate_public_event_input(
    EventPortRef const& port, std::string_view id, std::string_view file,
    std::uint32_t begin, std::uint32_t end)
try {
    session(port.session).annotate_public_event_input_source_info(
        port, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(port.session, std::current_exception(), {});
    throw;
}

// ---- scope operations --------------------------------------------------

PublicSampleInputRef iv_builder_scope_input(
    BuilderSession* value, BuilderScopeId scope, ContinuousInputConfig config)
try {
    return session(value).scope_input(scope, std::move(config));
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
PublicEventInputRef iv_builder_scope_event_input(
    BuilderSession* value, BuilderScopeId scope, std::string_view name, EventTypeId type)
try {
    return session(value).scope_event_input(scope, name, type);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_scope_outputs(
    BuilderSession* value, BuilderScopeId scope, std::span<SampleOutputRequest const> refs)
try {
    session(value).scope_outputs(scope, refs);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_scope_named_outputs(
    BuilderSession* value, BuilderScopeId scope, std::span<NamedRef const> refs)
try {
    session(value).scope_outputs(scope, refs);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_scope_event_outputs(
    BuilderSession* value, BuilderScopeId scope, std::span<EventOutputRequest const> refs)
try {
    session(value).scope_event_outputs(scope, refs);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
BuilderScopeId iv_builder_begin_subgraph_scope(BuilderSession* value)
try {
    return begin_builder_scope(value, BuilderScopeKind::subgraph);
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
NodeRef iv_builder_finish_subgraph_scope(
    BuilderSession* value, BuilderScopeId scope, std::string_view kind)
try {
    auto const handle = finish_builder_scope(value, scope, kind, std::nullopt);
    NodeRef result(value, handle);
    // The boundary's declaration identity annotates the finished subgraph in
    // the enclosing scope, as an authored subgraph's backing virtual node.
    for (auto const& info : session(value).subgraph_boundary_source_infos(handle)) {
        result._annotate_source_info(
            info.declaration_identity,
            info.span.file_path, info.span.begin, info.span.end);
    }
    return result;
} catch (...) {
    // An exception leaving a session operation fails the session.
    record_builder_failure(value, std::current_exception(), {});
    throw;
}
void iv_builder_record_failure(
    BuilderSession* value, std::exception_ptr failure, std::string_view context) noexcept
{
    record_builder_failure(value, std::move(failure), context);
}
void iv_builder_throw_if_failed(BuilderSession* value)
{
    throw_if_builder_failed(value);
}
} // namespace details

// ---- GraphBuilder views --------------------------------------------------

GraphBuilder::GraphBuilder()
    : GraphBuilder(details::iv_builder_session_create(),
          details::root_builder_scope_id, true, false)
{}

GraphBuilder::GraphBuilder(details::BuilderSession* session) noexcept
    : GraphBuilder(session, details::iv_builder_current_scope(session), false, false)
{}

GraphBuilder::GraphBuilder(
    details::BuilderSession* session, details::BuilderScopeId scope) noexcept
    : GraphBuilder(session, scope, false, false)
{}

GraphBuilder::GraphBuilder(
    details::BuilderSession* session, details::BuilderScopeId scope,
    bool owns_session, bool owns_scope) noexcept
    : _session(session)
    , _scope(scope)
    , _owns_session(owns_session)
    , _owns_scope(owns_scope)
{}

GraphBuilder::~GraphBuilder()
{
    if (_owns_scope && _session)
        details::abandon_builder_scope(_session, _scope);
    if (_owns_session && _session)
        details::iv_builder_session_destroy(_session);
}

GraphBuilder::GraphBuilder(GraphBuilder&& other) noexcept
    : _session(std::exchange(other._session, nullptr))
    , _scope(std::exchange(other._scope, details::root_builder_scope_id))
    , _owns_session(std::exchange(other._owns_session, false))
    , _owns_scope(std::exchange(other._owns_scope, false))
{}

GraphBuilder& GraphBuilder::operator=(GraphBuilder&& other) noexcept
{
    if (this == &other) return *this;
    if (_owns_scope && _session)
        details::abandon_builder_scope(_session, _scope);
    if (_owns_session && _session)
        details::iv_builder_session_destroy(_session);
    _session = std::exchange(other._session, nullptr);
    _scope = std::exchange(other._scope, details::root_builder_scope_id);
    _owns_session = std::exchange(other._owns_session, false);
    _owns_scope = std::exchange(other._owns_scope, false);
    return *this;
}

PublicSampleInputRef GraphBuilder::input()
try {
    return details::iv_builder_scope_input(_session, _scope,
        details::make_default_boundary_input_config({}));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
PublicSampleInputRef GraphBuilder::input(
    Sample value, std::optional<Sample> min, std::optional<Sample> max)
try {
    return details::iv_builder_scope_input(_session, _scope,
        details::make_sample_boundary_input_config({}, {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        }, value, min, max));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
PublicEventInputRef GraphBuilder::event_input(EventTypeId type)
try {
    return details::iv_builder_scope_event_input(_session, _scope, {}, type);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
PublicSampleInputRef GraphBuilder::input_named(
    std::string_view name, ContinuousInputConfig config)
try {
    config.name = name;
    return details::iv_builder_scope_input(_session, _scope, std::move(config));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
PublicEventInputRef GraphBuilder::event_input_named(
    std::string_view name, EventTypeId type)
try {
    return details::iv_builder_scope_event_input(_session, _scope, name, type);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

void GraphBuilder::outputs(std::initializer_list<NamedRef> refs)
try {
    outputs(std::span<NamedRef const>(refs.begin(), refs.size()));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::outputs(std::span<NamedRef const> refs)
try {
    details::iv_builder_scope_named_outputs(_session, _scope, refs);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::outputs(std::span<SampleOutputRequest const> refs)
try {
    details::iv_builder_scope_outputs(_session, _scope, refs);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::event_outputs(std::span<EventOutputRequest const> refs)
try {
    details::iv_builder_scope_event_outputs(_session, _scope, refs);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef GraphBuilder::subgraph(SubgraphClosure&& closure, std::string_view kind)
try {
    return subgraph(closure, kind);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef GraphBuilder::subgraph(SubgraphClosure& closure, std::string_view kind)
try {
    return std::visit([&](auto& alternative) -> NodeRef {
        using Alternative = std::remove_cvref_t<decltype(alternative)>;
        if constexpr (std::same_as<Alternative,
                          std::reference_wrapper<ConfiguredGraph const>>) {
            return NodeRef(_session, embed(alternative.get(), kind).root_scope);
        } else if constexpr (std::same_as<Alternative,
                                 std::shared_ptr<ConfiguredGraph const>>) {
            return NodeRef(_session, embed(*alternative, kind).root_scope);
        } else {
            return subgraph(
                [&](SubgraphBuilder& boundary) { alternative(boundary); }, kind);
        }
    }, closure._value);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef GraphBuilder::subgraph(GraphBuilder&& nested, std::string_view kind)
try {
    if (!nested._session || nested._session != _session) {
        details::error(
            "a nested GraphBuilder can only be finalized in its own builder session");
    }
    if (!nested._owns_scope) {
        details::error(
            "only an unattached GraphBuilder::nested() view can be finalized as a subgraph");
    }
    auto const handle = details::finish_builder_scope(
        _session, nested._scope, kind, std::nullopt);
    nested._owns_scope = false;
    return NodeRef(_session, handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

GraphBuilder GraphBuilder::nested()
try {
    auto const scope = details::begin_builder_scope(
        _session, details::BuilderScopeKind::definition);
    return GraphBuilder(_session, scope, false, true);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

std::expected<ConfiguredGraph, ConfiguredGraphSnapshotError>
GraphBuilder::configured_graph() const
try {
    return state(*this).snapshot_scope(_scope);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

SubgraphClosure::SubgraphClosure(std::shared_ptr<ConfiguredGraph const> graph)
    : _value(std::move(graph))
{
    if (!std::get<std::shared_ptr<ConfiguredGraph const>>(_value)) {
        details::error("a SubgraphClosure cannot hold an empty ConfiguredGraph");
    }
}

NodeRef GraphBuilder::configure_runtime_binary_op(
    SamplePortRef lhs,
    SamplePortRef rhs,
    std::string_view op_name,
    details::NodeBuildRequest const& request)
try {
    if (lhs.session != _session || rhs.session != _session) {
        details::error(std::string(op_name)
            + ": operands belong to different builder sessions");
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
    NodeRef result(_session, handle);
    std::array<details::NodeCallSampleInput, 2> inputs{{
        {.source = std::move(lhs),
         .name = {},
         .input_index = 0,
         .target = details::NodeCallInputTarget::explicit_index},
        {.source = std::move(rhs),
         .name = {},
         .input_index = 1,
         .target = details::NodeCallInputTarget::explicit_index},
    }};
    result.apply_node_call(
        {.data = inputs.data(), .size = inputs.size()}, {});
    return result;
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

SamplePortRef GraphBuilder::lift_to_sample_port(SamplePortRef const& value)
try {
    return state(*this).require_sample_port(value);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
SamplePortRef GraphBuilder::lift_to_sample_port(SamplePortRef&& value)
try {
    return state(*this).require_sample_port(value);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
SamplePortRef GraphBuilder::lift_to_sample_port(NamedRef const& value)
try {
    return std::visit(
        [&](auto const& concrete) -> SamplePortRef {
            using T = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<T, EventPortRef>) {
                details::error("expected sample value, got event");
            } else {
                return lift_to_sample_port(concrete);
            }
        },
        value.value);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

void GraphBuilder::connect_sample_input(NodeBundlePortId target, SamplePortRef source)
try {
    details::iv_builder_connect_sample_input(_session, target, source);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::connect_sample_input(
    NodeBundlePortId target, std::span<SamplePortRef const> sources)
try {
    details::iv_builder_connect_sample_inputs(_session, target, sources);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::connect_event_input(NodeBundlePortId target, EventPortRef source)
try {
    details::iv_builder_connect_event_input(_session, target, source);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
bool GraphBuilder::sample_input_is_connected(NodeBundlePortId target) const
try {
    return details::iv_builder_sample_input_is_connected(_session, target);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
bool GraphBuilder::event_input_is_connected(NodeBundlePortId target) const
try {
    return details::iv_builder_event_input_is_connected(_session, target);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
EventPortRef GraphBuilder::event_output(NodeBundlePortId source) const
try {
    if (source.port_kind != PortKind::event)
        details::error(
            "attempted to read an event output from a sample NodeBundle port");
    return details::iv_builder_event_port_from_output(_session, source);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
size_t GraphBuilder::sample_port_index(
    NodeBundleHandle handle, bool inputs, std::string_view name) const
try {
    return details::iv_builder_sample_port_index(_session, handle, inputs, name);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
size_t GraphBuilder::event_port_index(
    NodeBundleHandle handle, bool inputs, std::string_view name) const
try {
    return details::iv_builder_event_port_index(_session, handle, inputs, name);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
size_t GraphBuilder::sample_input_count(NodeBundleHandle handle) const
try {
    return details::iv_builder_sample_input_count(_session, handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
size_t GraphBuilder::sample_output_count(NodeBundleHandle handle) const
try {
    return details::iv_builder_sample_output_count(_session, handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
size_t GraphBuilder::event_input_count(NodeBundleHandle handle) const
try {
    return details::iv_builder_event_input_count(_session, handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
size_t GraphBuilder::event_output_count(NodeBundleHandle handle) const
try {
    return details::iv_builder_event_output_count(_session, handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
NodeBundlePortId GraphBuilder::input_port_at(
    NodeBundleHandle handle, size_t position) const
try {
    return details::iv_builder_input_port_at(_session, handle, position);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
NodeBundleHandle GraphBuilder::tiled_member(
    NodeBundleHandle handle, size_t channel) const
try {
    return details::iv_builder_tiled_member(_session, handle, channel);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
NodePorts const& GraphBuilder::typed_ports(NodeBundleHandle handle) const
try {
    return details::iv_builder_typed_ports(_session, handle);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
SamplePortRef GraphBuilder::sample_port_from_output(NodeBundlePortId port)
try {
    return details::iv_builder_sample_port_from_output(_session, port);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
EventPortRef GraphBuilder::event_port_from_output(NodeBundlePortId port) const
try {
    return details::iv_builder_event_port_from_output(_session, port);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
SamplePortRef GraphBuilder::make_sample_port(
    ChannelTypeId type, std::span<SampleOutputChannelId const> channels)
try {
    return details::iv_builder_make_sample_port(_session, type, channels);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
SamplePortRef GraphBuilder::make_tiled_sample_port(
    ChannelTypeId type, std::span<SamplePortRef const> members)
try {
    return details::iv_builder_make_tiled_sample_port(_session, type, members);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
SamplePortRef GraphBuilder::select_sample_port_channel(
    SamplePortRef const& port, size_t channel)
try {
    return details::iv_builder_select_sample_port_channel(
        state(*this).require_sample_port(port), channel);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
std::span<SampleOutputChannelId const>
GraphBuilder::sample_port_channels(SamplePortRef const& ref) const
try {
    return details::iv_builder_sample_port_channels(
        state(*this).require_sample_port(ref));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
EventPortRef GraphBuilder::make_event_port(
    EventTypeId type, std::span<EventOutputPortId const> sources)
try {
    return details::iv_builder_make_event_port(_session, type, sources);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
EventPortRef GraphBuilder::make_event_port(
    std::span<EventPortRef const> sources)
try {
    return details::iv_builder_merge_event_ports(_session, sources);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
std::span<EventOutputPortId const>
GraphBuilder::event_port_sources(EventPortRef const& ref) const
try {
    return details::iv_builder_event_port_sources(
        state(*this).require_event_port(ref));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
SamplePortRef GraphBuilder::detach_sample_port(
    SamplePortRef const& source, size_t latency,
    std::optional<Sample> initial_value)
try {
    return details::iv_builder_detach_sample_port(
        state(*this).require_sample_port(source), latency, initial_value);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
EventPortRef GraphBuilder::detach_event_port(
    EventPortRef const& source, size_t latency)
try {
    return details::iv_builder_detach_event_port(
        state(*this).require_event_port(source), latency);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::apply_ttl(NodeBundleHandle handle, size_t samples)
try {
    details::iv_builder_apply_ttl(_session, handle, samples);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::annotate_node(
    NodeBundleHandle handle, std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end)
try {
    details::iv_builder_annotate_node(_session, handle, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::annotate_node_input_source_info(
    NodeBundleHandle handle, PortKind port_kind,
    std::string_view port_name, std::string_view id,
    std::string_view file, uint32_t begin, uint32_t end)
try {
    details::iv_builder_annotate_node_input(
        _session, handle, port_kind, port_name, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::annotate_public_sample_output_source_info(
    std::span<SourceInfo const> infos)
try {
    state(*this).annotate_scope_sample_output_source_info(_scope, infos);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::annotate_public_event_output_source_info(
    std::span<SourceInfo const> infos)
try {
    state(*this).annotate_scope_event_output_source_info(_scope, infos);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::annotate_public_sample_output_source_info(
    size_t index, SourceInfo info)
try {
    state(*this).annotate_scope_sample_output_source_info(_scope, index, std::move(info));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
void GraphBuilder::annotate_public_event_output_source_info(
    size_t index, SourceInfo info)
try {
    state(*this).annotate_scope_event_output_source_info(_scope, index, std::move(info));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

ConfiguredGraphEmbedding GraphBuilder::embed(
    ConfiguredGraph const& child, std::string_view kind)
try {
    return state(*this).embed_configured_graph(child, kind);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

ConfiguredGraph GraphBuilder::finish() const &
try {
    return state(*this).finish();
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}
ConfiguredGraph GraphBuilder::finish() &&
try {
    if (!_session) throw std::logic_error("cannot finish an empty GraphBuilder");
    if (_scope != details::root_builder_scope_id) {
        throw std::logic_error("only the root GraphBuilder view can finish its session");
    }
    return details::take_built_graph(_session);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::record_builder_failure(_session, std::current_exception(), {});
    throw;
}

} // namespace iv
