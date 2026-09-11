#include <intravenous/graph/builder.h>

#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace iv {
namespace details {
SamplePortRef make_tiled_sample_port(
    ChannelTypeId type, SamplePortRef const* members, size_t member_count)
{
    if (member_count == 0) {
        error("cannot tile an empty sample output");
    }

    auto* const builder = members[0].graph_builder;
    if (!builder) {
        error("cannot tile an empty sample output");
    }

    std::vector<SampleOutputChannelId> channels;
    channels.reserve(member_count);
    for (size_t i = 0; i < member_count; ++i) {
        auto const& member = members[i];
        if (member.graph_builder != builder) {
            error("cannot tile sample outputs from different builders");
        }
        auto const member_channels = member.channels();
        if (member.channel_type != ChannelTypeId::mono
            || member_channels.size() != 1) {
            error("each g.tile channel must be a scalar sample expression");
        }
        channels.push_back(member_channels.front());
    }
    return SamplePortRef(*builder, type, channels);
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
{
    if (!graph_builder)
        details::error("attempted to read channels from an empty sample port");
    return graph_builder->sample_port_channels(*this);
}

SamplePortRef SamplePortRef::select_channel(size_t channel) const
{
    if (!graph_builder)
        details::error("attempted to select a channel from an empty sample port");
    auto const semantic_channels = channels();
    if (channel >= semantic_channels.size())
        details::error("sample channel ordinal is out of bounds");
    return SamplePortRef(
        *graph_builder, ChannelTypeId::mono,
        std::array{semantic_channels[channel]});
}

SamplePortRef SamplePortRef::detach(size_t latency) const
{
    if (!graph_builder)
        details::error("attempted to detach an empty sample port");
    return graph_builder->detach_sample_port(*this, latency);
}

std::string SamplePortRef::to_string() const
{
    if (!graph_builder) return "empty sample port";
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
{
    if (graph_builder)
        graph_builder->annotate_sample_port_source_info(
            *this, id, file, begin, end);
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

std::span<EventOutputPortId const> EventPortRef::sources() const
{
    if (!graph_builder)
        details::error("attempted to read sources from an empty event port");
    return graph_builder->event_port_sources(*this);
}

std::string EventPortRef::to_string() const
{
    if (!graph_builder) return "empty event";
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
{
    if (graph_builder)
        graph_builder->annotate_event_port_source_info(
            *this, id, file, begin, end);
}

void PublicSampleInputRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const
{
    if (port.graph_builder)
        port.graph_builder->annotate_public_sample_input_source_info(
            *this, id, file, begin, end);
}

void PublicEventInputRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const
{
    if (port.graph_builder)
        port.graph_builder->annotate_public_event_input_source_info(
            *this, id, file, begin, end);
}

NodeRef NodeRef::node_ref() const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    return _clone_handle();
}

NodeRef NodeRef::_clone_handle() const
{
    return _graph_builder ? NodeRef(*_graph_builder, _index) : NodeRef{};
}

SamplePortRef NodeRef::operator[](size_t port) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    if (port >= _graph_builder->sample_output_count(_index)) {
        details::error("sample output port is out of bounds on " + to_string());
    }
    return SamplePortRef(*_graph_builder, {_index, PortKind::sample, port});
}

SamplePortRef NodeRef::operator[](std::string_view name) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    return (*this)[_graph_builder->sample_port_index(_index, false, name)];
}

NodeRef::operator SamplePortRef() const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    if (sample_output_count() != 1) {
        details::error(to_string() + " does not have exactly 1 output port");
    }
    return (*this)[0];
}

size_t NodeRef::sample_input_count() const
{
    return _graph_builder->sample_input_count(node_bundle_handle());
}

size_t NodeRef::sample_output_count() const
{
    return _graph_builder->sample_output_count(node_bundle_handle());
}

size_t NodeRef::event_input_count() const
{
    return _graph_builder->event_input_count(node_bundle_handle());
}

size_t NodeRef::event_output_count() const
{
    return _graph_builder->event_output_count(node_bundle_handle());
}

bool NodeRef::input_is_connected(size_t port) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    return _graph_builder->sample_input_is_connected(
        {_index, PortKind::sample, port});
}

bool NodeRef::event_input_is_connected(size_t port) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    return _graph_builder->event_input_is_connected(
        {_index, PortKind::event, port});
}

