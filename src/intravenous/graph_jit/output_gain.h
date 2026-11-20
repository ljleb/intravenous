#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph_jit/output_gain_runtime.h>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/graph/builder.h>
#include <intravenous/runtime/node_definition_types.h>

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
};

inline OutputGainExpansion expand_output_gains(ConfiguredGraph& graph, NodeDefinitionsSnapshot const& definitions)
{
    OutputGainExpansion result;
    std::vector<ConfiguredManualControl> controls;
    std::optional<ResolvedPortRealization> realization;
    std::map<std::pair<ChannelTypeId, SampleStreamLayout>, ConcreteNode> tick_prototypes;
    auto tick_node = [&](ChannelLayout layout) -> ConcreteNode {
        auto key = std::pair{layout.channel_type, layout.sample_layout};
        if (auto found = tick_prototypes.find(key); found != tick_prototypes.end()) return found->second;
        std::optional<ConcreteNode> tick_prototype;
        {
            auto const id = "builtin.output_gain." + std::string(channel_type_info(layout.channel_type).wire_name)
                + (layout.sample_layout == SampleStreamLayout::planar ? ".planar" : ".interleaved");
            auto definition = definitions.by_id.find(id);
            if (definition == definitions.by_id.end())
                throw std::invalid_argument("the pinned builtin package has no output-gain node");
            auto leaf = std::get_if<LeafNodeDefinition>(&definition->second.definition);
            if (!leaf || !leaf->provider.leaf_build)
                throw std::invalid_argument("the output-gain definition is not a constructible leaf");
            GraphBuilder builder;
            (void)leaf->provider.leaf_build(builder, {}, nullptr);
            builder.outputs();
            auto configured = std::move(builder).finish();
            for (NodeBundleHandle bundle = 0; bundle < configured.node_bundles.size(); ++bundle) {
                if (!configured.node_bundles.bundle(bundle).is_concrete()) continue;
                if (tick_prototype) throw std::invalid_argument("output-gain construction produced several primitives");
                tick_prototype = GraphBuilderNodeBundles::make_concrete_node(
                    configured.node_bundles.materialize_concrete_description(bundle));
                tick_prototype->registered_node_type_identity = RegisteredNodeTypeIdentity{
                    .node_type_id = leaf->definition_id,
                    .provider_package_root = leaf->package_root.generic_string()};
            }
            if (!tick_prototype) throw std::invalid_argument("output-gain construction produced no primitive");
        }
        tick_prototypes.emplace(key, *tick_prototype);
        return *tick_prototype;
    };
    auto const connections = graph.connections.configured_sample_connections();
    std::vector<ConfiguredSampleConnection> samples(connections.begin(), connections.end());
    for (auto const& control : graph.manual_controls) {
        if (!control.output) { controls.push_back(control); continue; }
        if (!std::isfinite(control.settled_value.value))
            throw std::invalid_argument("output gain must be finite");
        auto const source = *control.output;
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
    // Keep an aggregate port's gain distinct from a gain on one tiled child.
    // Opening a tiled route only when necessary preserves channel positions
    // while making a selected child's gain visible to aggregate consumers.
    auto routed_source = [&](auto&& self, SampleOutputChannelId source, bool random_access,
                             std::optional<NodeBundlePortId> raw_ingress) -> SampleOutputChannelId {
        NodeBundlePortId const id{source.bundle, PortKind::sample, source.port};
        auto const& views = random_access ? result.background_outputs : result.outputs;
        if (id != raw_ingress) {
            auto found = views.find(id);
            if (found == views.end() && random_access) {
                auto live = result.outputs.find(id);
                if (live != result.outputs.end()) return {live->second.node_bundle_handle, live->second.port_index, source.channel};
            } else if (found != views.end()) return {found->second.node_bundle_handle, found->second.port_index, source.channel};
        }
        if (graph.node_bundles.bundle(source.bundle).is_tiled()) {
            SampleOutputChannelId const child{
                graph.node_bundles.tiled_member(source.bundle, source.channel), source.port, 0};
            auto transformed = self(self, child, random_access, raw_ingress);
            if (transformed != child) return transformed;
        }
        return source;
    };
    std::vector<bool> tick_owners;
    graph.node_bundles.for_each_configured_bundle([&](ConfiguredNodeBundleView const& view) {
        tick_owners.push_back(view.operations && view.operations->runtime.tick_block);
    });
    for (auto& connection : samples) for (auto& source : connection.source_channels) {
        bool const random_access = std::ranges::any_of(connection.target_channels, [&](auto target) {
            return !is_sequential(graph.node_bundles.resolve_sample_input({target.bundle, PortKind::sample, target.port}).config)
                || (graph.node_bundles.bundle(target.bundle).is_concrete() && !tick_owners[target.bundle]);
        });
        source.channel = routed_source(routed_source, source.channel, random_access, std::nullopt);
    }
    for (auto const& stage : result.stages) {
        auto const output = graph.node_bundles.resolve_sample_output(stage.source).config;
        ConfiguredSampleConnection ingress{.source_type = output.channel_layout.channel_type,
            .target_type = output.channel_layout.channel_type};
        for (std::size_t channel = 0; channel < channel_count(output.channel_layout); ++channel) {
            auto source = routed_source(routed_source,
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
