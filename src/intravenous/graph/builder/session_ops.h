#pragma once

// Module-facing ref operations. Every live NodeRef/SamplePortRef/EventPortRef
// carries the BuilderSession that owns its handles, so these operate on that
// session directly rather than on whichever GraphBuilder view created the ref.
// libiv_builder validates that every ref passed together belongs to one session.

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/graph/node_ports.h>
#include <intravenous/graph/port_ids.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace iv::details {
struct BuilderSession;

[[nodiscard]] SamplePortRef iv_builder_require_sample_port(
    BuilderSession*, SamplePortRef const&);
void iv_builder_connect_sample_input(
    BuilderSession*, NodeBundlePortId, SamplePortRef const&);
void iv_builder_connect_sample_inputs(
    BuilderSession*, NodeBundlePortId, std::span<SamplePortRef const>);
void iv_builder_connect_event_input(
    BuilderSession*, NodeBundlePortId, EventPortRef const&);
[[nodiscard]] bool iv_builder_sample_input_is_connected(
    BuilderSession*, NodeBundlePortId);
[[nodiscard]] bool iv_builder_event_input_is_connected(
    BuilderSession*, NodeBundlePortId);
[[nodiscard]] std::size_t iv_builder_sample_port_index(
    BuilderSession*, NodeBundleHandle, bool inputs, std::string_view name);
[[nodiscard]] std::size_t iv_builder_event_port_index(
    BuilderSession*, NodeBundleHandle, bool inputs, std::string_view name);
[[nodiscard]] std::size_t iv_builder_sample_input_count(
    BuilderSession*, NodeBundleHandle);
[[nodiscard]] std::size_t iv_builder_sample_output_count(
    BuilderSession*, NodeBundleHandle);
[[nodiscard]] std::size_t iv_builder_event_input_count(
    BuilderSession*, NodeBundleHandle);
[[nodiscard]] std::size_t iv_builder_event_output_count(
    BuilderSession*, NodeBundleHandle);
[[nodiscard]] NodeBundlePortId iv_builder_input_port_at(
    BuilderSession*, NodeBundleHandle, std::size_t position);
[[nodiscard]] NodePorts const& iv_builder_typed_ports(
    BuilderSession*, NodeBundleHandle);
[[nodiscard]] NodeBundleHandle iv_builder_tiled_member(
    BuilderSession*, NodeBundleHandle, std::size_t channel);
[[nodiscard]] SamplePortRef iv_builder_sample_port_from_output(
    BuilderSession*, NodeBundlePortId);
[[nodiscard]] EventPortRef iv_builder_event_port_from_output(
    BuilderSession*, NodeBundlePortId);
[[nodiscard]] SamplePortRef iv_builder_make_sample_port(
    BuilderSession*, ChannelTypeId, std::span<SampleOutputChannelId const>);
[[nodiscard]] SamplePortRef iv_builder_make_tiled_sample_port(
    BuilderSession*, ChannelTypeId, std::span<SamplePortRef const>);
[[nodiscard]] SamplePortRef iv_builder_select_sample_port_channel(
    SamplePortRef const&, std::size_t channel);
[[nodiscard]] std::span<SampleOutputChannelId const>
iv_builder_sample_port_channels(SamplePortRef const&);
[[nodiscard]] EventPortRef iv_builder_make_event_port(
    BuilderSession*, EventTypeId, std::span<EventOutputPortId const>);
[[nodiscard]] EventPortRef iv_builder_merge_event_ports(
    BuilderSession*, std::span<EventPortRef const>);
[[nodiscard]] std::span<EventOutputPortId const>
iv_builder_event_port_sources(EventPortRef const&);
[[nodiscard]] SamplePortRef iv_builder_detach_sample_port(
    SamplePortRef const&, std::size_t loop_extra_latency,
    std::optional<Sample> initial_value);
[[nodiscard]] EventPortRef iv_builder_detach_event_port(
    EventPortRef const&, std::size_t loop_extra_latency);
void iv_builder_apply_ttl(BuilderSession*, NodeBundleHandle, std::size_t samples);
void iv_builder_annotate_node(
    BuilderSession*, NodeBundleHandle, std::string_view declaration_identity,
    std::string_view file_path, std::uint32_t begin, std::uint32_t end);
void iv_builder_annotate_node_input(
    BuilderSession*, NodeBundleHandle, PortKind, std::string_view port_name,
    std::string_view declaration_identity, std::string_view file_path,
    std::uint32_t begin, std::uint32_t end);
void iv_builder_annotate_sample_port(
    SamplePortRef const&, std::string_view declaration_identity,
    std::string_view file_path, std::uint32_t begin, std::uint32_t end);
void iv_builder_annotate_event_port(
    EventPortRef const&, std::string_view declaration_identity,
    std::string_view file_path, std::uint32_t begin, std::uint32_t end);
void iv_builder_annotate_public_sample_input(
    SamplePortRef const&, std::string_view declaration_identity,
    std::string_view file_path, std::uint32_t begin, std::uint32_t end);
void iv_builder_annotate_public_event_input(
    EventPortRef const&, std::string_view declaration_identity,
    std::string_view file_path, std::uint32_t begin, std::uint32_t end);
} // namespace iv::details