void NodeRef::apply_node_call(
    details::NodeCallSampleInputList sample_inputs,
    details::NodeCallEventInputList event_inputs) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");

    size_t positional_sample = 0;
    for (size_t i = 0; i < sample_inputs.size; ++i) {
        auto const& input = sample_inputs.data[i];
        if (input.source.graph_builder != _graph_builder) {
            details::error("sample source belongs to another builder");
        }
        size_t input_port = 0;
        switch (input.target) {
        case details::NodeCallInputTarget::positional:
            input_port = positional_sample++;
            break;
        case details::NodeCallInputTarget::named:
            input_port = _graph_builder->sample_port_index(
                _index, true, input.name);
            break;
        case details::NodeCallInputTarget::explicit_ordinal:
            input_port = input.input_ordinal;
            break;
        }
        if (input_port >= sample_input_count()) {
            details::error("too many sample inputs");
        }
        _graph_builder->connect_sample_input(
            {_index, PortKind::sample, input_port}, input.source);
    }

    size_t positional_event = 0;
    for (size_t i = 0; i < event_inputs.size; ++i) {
        auto const& input = event_inputs.data[i];
        if (input.source.graph_builder != _graph_builder) {
            details::error("event source belongs to another builder");
        }
        size_t input_port = 0;
        switch (input.target) {
        case details::NodeCallInputTarget::positional:
            input_port = positional_event++;
            break;
        case details::NodeCallInputTarget::named:
            input_port = _graph_builder->event_port_index(
                _index, true, input.name);
            break;
        case details::NodeCallInputTarget::explicit_ordinal:
            input_port = input.input_ordinal;
            break;
        }
        if (input_port >= event_input_count()) {
            details::error("too many event inputs");
        }
        _graph_builder->connect_event_input(
            {_index, PortKind::event, input_port}, input.source);
    }
}

NodeRef NodeRef::connect_event_input(size_t port, EventPortRef value) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    if (value.graph_builder != _graph_builder) {
        details::error("event source belongs to another builder");
    }
    _graph_builder->connect_event_input(
        {_index, PortKind::event, port}, std::move(value));
    return _clone_handle();
}

NodeRef NodeRef::connect_event_input(
    std::string_view name, EventPortRef value) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    return connect_event_input(
        _graph_builder->event_port_index(_index, true, name), std::move(value));
}

EventPortRef NodeRef::event_port(size_t port) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    if (port >= event_output_count()) {
        details::error("event output port is out of bounds on " + to_string());
    }
    return _graph_builder->event_output({_index, PortKind::event, port});
}

EventPortRef NodeRef::event_port(std::string_view name) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    return event_port(_graph_builder->event_port_index(_index, false, name));
}

EventPortRef NodeRef::event_port() const
{
    if (event_output_count() != 1) {
        details::error(to_string() + " does not have exactly 1 event output port");
    }
    return event_port(0);
}

NodeRef NodeRef::ttl(size_t samples) const
{
    if (!_graph_builder) details::error("attempted to use a null NodeRef");
    _graph_builder->apply_ttl(_index, samples);
    return _clone_handle();
}

NodeRef NodeRef::no_ttl() const
{
    return ttl(std::numeric_limits<size_t>::max());
}

std::string NodeRef::to_string() const
{
    return _graph_builder
        ? "node bundle " + std::to_string(_index)
        : "empty node";
}

NodeRef& NodeRef::operator=(NodeRef const& rhs)
{
    if (this == &rhs) return *this;
    if (_graph_builder || _has_source_identity) {
        details::error("cannot reassign an initialized NodeRef");
    }
    if (!rhs._graph_builder) {
        details::error("cannot initialize NodeRef from empty NodeRef");
    }
    _graph_builder = rhs._graph_builder;
    _index = rhs._index;
    _has_source_identity = rhs._has_source_identity;
    return *this;
}

NodeRef& NodeRef::operator=(NodeRef&& rhs)
{
    if (this == &rhs) return *this;
    if (_graph_builder || _has_source_identity) {
        details::error("cannot reassign an initialized NodeRef");
    }
    if (!rhs._graph_builder) {
        details::error("cannot initialize NodeRef from empty NodeRef");
    }
    _graph_builder = rhs._graph_builder;
    _index = rhs._index;
    _has_source_identity = rhs._has_source_identity;
    rhs._graph_builder = nullptr;
    rhs._index = 0;
    rhs._has_source_identity = false;
    return *this;
}

void NodeRef::_annotate_source_info(
    std::string_view id, std::string_view file,
    uint32_t begin, uint32_t end) const
{
    _has_source_identity = _has_source_identity || !id.empty();
    if (_graph_builder) {
        _graph_builder->annotate_node(_index, id, file, begin, end);
    }
}
} // namespace iv
