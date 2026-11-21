#pragma once

#include <set>

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph_jit/output_gain_runtime.h>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/basic_nodes/output_gain.h>
#include <intravenous/packages/node_definition_types.h>
#include <intravenous/packages/package_pipeline_types.h>

#include <map>
#include <cmath>

namespace iv::graph_jit::detail {
struct OutputGainStage {
    NodeBundleHandle bundle = 0;
    NodeBundlePortId source{};
    bool tick = false;
    std::shared_ptr<OutputGainConfiguration const> configuration{};
};
struct OutputGainExpansion {
    std::vector<OutputGainStage> stages{};
    std::map<NodeBundlePortId, NodeBundlePortId, NodeBundlePortIdLess> outputs{};
    // A recording's random-access reads use its retained bytes, not the live
    // Tick gain's current Sequential timeline.
    std::map<NodeBundlePortId, NodeBundlePortId, NodeBundlePortIdLess> background_outputs{};

    // Keep an aggregate port's gain distinct from a gain on one tiled child.
    // Opening a tiled route only when necessary preserves channel positions
    // while making a selected child's gain visible to aggregate consumers.
    [[nodiscard]] SampleOutputChannelId routed_source(GraphBuilderNodeBundles const& bundles,
        SampleOutputChannelId source, bool random_access = false,
        std::optional<NodeBundlePortId> raw_ingress = std::nullopt) const
    {
        NodeBundlePortId const id{source.bundle, PortKind::sample, source.port};
        auto const& views = random_access ? background_outputs : outputs;
        if (id != raw_ingress) {
            auto found = views.find(id);
            if (found == views.end() && random_access) {
                auto live = outputs.find(id);
                if (live != outputs.end()) return {live->second.node_bundle_handle, live->second.port_index, source.channel};
            } else if (found != views.end()) return {found->second.node_bundle_handle, found->second.port_index, source.channel};
        }
        if (bundles.bundle(source.bundle).is_tiled()) {
            SampleOutputChannelId const child{
                bundles.tiled_member(source.bundle, source.channel), source.port, 0};
            auto transformed = routed_source(bundles, child, random_access, raw_ingress);
            if (transformed != child) return transformed;
        }
        return source;
    }
};

inline iv::details::NodeCompilerRecord const& output_gain_compiler_record(
    NodeDefinitionsSnapshot const& definitions, NodeCodeKey key)
{
    auto const& revisions = definitions.package_revisions;
    auto const builtin = std::ranges::find_if(revisions, [](auto const& revision) {
        return revision->provenance == PackageProvenance::built_in
            && revision->package_namespace == "builtin";
    });
    auto const providers = builtin == revisions.end()
        ? std::span{revisions} : std::span{builtin, std::size_t{1}};
    iv::details::NodeCompilerRecord const* record = nullptr;
    for (auto const& provider : providers) {
        auto const found = std::ranges::find(
            provider->node_compiler_records, key, &iv::details::NodeCompilerRecord::code_key);
        if (found == provider->node_compiler_records.end()) continue;
        if (record) throw std::invalid_argument("more than one pinned package provides the output gain type");
        record = &*found;
    }
    if (!record) throw std::invalid_argument("the pinned packages have no compiler record for the output gain type");
    return *record;
}

template<ChannelTypeId Channels, SampleStreamLayout Layout>
ConcreteNode make_tick_output_gain(NodeDefinitionsSnapshot const& definitions)
{
    using Gain = OutputGainNode<Channels, Layout>;
    auto storage = std::make_shared<Gain const>();
    auto const& record = output_gain_compiler_record(definitions, iv::details::node_code_key_v<Gain>);
    iv::details::NodeBuildRequest const request{
        .compiler_record = &record,
        .config = storage.get(),
        .config_size = sizeof(Gain),
        .config_alignment = alignof(Gain),
        .describe = &iv::details::describe_node<Gain>,
    };
    return GraphBuilderNodeBundles::make_concrete_node(
        iv::details::materialize_node_description(request, std::move(storage)));
}

inline ConcreteNode make_tick_output_gain(
    ChannelLayout layout, NodeDefinitionsSnapshot const& definitions)
{
    switch (layout.channel_type) {
#define IV_MAKE_TICK_OUTPUT_GAIN(name, ...) \
    case ChannelTypeId::name: \
        return layout.sample_layout == SampleStreamLayout::planar \
            ? make_tick_output_gain<ChannelTypeId::name, SampleStreamLayout::planar>(definitions) \
            : make_tick_output_gain<ChannelTypeId::name, SampleStreamLayout::interleaved>(definitions);
    IV_CHANNEL_TYPES(IV_MAKE_TICK_OUTPUT_GAIN)
#undef IV_MAKE_TICK_OUTPUT_GAIN
    case ChannelTypeId::count: break;
    }
    throw std::invalid_argument("invalid channel type for output gain");
}

inline OutputGainExpansion expand_output_gains(ConfiguredGraph& graph, NodeDefinitionsSnapshot const& definitions,
    std::span<SampleOutputChannelId const> observed_outputs = {})
{
    OutputGainExpansion result;
    std::vector<ConfiguredManualControl> controls;
    std::optional<ResolvedPortRealization> realization;
    std::map<std::pair<ChannelTypeId, SampleStreamLayout>, ConcreteNode> tick_prototypes;
    auto tick_node = [&](ChannelLayout layout) -> ConcreteNode {
        auto key = std::pair{layout.channel_type, layout.sample_layout};
        if (auto found = tick_prototypes.find(key); found != tick_prototypes.end()) return found->second;
        auto prototype = make_tick_output_gain(layout, definitions);
        tick_prototypes.emplace(key, prototype);
        return prototype;
    };
    auto const connections = graph.connections.configured_sample_connections();
    std::vector<ConfiguredSampleConnection> samples(connections.begin(), connections.end());
    std::set<NodeBundlePortId, NodeBundlePortIdLess> used_outputs;
    if (std::ranges::any_of(graph.manual_controls, [](auto const& control) { return is_virtual_output_gain(control.id); })) {
        auto mark_output_used = [&](auto channel) {
            for (;;) {
                used_outputs.insert({channel.bundle, PortKind::sample, channel.port});
                if (!graph.node_bundles.bundle(channel.bundle).is_tiled()) break;
                channel = {graph.node_bundles.tiled_member(channel.bundle, channel.channel), channel.port, 0};
            }
        };
        for (auto const& connection : connections) for (auto const& contribution : connection.source_channels)
            mark_output_used(contribution.channel);
        for (auto channel : observed_outputs) mark_output_used(channel);
    }
    for (auto const& control : graph.manual_controls) {
        if (!control.output) { controls.push_back(control); continue; }
        if (!std::isfinite(control.settled_value.value))
            throw std::invalid_argument("output gain must be finite");
        auto const source = *control.output;
        if (is_virtual_output_gain(control.id) && !control.dynamic && !used_outputs.contains(source)) continue;
        auto output = graph.node_bundles.resolve_sample_output(source).config;
        auto const sequential_source = is_sequential_output(output);
        auto const tick = sequential_source || !control.requires_preparation;
        auto const domain = continuous_value_type_domain(output);
        auto type = domain.sole_type();
        if (!type) {
            if (!realization) {
                auto resolved = resolve_port_realization(graph);
                if (!resolved) throw std::invalid_argument("cannot resolve output gain value type");
                realization.emplace(std::move(*resolved));
            }
            type = realization->output(source.node_bundle_handle, source.port_index).value_type;
        }
        if (!continuous_value_type_info(*type).apply_output_gain)
            throw std::invalid_argument("output value type has no scalar gain operation");
        if (!control.dynamic && control.settled_value == Sample{1}) continue;
        ContinuousInputConfig input{.name = "signal", .channel_layout = output.channel_layout};
        if (tick) {
            SequentialContinuousInputProperties properties;
            properties.value_metadata.clear();
            for (std::size_t type = 0; type < static_cast<std::size_t>(ContinuousValueTypeId::count); ++type)
                if (domain.contains(static_cast<ContinuousValueTypeId>(type)))
                    properties.value_metadata.push_back(default_sequential_input_metadata(static_cast<ContinuousValueTypeId>(type)));
            input.access = std::move(properties);
        } else if (output_production(output) == OutputProductionKind::region) {
            RegionContinuousInputProperties properties;
            properties.value_metadata.clear();
            for (std::size_t type = 0; type < static_cast<std::size_t>(ContinuousValueTypeId::count); ++type)
                if (domain.contains(static_cast<ContinuousValueTypeId>(type)))
                    properties.value_metadata.push_back(default_region_input_metadata(static_cast<ContinuousValueTypeId>(type)));
            input.access = std::move(properties);
        } else {
            CoverageContinuousInputProperties properties;
            properties.value_metadata.clear();
            for (std::size_t type = 0; type < static_cast<std::size_t>(ContinuousValueTypeId::count); ++type)
                if (domain.contains(static_cast<ContinuousValueTypeId>(type)))
                    properties.value_metadata.push_back(default_coverage_input_metadata(static_cast<ContinuousValueTypeId>(type)));
            input.access = std::move(properties);
        }
        output.name = "scaled";
        if (tick) {
            if (!sequential_source) output.production = default_sequential_output_properties(domain);
            output.realization = OutputRealization::computed;
        }
        auto config = std::make_shared<OutputGainConfiguration const>(OutputGainConfiguration{control.settled_value});
        ConcreteNode stage{
            .ports = {.input_configs = {make_input_config(input)}, .output_configs = {make_output_config(output)}},
            .node_storage = config,
            .node_size = sizeof(OutputGainConfiguration),
            .node_alignment = alignof(OutputGainConfiguration),
            .type_identity = NodeTypeIdentity{.value = "compiler.output_gain"},
            .replay_mode = tick ? TickReplayMode::input_union : TickReplayMode::disabled,
        };
        PortConstraintRef const signal{PortConstraintDirection::input, PortKind::sample, 0};
        PortConstraintRef const out{PortConstraintDirection::output, PortKind::sample, 0};
        stage.port_constraints.value_type_equalities.push_back({signal, out});
        stage.port_constraints.value_type_anchors.push_back({out, *type});
        stage.port_constraints.size_equalities.push_back({signal, out});
        stage.port_constraints.pace_equalities.push_back({signal, out});
        stage.operations.runtime.node_data = config.get();
        stage.operations.runtime.declare_node = &declare_output_gain;
        if (tick) {
            auto ordinary = tick_node(output.channel_layout);
            auto knob = ordinary.ports.input_configs.at(1);
            ordinary.ports = stage.ports;
            ordinary.port_constraints = stage.port_constraints;
            stage = std::move(ordinary);
            stage.ports.input_configs.push_back(std::move(knob));
            stage.port_constraints.pace_equalities.push_back({{PortConstraintDirection::input, PortKind::sample, 1}, out});
        } else {
            stage.operations.runtime.has_propagate_forward_coverage = true;
            stage.operations.runtime.has_propagate_reverse_coverage = true;
        }
        auto const bundle = graph.node_bundles.append_concrete(std::move(stage));
        graph.node_bundles.bundle(bundle).set_output_gain_source(source);
        if (!result.outputs.emplace(source, NodeBundlePortId{bundle, PortKind::sample, 0}).second)
            throw std::invalid_argument("two gains resolve to the same concrete output");
        result.stages.push_back({bundle, source, tick, config});
        if (sequential_source && control.requires_preparation
            && is_buffered(graph.node_bundles.resolve_sample_output(source).config.realization)) {
            // Historical reads are Coverage-shaped, regardless of the live
            // Sequential declaration. Preserve the recording's exact Regions
            // and complete-buffer capability with an ordinary Tock scaling
            // stage; both stages use this frozen prepared gain snapshot.
            CoverageContinuousInputProperties properties;
            properties.value_metadata.clear();
            properties.value_metadata.push_back(default_coverage_input_metadata(*type));
            input.access = std::move(properties);
            output.production = default_coverage_output_properties(continuous_value_type_domain(output));
            output.realization = OutputRealization::buffered;
            ConcreteNode historical{
                .ports = {.input_configs = {make_input_config(input)}, .output_configs = {make_output_config(output)}},
                .node_storage = config, .node_size = sizeof(OutputGainConfiguration),
                .node_alignment = alignof(OutputGainConfiguration),
                .type_identity = NodeTypeIdentity{.value = "compiler.recording_output_gain"}};
            historical.port_constraints.value_type_equalities.push_back({signal, out});
            historical.port_constraints.value_type_anchors.push_back({out, *type});
            historical.port_constraints.size_equalities.push_back({signal, out});
            historical.port_constraints.pace_equalities.push_back({signal, out});
            historical.operations.runtime.node_data = config.get();
            historical.operations.runtime.declare_node = &declare_output_gain;
            historical.operations.runtime.has_propagate_forward_coverage = true;
            historical.operations.runtime.has_propagate_reverse_coverage = true;
            auto const view = graph.node_bundles.append_concrete(std::move(historical));
            graph.node_bundles.bundle(view).set_output_gain_source(source);
            result.background_outputs.emplace(source, NodeBundlePortId{view, PortKind::sample, 0});
            result.stages.push_back({view, source, false, config});
        }
        if (tick) {
            auto lowered = control;
            lowered.output.reset();
            lowered.input = {bundle, PortKind::sample, 1};
            lowered.dynamic &= !lowered.requires_preparation;
            lowered.override_connections = false;
            controls.push_back(std::move(lowered));
        }
    }
    if (result.stages.empty()) {
        graph.manual_controls = std::move(controls);
        return result;
    }
    std::vector<bool> tick_owners;
    graph.node_bundles.for_each_configured_bundle([&](ConfiguredNodeBundleView const& view) {
        tick_owners.push_back(view.operations && view.operations->runtime.tick_block);
    });
    for (auto& connection : samples) for (auto& source : connection.source_channels) {
        bool const random_access = std::ranges::any_of(connection.target_channels, [&](auto target) {
            return !is_sequential(graph.node_bundles.resolve_sample_input({target.bundle, PortKind::sample, target.port}).config)
                || (graph.node_bundles.bundle(target.bundle).is_concrete() && !tick_owners[target.bundle]);
        });
        source.channel = result.routed_source(graph.node_bundles, source.channel, random_access);
    }
    for (auto const& stage : result.stages) {
        auto const output = graph.node_bundles.resolve_sample_output(stage.source).config;
        ConfiguredSampleConnection ingress{.source_type = output.channel_layout.channel_type,
            .target_type = output.channel_layout.channel_type};
        for (std::size_t channel = 0; channel < channel_count(output.channel_layout); ++channel) {
            auto source = result.routed_source(graph.node_bundles,
                {stage.source.node_bundle_handle, stage.source.port_index, channel}, !stage.tick, stage.source);
            ingress.source_channels.push_back({.channel = source});
            ingress.target_channels.push_back({stage.bundle, 0, channel});
        }
        samples.push_back(std::move(ingress));
    }
    graph.connections = GraphBuilderConnections::from_configured_connections(samples,
        graph.connections.configured_event_connections());
    graph.manual_controls = std::move(controls);
    return result;
}
} // namespace iv::graph_jit::detail
