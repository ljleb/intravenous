#include <intravenous/graph/builder.h>

#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace iv {
NodeAddress node_message_target(
    NodeAddress const& context_address,
    NodeRef const& target)
{
    if (!target._has_semantic_message_path()) {
        details::error(
            "captured NodeRef has no stable semantic path for node-message dispatch");
    }
    auto result = context_address;
    auto const path = target._semantic_message_path();
    result.path.insert(result.path.end(), path.begin(), path.end());
    return result;
}

namespace details {
SamplePortRef make_tiled_sample_port(
    ChannelTypeId type, SamplePortRef const* members, size_t member_count)
{
    if (member_count == 0) error("cannot tile an empty sample output");
    auto* const session = members[0].session;
    if (!session) error("cannot tile an empty sample output");
    return iv_builder_make_tiled_sample_port(
        session, type, std::span<SamplePortRef const>(members, member_count));
}
} // namespace details

SamplePortRef::SamplePortRef(GraphBuilder& builder, NodeBundlePortId port)
{
    *this = builder.sample_port_from_output(port);
}

SamplePortRef::SamplePortRef(
    GraphBuilder& builder, ChannelTypeId type,
    std::span<SampleOutputChannelId const> channels)
{
    *this = builder.make_sample_port(type, channels);
}

SamplePortRef SamplePortRef::_clone_handle() const { return *this; }

std::span<SampleOutputChannelId const> SamplePortRef::channels() const
try {
    if (!session)
        details::error("attempted to read channels from an empty sample port");
    return details::iv_builder_sample_port_channels(*this);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(session, std::current_exception(), {});
    throw;
}

SamplePortRef SamplePortRef::select_channel(size_t channel) const
try {
    if (!session)
        details::error("attempted to select a channel from an empty sample port");
    return details::iv_builder_select_sample_port_channel(*this, channel);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(session, std::current_exception(), {});
    throw;
}

SamplePortRef SamplePortRef::detach(
    size_t latency, std::optional<Sample> initial_value) const
try {
    if (!session)
        details::error("attempted to detach an empty sample port");
    return details::iv_builder_detach_sample_port(*this, latency, initial_value);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(session, std::current_exception(), {});
    throw;
}

std::string SamplePortRef::to_string() const
{
    if (!session) return "empty sample port";
    auto const semantic_channels = channels();
    if (semantic_channels.size() == 1) {
        auto const channel = semantic_channels.front();
        return "sample output channel " + std::to_string(channel.channel)
            + " of port " + std::to_string(channel.port)
            + " on node bundle " + std::to_string(channel.bundle);
    }
    return "sample expression with "
        + std::to_string(semantic_channels.size()) + " channel(s)";
}

void SamplePortRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const
try {
    if (session)
        details::iv_builder_annotate_sample_port(*this, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(session, std::current_exception(), {});
    throw;
}

EventPortRef::EventPortRef(GraphBuilder& builder, NodeBundlePortId port)
{
    if (port.port_kind != PortKind::event) {
        details::error(
            "attempted to create an EventPortRef from a sample NodeBundle port");
    }
    *this = builder.event_port_from_output(port);
}

EventPortRef::EventPortRef(
    GraphBuilder& builder, EventTypeId type,
    std::span<EventOutputPortId const> sources)
{
    *this = builder.make_event_port(type, sources);
}

EventPortRef::EventPortRef(
    GraphBuilder& builder, std::span<EventPortRef const> sources)
{
    *this = builder.make_event_port(sources);
}

std::span<EventOutputPortId const> EventPortRef::sources() const
try {
    if (!session)
        details::error("attempted to read sources from an empty event port");
    return details::iv_builder_event_port_sources(*this);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(session, std::current_exception(), {});
    throw;
}

EventPortRef EventPortRef::detach(size_t latency) const
try {
    if (!session)
        details::error("attempted to detach an empty event port");
    return details::iv_builder_detach_event_port(*this, latency);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(session, std::current_exception(), {});
    throw;
}

std::string EventPortRef::to_string() const
{
    if (!session) return "empty event";
    auto const semantic_sources = sources();
    if (semantic_sources.size() == 1) {
        return "event output " + std::to_string(semantic_sources.front().port)
            + " of node bundle " + std::to_string(semantic_sources.front().bundle);
    }
    return "event expression with "
        + std::to_string(semantic_sources.size()) + " source(s)";
}

void EventPortRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const
try {
    if (session)
        details::iv_builder_annotate_event_port(*this, id, file, begin, end);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(session, std::current_exception(), {});
    throw;
}

void PublicSampleInputRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const
{
    if (port.session)
        details::iv_builder_annotate_public_sample_input(
            port, id, file, begin, end);
}

void PublicEventInputRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const
{
    if (port.session)
        details::iv_builder_annotate_public_event_input(
            port, id, file, begin, end);
}

NodeRef::NodeRef(GraphBuilder const& graph_builder, size_t index)
    : _session(graph_builder._session)
    , _index(index)
{}

NodeRef NodeRef::node_ref() const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    return _clone_handle();
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef::NodeRef(NodeRef const& other)
    : _session(other._session)
    , _index(other._index)
    , _has_source_identity(other._has_source_identity)
    , _has_message_path(other._has_message_path)
    , _message_path(other._message_path)
{}

NodeRef::NodeRef(NodeRef&& other) noexcept
    : _session(std::exchange(other._session, nullptr))
    , _index(std::exchange(other._index, 0))
    , _has_source_identity(std::exchange(other._has_source_identity, false))
    , _has_message_path(std::exchange(other._has_message_path, false))
    , _message_path(std::move(other._message_path))
{}

NodeRef NodeRef::_clone_handle() const
{
    if (!_session) return NodeRef{};
    NodeRef result(_session, _index);
    result._has_source_identity = _has_source_identity;
    result._has_message_path = _has_message_path;
    result._message_path = _message_path;
    return result;
}

SamplePortRef NodeRef::operator[](size_t port) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    if (port >= details::iv_builder_sample_output_count(_session, _index)) {
        details::error("sample output port is out of bounds on " + to_string());
    }
    return details::iv_builder_sample_port_from_output(
        _session, {_index, PortKind::sample, port});
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

SamplePortRef NodeRef::operator[](std::string_view name) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    return (*this)[details::iv_builder_sample_port_index(
        _session, _index, false, name)];
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef::operator SamplePortRef() const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    if (sample_output_count() == 0) {
        details::error(to_string() + " has no sample output port");
    }
    return (*this)[0];
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

NodeBundlePortId NodeRef::output_port_at(size_t position) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    return details::iv_builder_output_port_at(_session, _index, position);
} catch (...) {
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

size_t NodeRef::sample_input_count() const
try {
    return details::iv_builder_sample_input_count(_session, node_bundle_handle());
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

size_t NodeRef::sample_output_count() const
try {
    return details::iv_builder_sample_output_count(_session, node_bundle_handle());
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

size_t NodeRef::event_input_count() const
try {
    return details::iv_builder_event_input_count(_session, node_bundle_handle());
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

size_t NodeRef::event_output_count() const
try {
    return details::iv_builder_event_output_count(_session, node_bundle_handle());
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

bool NodeRef::input_is_connected(size_t port) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    return details::iv_builder_sample_input_is_connected(_session,
        {_index, PortKind::sample, port});
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

bool NodeRef::event_input_is_connected(size_t port) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    return details::iv_builder_event_input_is_connected(_session,
        {_index, PortKind::event, port});
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

void NodeRef::apply_node_call(
    details::NodeCallSampleInputList sample_inputs,
    details::NodeCallEventInputList event_inputs) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");

    for (size_t i = 0; i < sample_inputs.size; ++i) {
        auto const& input = sample_inputs.data[i];
        if (input.source.session != _session) {
            details::error("sample source belongs to another builder session");
        }
        size_t input_port = 0;
        switch (input.target) {
        case details::NodeCallInputTarget::positional:
            {
                auto const port = details::iv_builder_input_port_at(
                    _session, _index, input.input_index);
                if (port.port_kind != PortKind::sample) {
                    details::error(
                        "positional sample argument does not match the declared input order");
                }
                input_port = port.port_index;
            }
            break;
        case details::NodeCallInputTarget::named:
            input_port = details::iv_builder_sample_port_index(
                _session, _index, true, input.name);
            break;
        case details::NodeCallInputTarget::explicit_index:
            input_port = input.input_index;
            break;
        }
        if (input_port >= sample_input_count()) {
            details::error("too many sample inputs");
        }
        details::iv_builder_connect_sample_input(
            _session, {_index, PortKind::sample, input_port}, input.source);
    }

    for (size_t i = 0; i < event_inputs.size; ++i) {
        auto const& input = event_inputs.data[i];
        if (input.source.session != _session) {
            details::error("event source belongs to another builder session");
        }
        size_t input_port = 0;
        switch (input.target) {
        case details::NodeCallInputTarget::positional:
            {
                auto const port = details::iv_builder_input_port_at(
                    _session, _index, input.input_index);
                if (port.port_kind != PortKind::event) {
                    details::error(
                        "positional event argument does not match the declared input order");
                }
                input_port = port.port_index;
            }
            break;
        case details::NodeCallInputTarget::named:
            input_port = details::iv_builder_event_port_index(
                _session, _index, true, input.name);
            break;
        case details::NodeCallInputTarget::explicit_index:
            input_port = input.input_index;
            break;
        }
        if (input_port >= event_input_count()) {
            details::error("too many event inputs");
        }
        details::iv_builder_connect_event_input(
            _session, {_index, PortKind::event, input_port}, input.source);
    }
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef NodeRef::connect_event_input(size_t port, EventPortRef value) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    if (value.session != _session) {
        details::error("event source belongs to another builder session");
    }
    details::iv_builder_connect_event_input(
        _session, {_index, PortKind::event, port}, value);
    return _clone_handle();
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef NodeRef::connect_event_input(
    std::string_view name, EventPortRef value) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    return connect_event_input(
        details::iv_builder_event_port_index(_session, _index, true, name),
        std::move(value));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

EventPortRef NodeRef::event_port(size_t port) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    if (port >= event_output_count()) {
        details::error("event output port is out of bounds on " + to_string());
    }
    return details::iv_builder_event_port_from_output(
        _session, {_index, PortKind::event, port});
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

EventPortRef NodeRef::event_port(std::string_view name) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    return event_port(details::iv_builder_event_port_index(
        _session, _index, false, name));
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

EventPortRef NodeRef::event_port() const
try {
    if (event_output_count() == 0) {
        details::error(to_string() + " has no event output port");
    }
    return event_port(0);
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef NodeRef::ttl(size_t samples) const
try {
    if (!_session) details::error("attempted to use a null NodeRef");
    details::iv_builder_apply_ttl(_session, _index, samples);
    return _clone_handle();
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

NodeRef NodeRef::no_ttl() const
try {
    return ttl(std::numeric_limits<size_t>::max());
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

std::string NodeRef::to_string() const
{
    return _session
        ? "node bundle " + std::to_string(_index)
        : "empty node";
}

NodeRef& NodeRef::operator=(NodeRef const& rhs)
{
    if (this == &rhs) return *this;
    if (_session || _has_source_identity || _has_message_path
        || !_message_path.empty()) {
        details::error("cannot reassign an initialized NodeRef");
    }
    if (!rhs._session) {
        details::error("cannot initialize NodeRef from empty NodeRef");
    }
    _session = rhs._session;
    _index = rhs._index;
    _has_source_identity = rhs._has_source_identity;
    _has_message_path = rhs._has_message_path;
    _message_path = rhs._message_path;
    return *this;
}

NodeRef& NodeRef::operator=(NodeRef&& rhs)
{
    if (this == &rhs) return *this;
    if (_session || _has_source_identity || _has_message_path
        || !_message_path.empty()) {
        details::error("cannot reassign an initialized NodeRef");
    }
    if (!rhs._session) {
        details::error("cannot initialize NodeRef from empty NodeRef");
    }
    _session = rhs._session;
    _index = rhs._index;
    _has_source_identity = rhs._has_source_identity;
    _has_message_path = rhs._has_message_path;
    _message_path = std::move(rhs._message_path);
    rhs._session = nullptr;
    rhs._index = 0;
    rhs._has_source_identity = false;
    rhs._has_message_path = false;
    rhs._message_path.clear();
    return *this;
}

void NodeRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const
try {
    _has_source_identity = _has_source_identity || !id.empty();
    if (_session) {
        details::iv_builder_annotate_node(_session, _index, id, file, begin, end);
    }
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}

void NodeRef::_annotate_input_source_info(
    PortKind port_kind,
    std::string_view port_name,
    std::string_view id,
    std::string_view file,
    uint32_t begin,
    uint32_t end) const
try {
    _has_source_identity = _has_source_identity || !id.empty();
    if (_session) {
        details::iv_builder_annotate_node_input(
            _session, _index, port_kind, port_name, id, file, begin, end);
    }
} catch (...) {
    // An exception leaving a session operation fails the session.
    details::iv_builder_record_failure(_session, std::current_exception(), {});
    throw;
}
} // namespace iv
