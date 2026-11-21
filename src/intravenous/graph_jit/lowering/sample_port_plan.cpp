#include <intravenous/graph_jit/lowering/sample_port_plan.h>

#include <intravenous/graph_jit/background_storage_plan.h>
#include <intravenous/graph_jit/lowering.h>
#include <intravenous/graph_jit/lowering/graph_analysis.h>
#include <intravenous/graph_jit/module_input_resolution_runtime.h>
#include <intravenous/graph_jit/transient_arena_plan.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>

namespace iv::graph_jit::detail {
namespace {
// Builds the additive Tick contribution of one mixed Tick/background sample
// connection. Each selected source channel becomes an explicit mono -> target
// projection, so the resulting full-layout value can be combined with the
// separately materialized background contribution at the callback boundary.
std::expected<SampleConnectionPlan, std::string>
realtime_sample_partition(ConnectionAnalysisPlan const& plan, SampleConnectionPlan const& connection)
{
    if (connection.detach || !connection.routed_contributions.empty()) {
        return std::unexpected(
            "GraphJit mixed Tick/background sample connection cannot contain a detached route");
    }
    SampleConnectionPlan result = connection;
    result.source_channels.clear();
    result.source_channel_timings.clear();
    result.projection_contributions.clear();
    result.routed_contributions.clear();
    result.canonical_source_port.reset();
    result.canonical_source_layout.reset();
    result.detach.reset();
    result.detach_initial_value.reset();
    result.detach_region.reset();
    result.transform = {};
    result.source_type = ChannelTypeId::mono;
    result.requires_conversion = true;
    result.requires_block_materialization = true;
    result.source_history = 0;
    result.source_latency = 0;
    result.read_latency = 0;

    std::vector<SequentialCoverageSource> projection_coverage;
    auto append_source = [&](std::size_t original_source,
                             std::span<std::size_t const> target_channels,
                             ChannelTypeId converted_type,
                             std::span<float const> coefficients)
        -> std::expected<void, std::string> {
        if (original_source >= connection.source_channel_timings.size()
            || original_source >= connection.source_channels.size()
            || coefficients.size() != target_channels.size()) {
            return std::unexpected(
                "GraphJit mixed sample partition has inconsistent channel metadata");
        }
        auto const& timing = connection.source_channel_timings[original_source];
        if (!uses_realtime_storage(timing.delivery)) return {};
        auto const local_source = result.source_channel_timings.size();
        result.source_channels.push_back(connection.source_channels[original_source]);
        result.source_channel_timings.push_back(timing);
        result.source_history = std::max(
            result.source_history, timing.source_history);
        result.source_latency = std::max(
            result.source_latency, timing.source_latency);
        result.projection_contributions.push_back(
            SampleProjectionContributionPlan{
                .source_type = ChannelTypeId::mono,
                .source_channel_indices = {local_source},
                .target_type = converted_type,
                .target_channels = std::vector<std::size_t>(
                    target_channels.begin(), target_channels.end()),
                .transform = SampleChannelTransformPlan{
                    .source_channel_count = 1,
                    .target_channel_count = target_channels.size(),
                    .coefficients = std::vector<float>(
                        coefficients.begin(), coefficients.end()),
                },
                .coverage_sources = projection_coverage,
            });
        return {};
    };

    auto partition_projection = [&](ChannelTypeId source_type,
                                    std::span<std::size_t const> source_indices,
                                    ChannelTypeId target_type,
                                    std::span<std::size_t const> target_channels,
                                    SampleChannelTransformPlan const& transform)
        -> std::expected<void, std::string> {
        projection_coverage.clear();
        for (auto index : source_indices) {
            if (index >= connection.source_channel_timings.size())
                return std::unexpected("mixed sample projection lost a source timing");
            auto coverage = sequential_source_coverage(plan, connection.source_channel_timings[index]);
            if (!coverage) return std::unexpected(std::move(coverage.error()));
            projection_coverage.push_back(*coverage);
        }
        if (std::ranges::none_of(projection_coverage,
                [](auto const& source) { return source.output_change.has_value(); }))
            projection_coverage.clear();
        auto const source_count = source_indices.size();
        auto const target_count = target_channels.size();
        if ((!transform.empty()
                && (transform.source_channel_count != source_count
                    || transform.target_channel_count != target_count
                    || transform.coefficients.size()
                        != source_count * target_count))
            || (transform.empty()
                && (source_count != channel_count(source_type)
                    || target_count != channel_count(target_type)))) {
            return std::unexpected(
                "GraphJit mixed sample partition has an invalid conversion shape");
        }
        for (std::size_t source = 0; source < source_count; ++source) {
            std::vector<float> coefficients(target_count);
            for (std::size_t target = 0; target < target_count; ++target) {
                coefficients[target] = transform.empty()
                    ? channel_conversion_coefficient(
                        source_type, target_type, source, target)
                    : transform.coefficients[target * source_count + source];
            }
            auto appended = append_source(
                source_indices[source],
                target_channels,
                target_type,
                coefficients);
            if (!appended) return appended;
        }
        return {};
    };

    if (connection.projection_contributions.empty()) {
        std::vector<std::size_t> sources(
            connection.source_channel_timings.size());
        std::iota(sources.begin(), sources.end(), 0);
        std::vector<std::size_t> targets(connection.target_channels.size());
        std::iota(targets.begin(), targets.end(), 0);
        auto partitioned = partition_projection(
            connection.source_type,
            sources,
            connection.target_type,
            targets,
            connection.transform);
        if (!partitioned) return std::unexpected(partitioned.error());
    } else {
        for (auto const& projection : connection.projection_contributions) {
            auto partitioned = partition_projection(
                projection.source_type,
                projection.source_channel_indices,
                projection.target_type,
                projection.target_channels,
                projection.transform);
            if (!partitioned) return std::unexpected(partitioned.error());
        }
    }
    if (result.source_channel_timings.empty()) {
        return std::unexpected(
            "GraphJit mixed sample connection has no realtime contribution");
    }
    // Some channels may be supplied entirely by the background side. The
    // realtime operand has the full destination layout and contributes zero
    // to those channels.
    for (std::size_t channel = 0; channel < result.target_channels.size(); ++channel) {
        if (std::ranges::any_of(result.projection_contributions, [&](auto const& projection) {
                return std::ranges::contains(projection.target_channels, channel);
            })) {
            continue;
        }
        result.projection_contributions.push_back({
            .source_channel_indices = {0},
            .target_channels = {channel},
            .transform = {1, 1, {0.0f}},
        });
    }
    return result;
}

} // namespace

// One value of a Sequential continuous input's resolved geometry, set by its
// node's type-owned initializer to the input's neutral or default value.
std::expected<std::vector<std::byte>, std::string>
initialized_sequential_input_value(
    NodeImplementation const* implementation,
    std::size_t port,
    ContinuousValueStorageGeometry const& geometry,
    iv::details::SequentialInputInitialization initialization)
{
    if (!implementation || (!implementation->module_input_config
        && !implementation->host_initialize_sequential_input)) {
        return std::unexpected(
            "GraphJit continuous input has no type-owned initializer");
    }
    std::vector<std::byte> unaligned(
        geometry.value_size_bytes + geometry.element_alignment - 1);
    void* aligned = unaligned.data();
    auto space = unaligned.size();
    if (!std::align(
            geometry.element_alignment,
            geometry.value_size_bytes,
            aligned,
            space)
        || !(implementation->module_input_config
            ? initialize_module_input_value(implementation->module_input_config,
                geometry.value_type, initialization, static_cast<std::byte*>(aligned),
                geometry.value_size_bytes, geometry.elements_per_value)
            : implementation->host_initialize_sequential_input(port,
                geometry.value_type, initialization, static_cast<std::byte*>(aligned),
                geometry.value_size_bytes, geometry.elements_per_value))) {
        return std::unexpected(
            "GraphJit continuous input initializer rejected its resolved geometry");
    }
    std::vector<std::byte> value(geometry.value_size_bytes);
    std::memcpy(value.data(), aligned, geometry.value_size_bytes);
    return value;
}

std::expected<std::vector<SampleConnectionPlan>, std::string>
prepare_sample_storage_connections(
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections)
{
    std::vector<SampleConnectionPlan> sample_connections;
    sample_connections.reserve(connections.sample_connections.size());
    for (auto const& source : connections.sample_connections) {
        if (has_realtime_delivery(source)
            && !is_entirely_realtime_delivery(source)) {
            auto partitioned = realtime_sample_partition(connections, source);
            if (!partitioned) {
                return std::unexpected(std::move(partitioned.error()));
            }
            sample_connections.push_back(std::move(*partitioned));
            continue;
        }
        auto& connection = sample_connections.emplace_back(source);
        std::vector<std::optional<std::vector<std::byte>>*> initial_values;
        if (connection.detach)
            initial_values.push_back(&connection.detach_initial_value);
        for (auto& contribution : connection.routed_contributions) {
            if (contribution.detach)
                initial_values.push_back(&contribution.detach_initial_value);
        }
        if (initial_values.empty()) continue;
        for (auto const initial_value : initial_values) {
            if (*initial_value
                && (*initial_value)->size()
                    != connection.geometry.value_size_bytes) {
                return std::unexpected(
                    "GraphJit detach initial value does not match resolved continuous geometry");
            }
        }
        if (std::ranges::all_of(
                initial_values,
                [](auto const initial_value) { return initial_value->has_value(); }))
            continue;
        auto const target = std::ranges::find_if(
            analysis.primitives, [&](PrimitiveAnalysis const& primitive) {
                return primitive.bundle.node_bundle
                    == connection.target_port.node_bundle_handle;
            });
        if (target == analysis.primitives.end()) {
            return std::unexpected(
                "GraphJit detached continuous input has no concrete target initializer");
        }
        auto neutral = initialized_sequential_input_value(
            target->implementation,
            connection.target_port.port_index,
            connection.geometry,
            iv::details::SequentialInputInitialization::neutral);
        if (!neutral) return std::unexpected(std::move(neutral.error()));
        for (auto const initial_value : initial_values) {
            if (!*initial_value) *initial_value = *neutral;
        }
    }
    return sample_connections;
}

std::expected<SamplePortBindingPlan, std::string> plan_sample_ports(
    ConfiguredGraph const& graph,
    std::size_t block_size,
    GraphAnalysis const& analysis,
    SampleStorageConnections storage_connections,
    ResolvedPortRealization const& realization,
    RealtimeStorageCostModel const& cost_model,
    std::span<std::optional<Sample> const> static_sample_values)
{
    auto const& connections = storage_connections.analysis;
    SamplePortBindingPlan plan;
    if (analysis.empty) return plan;
    plan.primitives.resize(analysis.primitives.size());
    auto primitive_index_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
            if (analysis.primitives[i].bundle.node_bundle == bundle) return i;
        }
        return std::nullopt;
    };
    auto planned_node_for_bundle = [&](NodeBundleHandle bundle)
        -> PlannedGraphNode const* {
        auto const found = std::ranges::find_if(
            connections.nodes,
            [&](PlannedGraphNode const& node) { return node.bundle == bundle; });
        return found == connections.nodes.end() ? nullptr : &*found;
    };

    struct ValidatedSampleSourceBinding {
        std::size_t group_index = 0;
        std::size_t source_primitive = 0;
        std::size_t source_port = 0;
        std::size_t source_history = 0;
        std::size_t source_latency = 0;
    };
    struct ValidatedSampleTargetBinding {
        std::size_t connection_index = 0;
        std::size_t target_primitive = 0;
        std::size_t target_port = 0;
        ChannelLayout target_layout{};
        std::size_t target_history = 0;
        std::size_t read_latency = 0;
    };
    std::vector<ValidatedSampleSourceBinding> validated_sources;
    std::vector<ValidatedSampleTargetBinding> validated_targets;
    validated_sources.reserve(connections.sample_producer_groups.size());
    validated_targets.reserve(connections.sample_connections.size());

    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const* node = planned_node_for_bundle(
            analysis.primitives[i].bundle.node_bundle);
        if (!node) {
            return std::unexpected(
                "GraphJit sample-edge planning lost concrete-node port metadata");
        }
        if (analysis.primitives[i].implementation->tick_block.empty()) {
            // Tock ports are bound by BackgroundNodeCall for each batch. They
            // are not part of the generated realtime root's port-binding ABI.
            continue;
        }
        plan.primitives[i].inputs.resize(node->sample_input_count);
        plan.primitives[i].outputs.resize(node->sample_output_count);
        auto const bundle = analysis.primitives[i].bundle.node_bundle;
        for (std::size_t port = 0; port < node->sample_input_count; ++port) {
            auto const& resolved = realization.input(bundle, port);
            auto geometry = continuous_value_storage_geometry(
                resolved.value_type, resolved.size);
            if (!geometry) {
                return std::unexpected(
                    "GraphJit continuous input has invalid resolved storage geometry");
            }
            plan.primitives[i].inputs[port].geometry = *geometry;
            plan.primitives[i].inputs[port].absolute_pace_exponent =
                resolved.absolute_pace.exponent();
        }
        for (std::size_t port = 0; port < node->sample_output_count; ++port) {
            auto const config = graph.node_bundles.resolve_sample_output(
                {bundle, PortKind::sample, port}).config;
            plan.primitives[i].outputs[port].realtime =
                is_sequential_output(config);
            auto const& resolved = realization.output(bundle, port);
            auto geometry = continuous_value_storage_geometry(
                resolved.value_type, resolved.size);
            if (!geometry) {
                return std::unexpected(
                    "GraphJit continuous output has invalid resolved storage geometry");
            }
            plan.primitives[i].outputs[port].geometry = *geometry;
            plan.primitives[i].outputs[port].absolute_pace_exponent =
                resolved.absolute_pace.exponent();
        }
    }

    for (auto const& group : connections.sample_producer_groups) {
        // Realtime storage serves only Tick -> Sequential channels. A group
        // without one is delivered entirely by background evaluation; a Tick
        // producer of such a group writes a disconnected-output sink below,
        // which persisted capture reads like any other output binding. A mixed
        // group plans realtime storage from its realtime channels alone.
        if (!group.has_realtime_connections) continue;
        if (group.external_boundary) {
            return std::unexpected(
                "GraphJit sample-edge planning has no internal realtime storage plan");
        }
        if (!group.source_port || !group.canonical_source_layout) {
            return std::unexpected(
                "GraphJit sample fanout requires one canonical producer output port");
        }
    }

    auto producer_group_index_for_port = [&](NodeBundlePortId const& port)
        -> std::optional<std::size_t> {
        for (std::size_t i = 0;
             i < connections.sample_producer_groups.size(); ++i) {
            auto const& group = connections.sample_producer_groups[i];
            if (group.source_port && *group.source_port == port) return i;
        }
        return std::nullopt;
    };

    std::vector<SampleConstantProducerRequest> constant_producer_requests;
    std::vector<bool> constant_producer_primitives(
        analysis.primitives.size(), false);
    for (std::size_t primitive_index = 0;
         primitive_index < analysis.primitives.size(); ++primitive_index) {
        auto const bundle =
            analysis.primitives[primitive_index].bundle.node_bundle;
        if (bundle >= static_sample_values.size()
            || !static_sample_values[bundle]
            || bundle >= connections.schedule.callback_elided.size()
            || connections.schedule.callback_elided[bundle] == 0) {
            continue;
        }
        NodeBundlePortId const source_port{
            bundle, PortKind::sample, 0};
        auto const& binding = plan.primitives[primitive_index].outputs.front();
        if (binding.geometry.value_type != ContinuousValueTypeId::sample
            || binding.geometry.elements_per_value != 1
            || binding.geometry.value_size_bytes != sizeof(Sample)) {
            return std::unexpected(
                "GraphJit callback-elided static sample producer has invalid storage geometry");
        }
        auto const group_index = producer_group_index_for_port(source_port);
        if (!group_index
            || !connections.sample_producer_groups[*group_index]
                    .has_realtime_connections
            || connections.sample_producer_groups[*group_index]
                    .has_background_connections) {
            return std::unexpected(
                "GraphJit callback-elided static sample producer lost its realtime-only producer group");
        }
        std::vector<std::byte> value(sizeof(Sample));
        std::memcpy(
            value.data(),
            std::addressof(*static_sample_values[bundle]),
            sizeof(Sample));
        constant_producer_requests.push_back(SampleConstantProducerRequest{
            .producer_group_index = *group_index,
            .value = std::move(value),
        });
        constant_producer_primitives[primitive_index] = true;
    }

    for (std::size_t connection_index = 0;
         connection_index < connections.sample_connections.size();
         ++connection_index) {
        auto const& connection = connections.sample_connections[connection_index];
        // Background evaluation delivers a connection with no Tick ->
        // Sequential channel. A Sequential target then receives its block
        // through its Sequential Tick input slot and is planned below as
        // invocation_supplied; a Random Access target reads its
        // Random Access Tick input view.
        if (!has_realtime_delivery(connection)) continue;
        auto const mixed_delivery =
            !is_entirely_realtime_delivery(connection);
        if (connection.external_boundary) {
            return std::unexpected(
                "GraphJit sample-edge slice does not yet support external sample boundaries");
        }
        if (connection.source_channel_timings.size()
            != connection.source_channels.size()) {
            return std::unexpected(
                "GraphJit sample-edge planning lost source-channel timing metadata");
        }
        if (connection.source_channel_timings.empty()) {
            return std::unexpected(
                "GraphJit sample-edge connection has no source channels");
        }

        auto const target_port = connection.target_port;
        auto const target_primitive = primitive_index_for_bundle(
            target_port.node_bundle_handle);
        if (!target_primitive || target_port.port_kind != PortKind::sample) {
            return std::unexpected(
                "GraphJit sample-edge slice requires an internal concrete sample target");
        }
        auto const target = graph.node_bundles
            .resolve_sample_input(target_port).config;
        if (!is_sequential(target)) {
            return std::unexpected(
                "GraphJit sample-edge slice requires realtime sample port declarations");
        }
        if (target.channel_layout != connection.target_layout
            || connection.target_type != target.channel_layout.channel_type) {
            return std::unexpected(
                "GraphJit sample connection target layout disagrees with its declaration");
        }
        auto const target_channel_total = channel_count(target.channel_layout);
        if (connection.target_channels.size() != target_channel_total) {
            return std::unexpected(
                "GraphJit sample connection requires every target channel");
        }
        for (std::size_t channel = 0; channel < target_channel_total; ++channel) {
            auto const& target_channel = connection.target_channels[channel];
            if (target_channel.bundle != target_port.node_bundle_handle
                || target_channel.port != target_port.port_index
                || target_channel.channel != channel) {
                return std::unexpected(
                    "GraphJit normalized sample composition lost canonical target-port coverage");
            }
        }
        if (target_port.port_index
            >= plan.primitives[*target_primitive].inputs.size()) {
            return std::unexpected(
                "GraphJit sample-edge target index is outside primitive port metadata");
        }

        // Validate every actual producer port independently. A composed input
        // can reference several producer groups even though it realizes one
        // consumer-side storage.
        std::vector<NodeBundlePortId> source_ports_seen;
        for (std::size_t channel_index = 0;
             channel_index < connection.source_channel_timings.size();
             ++channel_index) {
            auto const& timing = connection.source_channel_timings[channel_index];
            if (!uses_realtime_storage(timing.delivery)) continue;
            auto const& source_channel = connection.source_channels[channel_index];
            if (source_channel.bundle != timing.source.bundle
                || source_channel.port != timing.source.port
                || source_channel.channel != timing.source.channel) {
                return std::unexpected(
                    "GraphJit sample-edge source timing disagrees with its channel identity");
            }
            NodeBundlePortId const source_port{
                timing.source.bundle,
                PortKind::sample,
                timing.source.port,
            };
            auto const source_primitive = primitive_index_for_bundle(
                source_port.node_bundle_handle);
            auto const group_index = producer_group_index_for_port(source_port);
            if (!source_primitive || !group_index) {
                return std::unexpected(
                    "GraphJit sample-edge slice requires internal concrete sample producers");
            }
            auto const source = graph.node_bundles
                .resolve_sample_output(source_port).config;
            auto const& group = connections.sample_producer_groups[*group_index];
            if (!is_sequential_output(source)
                || source.channel_layout != timing.source_layout
                || !group.canonical_source_layout
                || *group.canonical_source_layout != source.channel_layout
                || timing.source.channel >= channel_count(source.channel_layout)) {
                return std::unexpected(
                    "GraphJit sample producer channel disagrees with its declared output layout");
            }
            if (source_port.port_index
                >= plan.primitives[*source_primitive].outputs.size()) {
                return std::unexpected(
                    "GraphJit sample-edge source index is outside primitive port metadata");
            }

            if (std::ranges::find(source_ports_seen, source_port)
                == source_ports_seen.end()) {
                source_ports_seen.push_back(source_port);
                validated_sources.push_back(ValidatedSampleSourceBinding{
                    .group_index = *group_index,
                    .source_primitive = *source_primitive,
                    .source_port = source_port.port_index,
                    .source_history = timing.source_history,
                    .source_latency = timing.source_latency,
                });
            }
        }

        std::size_t target_read_latency = 0;
        if (connection.canonical_source_port) {
            if (!connection.canonical_source_layout) {
                return std::unexpected(
                    "GraphJit canonical sample source lost its channel layout");
            }
            auto const source_port = *connection.canonical_source_port;
            auto const source = graph.node_bundles
                .resolve_sample_output(source_port).config;
            if (source.channel_layout != *connection.canonical_source_layout
                || connection.source_type != source.channel_layout.channel_type) {
                return std::unexpected(
                    "GraphJit sample fanout source layout disagrees with its declaration");
            }
            auto const source_channel_total = channel_count(source.channel_layout);
            if (connection.source_channels.size() != source_channel_total) {
                return std::unexpected(
                    "GraphJit whole-port sample connection requires every source channel");
            }
            for (std::size_t channel = 0;
                 channel < source_channel_total; ++channel) {
                auto const& source_channel = connection.source_channels[channel];
                if (source_channel.bundle != source_port.node_bundle_handle
                    || source_channel.port != source_port.port_index
                    || source_channel.channel != channel) {
                    return std::unexpected(
                        "GraphJit canonical sample source channels lost canonical ordering");
                }
            }
            if (connection.requires_conversion) {
                try {
                    (void)ChannelConversionRegistry::plan(
                        source.channel_layout, target.channel_layout);
                } catch (std::exception const& e) {
                    return std::unexpected(
                        "GraphJit sample fanout conversion is unsupported: "
                        + std::string(e.what()));
                }
            } else if (source.channel_layout != target.channel_layout) {
                return std::unexpected(
                    "GraphJit sample connection lost its required layout conversion");
            }
            if (connection.detach) {
                auto const detach_latency =
                    connection.detach->loop_extra_latency;
                if (connection.read_latency
                    > std::numeric_limits<std::size_t>::max()
                        - detach_latency) {
                    return std::unexpected(
                        "GraphJit sample feedback read latency overflows size_t");
                }
                target_read_latency =
                    detach_latency + connection.read_latency;
            } else {
                target_read_latency = connection.read_latency;
            }
        } else {
            // Projection/permutation normalization preserves one semantic
            // source-set -> conversion -> projection contribution per configured
            // connection. The flattened source vector is only producer/timing
            // inventory; contribution metadata is authoritative for channel
            // conversion and final target placement.
            if (connection.canonical_source_layout
                || (connection.projection_contributions.empty()
                    && connection.routed_contributions.empty())) {
                return std::unexpected(
                    "GraphJit sample composition lost normalized projection metadata");
            }
            if (!connection.routed_contributions.empty()) {
                if (!connection.projection_contributions.empty()
                    || connection.detach) {
                    return std::unexpected(
                        "GraphJit routed sample composition has conflicting normalized metadata");
                }
                bool has_detach = false;
                for (auto const& contribution :
                     connection.routed_contributions) {
                    if (contribution.source_coefficients.size()
                            != connection.source_channel_timings.size()
                        || contribution.target_coefficients.size()
                            != target_channel_total) {
                        return std::unexpected(
                            "GraphJit routed sample contribution has inconsistent channel counts");
                    }
                    has_detach = has_detach || contribution.detach.has_value();
                }
                if (!has_detach) {
                    return std::unexpected(
                        "GraphJit routed sample composition contains no detach cut");
                }
                // The storage plan combines ordinary sources and retained mono
                // detach timelines into one timestamp-aligned target value
                // immediately before the target callback.
                target_read_latency = 0;
            } else {
                std::vector<bool> populated_targets(
                    target_channel_total, false);
                for (auto const& contribution :
                     connection.projection_contributions) {
                    auto const source_count = contribution.transform.empty()
                        ? channel_count(contribution.source_type)
                        : contribution.transform.source_channel_count;
                    auto const target_count = contribution.transform.empty()
                        ? channel_count(contribution.target_type)
                        : contribution.transform.target_channel_count;
                    if (contribution.source_channel_indices.size()
                            != source_count
                        || contribution.target_channels.size()
                            != target_count
                        || (!contribution.transform.empty()
                            && contribution.transform.coefficients.size()
                                != source_count * target_count)) {
                        return std::unexpected(
                            "GraphJit sample composition contribution has inconsistent semantic channel counts");
                    }
                    if (contribution.transform.empty()) {
                        try {
                            (void)ChannelConversionRegistry::plan(
                                ChannelLayout{
                                    .channel_type = contribution.source_type,
                                    .sample_layout = SampleStreamLayout::planar,
                                },
                                ChannelLayout{
                                    .channel_type = contribution.target_type,
                                    .sample_layout = SampleStreamLayout::planar,
                                });
                        } catch (std::exception const& e) {
                            return std::unexpected(
                                "GraphJit sample composition conversion is unsupported: "
                                + std::string(e.what()));
                        }
                    }
                    for (auto const source_index :
                         contribution.source_channel_indices) {
                        if (source_index
                            >= connection.source_channel_timings.size()) {
                            return std::unexpected(
                                "GraphJit sample composition contribution lost a source timing");
                        }
                    }
                    for (auto const target_channel :
                         contribution.target_channels) {
                        if (target_channel >= populated_targets.size()) {
                            return std::unexpected(
                                "GraphJit sample composition contribution has an invalid target projection");
                        }
                        populated_targets[target_channel] = true;
                    }
                }
                if (!std::ranges::all_of(
                        populated_targets, [](bool value) { return value; })) {
                        return std::unexpected(
                            "GraphJit sample composition does not populate every target channel");
                }
                    // Feed-forward composition exposes timestamp-aligned target
                // channels and therefore reads at port latency zero; channels may
                // be direct delayed aliases or computed transient results. Detached
                // composition writes each source frame forward by its channel's
                // read latency into a persistent aligned timeline, leaving only
                // the authored loop delay for the InputPort binding.
                target_read_latency = connection.detach
                    ? connection.detach->loop_extra_latency
                    : 0;
            }
        }

        validated_targets.push_back(ValidatedSampleTargetBinding{
            .connection_index = connection_index,
            .target_primitive = *target_primitive,
            .target_port = target_port.port_index,
            .target_layout = connection.target_layout,
            .target_history = connection.target_history,
            .read_latency = mixed_delivery ? 0 : target_read_latency,
        });
    }

    struct DisconnectedSampleInput {
        std::size_t primitive = 0;
        std::size_t port = 0;
        ContinuousInputConfig config{};
    };
    struct DisconnectedSampleOutput {
        std::size_t primitive = 0;
        std::size_t port = 0;
        ContinuousOutputConfig config{};
    };
    // A slot-fed input with history, or any mixed input, owns a retained
    // timeline into which the root writes each delivered block.
    struct TimelineSampleInput {
        std::size_t primitive = 0;
        std::size_t port = 0;
        bool bind = true;
    };
    std::vector<DisconnectedSampleInput> disconnected_inputs;
    std::vector<DisconnectedSampleOutput> disconnected_outputs;
    std::vector<SampleConstantInputRequest> constant_input_requests;
    std::vector<SampleSinkStorageRequest> sink_requests;
    std::vector<TimelineSampleInput> timeline_inputs;
    std::vector<SampleInputTimelineRequest> timeline_requests;

    auto has_authored_connection = [&](std::size_t primitive, std::size_t port) {
        auto const target = NodeBundlePortId{
            analysis.primitives[primitive].bundle.node_bundle,
            PortKind::sample,
            port,
        };
        return std::ranges::any_of(
            connections.sample_connections,
            [&](SampleConnectionPlan const& connection) {
                return connection.target_port == target;
            });
    };
    for (auto const& connection : connections.sample_connections) {
        if (connection.destination_access != PlannedDestinationAccess::sequential) continue;
        auto const primitive = primitive_index_for_bundle(connection.target_port.node_bundle_handle);
        if (!primitive) continue;
        auto& binding = plan.primitives[*primitive].inputs[connection.target_port.port_index];
        auto projections = sequential_connection_coverage(connections, connection);
        if (!projections) return std::unexpected(std::move(projections.error()));
        binding.coverage_projections = std::move(*projections);
        auto realtime = sequential_connection_coverage(connections, connection, true);
        auto background = sequential_connection_coverage(connections, connection, false);
        if (!realtime || !background)
            return std::unexpected(realtime ? std::move(background.error()) : std::move(realtime.error()));
        binding.realtime_coverage_projections = std::move(*realtime);
        binding.background_coverage_projections = std::move(*background);
        binding.resolves_coverage = std::ranges::any_of(binding.coverage_projections,
            [](auto const& projection) {
                return std::ranges::any_of(projection.sources,
                    [](auto const& source) { return source.output_change.has_value(); });
            });
    }
    for (auto const& configured : graph.manual_controls) {
        if (configured.coverage_source) continue;
        if (graph.node_bundles.bundle(configured.input.node_bundle_handle).is_subgraph()) continue;
        auto const primitive = primitive_index_for_bundle(
            configured.input.node_bundle_handle);
        if (!primitive || configured.input.port_kind != PortKind::sample
            || configured.input.port_index
                >= plan.primitives[*primitive].inputs.size()) {
            return std::unexpected(
                "GraphJit manual control does not select a Tick continuous input");
        }
        auto const& analyzed = analysis.primitives[*primitive];
        if (!analyzed.implementation
            || analyzed.implementation->tick_block.empty()) {
            return std::unexpected(
                "GraphJit manual controls require a Tick node input");
        }
        auto const port = configured.input.port_index;
        auto const input_config = graph.node_bundles.resolve_sample_input(
            configured.input).config;
        auto const& binding = plan.primitives[*primitive].inputs[port];
        if (!is_sequential(input_config)) {
            return std::unexpected(
                "GraphJit manual controls require a Sequential continuous input");
        }
        if (binding.geometry.value_type != ContinuousValueTypeId::sample
            || binding.geometry.elements_per_value != 1
            || binding.geometry.value_size_bytes != sizeof(Sample)) {
            return std::unexpected(
                "GraphJit manual controls currently require a scalar Sample input");
        }
        if (plan.primitives[*primitive].inputs[port].manual_control) {
            return std::unexpected(
                "GraphJit manual controls contain two values for one concrete input");
        }
        auto const connected = has_authored_connection(*primitive, port);
        auto const fallback_only = connected
            && !configured.override_connections
            && configured.participation != ManualControlParticipation::participate_with_connections;
        auto const participates = !connected || (fallback_only && binding.resolves_coverage)
            || configured.participation
                == ManualControlParticipation::participate_with_connections;
        auto const control_index = plan.manual_controls.size();
        plan.manual_controls.push_back(ManualControlPlan{
            .configured = configured,
            .primitive_index = *primitive,
            .input_index = port,
            .participates = participates,
            .stages_timeline = participates
                && (connected
                    || configured.dynamic
                    || realization.input(
                        configured.input.node_bundle_handle, port).history != 0),
            .fallback_only = fallback_only,
        });
        plan.primitives[*primitive].inputs[port].manual_control = control_index;
    }

    // Lower module-input contributions before any direct leaf control of
    // the same destination. Several public controls may contribute to one
    // concrete input without pretending they are one semantic control.
    std::vector<ManualControlPlan> boundary_controls;
    for (auto const& route : connections.manual_sample_routes) {
        if (route.control_index >= graph.manual_controls.size())
            return std::unexpected("GraphJit manual route lost its source control");
        auto const primitive = primitive_index_for_bundle(route.target.node_bundle_handle);
        if (!primitive || route.target.port_index >= plan.primitives[*primitive].inputs.size())
            return std::unexpected("GraphJit module manual input requires a Sequential Tick consumer");
        auto const& target = plan.primitives[*primitive].inputs[route.target.port_index];
        if (target.geometry.value_type != ContinuousValueTypeId::sample || target.geometry.elements_per_value != 1)
            return std::unexpected("GraphJit module manual route requires scalar Sample values");
        if (route.delay > std::numeric_limits<std::size_t>::max() - route.read_latency)
            return std::unexpected("GraphJit module manual route delay overflows");
        auto configured = graph.manual_controls[route.control_index];
        configured.input = route.target;
        ManualControlPlan lowered{
            .configured = std::move(configured), .primitive_index = *primitive,
            .input_index = route.target.port_index, .participates = true,
            .boundary_source = true,
            .channel_coefficients = std::vector<float>(channel_count(graph.node_bundles.resolve_sample_input(route.target).config.channel_layout), 0),
            .delay = route.delay + route.read_latency,
        };
        if (route.target_channel >= lowered.channel_coefficients.size())
            return std::unexpected("GraphJit module manual route has an invalid target channel");
        lowered.channel_coefficients[route.target_channel] = route.coefficient;
        lowered.delay_initial_values.resize(lowered.channel_coefficients.size());
        auto const neutral = sequential_input_metadata<Sample>(sequential_input_properties(
            graph.node_bundles.resolve_sample_input(route.target).config)).neutral_value;
        lowered.delay_initial_values[route.target_channel] = Sample{
            route.initial_coefficient * route.initial_value.value_or(neutral).value};
        auto existing = std::ranges::find_if(boundary_controls, [&](auto const& candidate) {
            return candidate.configured.id == lowered.configured.id && candidate.configured.input == lowered.configured.input
                && candidate.delay == lowered.delay;
        });
        if (existing == boundary_controls.end()) boundary_controls.push_back(std::move(lowered));
        else for (std::size_t channel = 0; channel < lowered.channel_coefficients.size(); ++channel) {
            existing->channel_coefficients[channel] += lowered.channel_coefficients[channel];
            existing->delay_initial_values[channel].value += lowered.delay_initial_values[channel].value;
        }
    }
    boundary_controls.insert(boundary_controls.end(), std::make_move_iterator(plan.manual_controls.begin()),
        std::make_move_iterator(plan.manual_controls.end()));
    plan.manual_controls = std::move(boundary_controls);
    for (auto& primitive : plan.primitives) for (auto& binding : primitive.inputs) binding.manual_control.reset();
    for (std::size_t index = 0; index < plan.manual_controls.size(); ++index) {
        auto& control = plan.manual_controls[index];
        auto& binding = plan.primitives[control.primitive_index].inputs[control.input_index];
        control.first_for_input = !binding.manual_control;
        if (control.participates && !binding.manual_control) binding.manual_control = index;
    }
    std::vector<std::vector<std::vector<std::size_t>>> controls_for_input(plan.primitives.size());
    for (std::size_t primitive = 0; primitive < plan.primitives.size(); ++primitive)
        controls_for_input[primitive].resize(plan.primitives[primitive].inputs.size());
    for (std::size_t index = 0; index < plan.manual_controls.size(); ++index) {
        auto const& control = plan.manual_controls[index];
        controls_for_input[control.primitive_index][control.input_index].push_back(index);
    }
    auto boundary_constant = [&](std::size_t primitive, std::size_t port) -> std::optional<Sample> {
        std::vector<float> channels;
        bool found = false;
        for (auto const index : controls_for_input[primitive][port]) {
            auto const& control = plan.manual_controls[index];
            if (!control.participates) continue;
            if (!control.boundary_source || control.configured.dynamic || control.delay != 0) return std::nullopt;
            if (channels.empty()) channels.resize(control.channel_coefficients.size());
            for (std::size_t channel = 0; channel < channels.size(); ++channel)
                channels[channel] += control.channel_coefficients[channel] * control.configured.settled_value.value;
            found = true;
        }
        if (!found || channels.empty() || !std::ranges::all_of(channels, [&](float value) { return value == channels.front(); }))
            return std::nullopt;
        return Sample{channels.front()};
    };
    for (auto& control : plan.manual_controls) {
        if (!control.participates) continue;
        bool const boundary = std::ranges::any_of(controls_for_input[control.primitive_index][control.input_index],
            [&](auto index) { return plan.manual_controls[index].boundary_source; });
        if (boundary) control.stages_timeline = has_authored_connection(control.primitive_index, control.input_index)
            || realization.input(control.configured.input.node_bundle_handle, control.input_index).history != 0
            || !boundary_constant(control.primitive_index, control.input_index).has_value();
    }

    auto initialized_input_value = [&](
        std::size_t primitive_index,
        std::size_t port,
        iv::details::SequentialInputInitialization initialization)
        -> std::expected<std::vector<std::byte>, std::string> {
        return initialized_sequential_input_value(
            analysis.primitives[primitive_index].implementation,
            port,
            plan.primitives[primitive_index].inputs[port].geometry,
            initialization);
    };
    auto request_input_timeline = [&](
        std::size_t primitive_index,
        std::size_t port,
        std::size_t execution_position,
        bool bind = true)
        -> std::expected<void, std::string> {
        auto existing = std::ranges::find_if(
            timeline_inputs,
            [&](TimelineSampleInput const& input) {
                return input.primitive == primitive_index
                    && input.port == port;
            });
        if (existing != timeline_inputs.end()) {
            existing->bind = existing->bind || bind;
            return {};
        }
        auto const& binding = plan.primitives[primitive_index].inputs[port];
        auto neutral = initialized_input_value(
            primitive_index,
            port,
            iv::details::SequentialInputInitialization::neutral);
        if (!neutral) return std::unexpected(std::move(neutral.error()));
        auto const bundle =
            analysis.primitives[primitive_index].bundle.node_bundle;
        auto migration_identity = input_timeline_migration_identity(
            connections.realtime_port_states,
            NodeBundlePortId{bundle, PortKind::sample, port},
            "graphjit.sample.input_timeline");
        if (!migration_identity) {
            return std::unexpected(std::move(migration_identity.error()));
        }
        timeline_inputs.push_back({
            .primitive = primitive_index,
            .port = port,
            .bind = bind,
        });
        timeline_requests.push_back(SampleInputTimelineRequest{
            .channel_layout = binding.channel_layout,
            .geometry = binding.geometry,
            .absolute_pace_exponent = binding.absolute_pace_exponent,
            .history = binding.history,
            .neutral_value = std::move(*neutral),
            .execution_position = execution_position,
            .migration_identity = std::move(*migration_identity),
        });
        return {};
    };

    auto execution_position_for_bundle = [&](NodeBundleHandle bundle)
        -> std::expected<std::size_t, std::string> {
        if (bundle >= connections.schedule.bundle_execution_position.size()
            || !connections.schedule.bundle_execution_position[bundle]) {
            return std::unexpected(
                "GraphJit disconnected sample port has no execution position");
        }
        return *connections.schedule.bundle_execution_position[bundle];
    };

    for (std::size_t primitive_index = 0;
         primitive_index < analysis.primitives.size(); ++primitive_index) {
        // Background callbacks receive their sample ports from the supplied
        // BackgroundNodeCall bindings. They have no realtime execution
        // position and must not acquire disconnected Tick constants or sinks.
        if (analysis.primitives[primitive_index].implementation->tick_block.empty()) {
            continue;
        }
        auto const bundle = analysis.primitives[primitive_index].bundle.node_bundle;
        // A callback-elided producer has no execution position. It has no
        // inputs, and its only output has realtime consumers.
        if (bundle < connections.schedule.callback_elided.size()
            && connections.schedule.callback_elided[bundle] != 0) {
            continue;
        }
        auto execution_position = execution_position_for_bundle(bundle);
        if (!execution_position) {
            return std::unexpected(std::move(execution_position.error()));
        }

        // An input without a realtime connection is either supplied by
        // background evaluation through its TickInvocationCall slot, or it is
        // disconnected and binds its declared-default constant.
        for (std::size_t port = 0;
             port < plan.primitives[primitive_index].inputs.size(); ++port) {
            auto const connected = std::ranges::any_of(
                validated_targets,
                [&](ValidatedSampleTargetBinding const& target) {
                    return target.target_primitive == primitive_index
                        && target.target_port == port;
                });
            if (connected) continue;

            auto const config = graph.node_bundles.resolve_sample_input(
                NodeBundlePortId{bundle, PortKind::sample, port}).config;
            auto invocation_supplied = external_tick_sequential_slot(
                connections.background, bundle, PortKind::sample, port);
            if (!invocation_supplied) {
                return std::unexpected(
                    std::move(invocation_supplied.error()));
            }
            if (*invocation_supplied) {
                auto& binding = plan.primitives[primitive_index].inputs[port];
                binding.invocation_supplied = true;
                binding.channel_layout = config.channel_layout;
                binding.history = realization.input(bundle, port).history;
                binding.read_latency = 0;
                // Without history the callback reads the slot's current
                // window directly.
                if (binding.history != 0) {
                    auto requested = request_input_timeline(
                        primitive_index, port, *execution_position);
                    if (!requested) {
                        return std::unexpected(std::move(requested.error()));
                    }
                }
                continue;
            }
            // Tick-time Random Access reads only the callback-pinned view in
            // its Random Access Tick input slot, whether the input is
            // connected or disconnected (empty coverage).
            if (!is_sequential(config)) {
                auto& binding = plan.primitives[primitive_index].inputs[port];
                binding.random_access = true;
                binding.channel_layout = config.channel_layout;
                continue;
            }
            auto const manual_control =
                plan.primitives[primitive_index].inputs[port].manual_control;
            if (manual_control && (plan.manual_controls[*manual_control].configured.dynamic
                || (plan.manual_controls[*manual_control].boundary_source && !boundary_constant(primitive_index, port)))) {
                auto& binding = plan.primitives[primitive_index].inputs[port];
                binding.channel_layout = config.channel_layout;
                binding.history = realization.input(bundle, port).history;
                binding.read_latency = 0;
                auto requested = request_input_timeline(
                    primitive_index, port, *execution_position);
                if (!requested) {
                    return std::unexpected(std::move(requested.error()));
                }
                continue;
            }
            auto& binding = plan.primitives[primitive_index].inputs[port];
            binding.channel_layout = config.channel_layout;
            binding.history = realization.input(bundle, port).history;
            binding.read_latency = 0;
            disconnected_inputs.push_back(DisconnectedSampleInput{
                .primitive = primitive_index,
                .port = port,
                .config = config,
            });
            auto const& input_plan =
                plan.primitives[primitive_index].inputs[port];
            auto default_value = initialized_input_value(
                primitive_index,
                port,
                iv::details::SequentialInputInitialization::disconnected);
            if (!default_value) {
                return std::unexpected(std::move(default_value.error()));
            }
            if (manual_control) {
                auto const value = boundary_constant(primitive_index, port).value_or(
                    plan.manual_controls[*manual_control].configured.settled_value);
                std::memcpy(
                    default_value->data(), &value, sizeof(value));
            }
            constant_input_requests.push_back(SampleConstantInputRequest{
                .channel_layout = config.channel_layout,
                .geometry = input_plan.geometry,
                .absolute_pace_exponent = input_plan.absolute_pace_exponent,
                .default_value = std::move(*default_value),
                .execution_position = *execution_position,
            });
            if (manual_control) {
                // Constant code still declares the same input timeline as its
                // dynamic replacement and binds the immutable constant. When
                // history is retained, lowering also keeps this shadow
                // timeline current so dynamic code can reuse it immediately.
                auto requested = request_input_timeline(
                    primitive_index, port, *execution_position, false);
                if (!requested) {
                    return std::unexpected(std::move(requested.error()));
                }
            }
        }

        for (std::size_t port = 0;
             port < plan.primitives[primitive_index].outputs.size(); ++port) {
            auto const connected = std::ranges::any_of(
                validated_sources,
                [&](ValidatedSampleSourceBinding const& source) {
                    return source.source_primitive == primitive_index
                        && source.source_port == port;
                });
            if (connected) continue;

            auto const config = graph.node_bundles.resolve_sample_output(
                NodeBundlePortId{bundle, PortKind::sample, port}).config;
            if (!is_sequential_output(config)) continue;
            disconnected_outputs.push_back(DisconnectedSampleOutput{
                .primitive = primitive_index,
                .port = port,
                .config = config,
            });
            auto const& resolved_output = realization.output(bundle, port);
            sink_requests.push_back(SampleSinkStorageRequest{
                .channel_layout = config.channel_layout,
                .geometry = plan.primitives[primitive_index].outputs[port].geometry,
                .absolute_pace_exponent = plan.primitives[primitive_index]
                    .outputs[port].absolute_pace_exponent,
                .history = resolved_output.history,
                .latency = resolved_output.latency,
                .execution_position = *execution_position,
                .migration_identity =
                    "graphjit.sample.disconnected_output:"
                    + raw_region_node_identity_text(connections, bundle) + "."
                    + std::to_string(port),
            });
        }
    }

    // A mixed input combines its realtime contribution with the slot's
    // current block into its own timeline.
    for (auto const& target : validated_targets) {
        if (is_entirely_realtime_delivery(
                connections.sample_connections[target.connection_index])) {
            continue;
        }
        auto& binding =
            plan.primitives[target.target_primitive].inputs[target.target_port];
        binding.channel_layout = target.target_layout;
        binding.history = target.target_history;
        auto const position = execution_position_for_bundle(
            analysis.primitives[target.target_primitive].bundle.node_bundle);
        if (!position) return std::unexpected(std::move(position.error()));
        auto requested = request_input_timeline(
            target.target_primitive, target.target_port, *position);
        if (!requested) return std::unexpected(std::move(requested.error()));
    }

    // A participating manual control on a connected input needs a private
    // target timeline even when the ordinary connection would otherwise be a
    // direct producer alias. Slot-fed inputs use this same timeline after the
    // existing invocation staging has written their ordinary contribution.
    for (auto const& control : plan.manual_controls) {
        if (!control.participates
            || (!control.boundary_source && !has_authored_connection(
                control.primitive_index, control.input_index))) {
            continue;
        }
        auto& binding = plan.primitives[control.primitive_index]
            .inputs[control.input_index];
        auto const config = graph.node_bundles.resolve_sample_input(
            control.configured.input).config;
        binding.channel_layout = config.channel_layout;
        binding.history = realization.input(
            control.configured.input.node_bundle_handle,
            control.input_index).history;
        auto const position = execution_position_for_bundle(
            control.configured.input.node_bundle_handle);
        if (!position) return std::unexpected(std::move(position.error()));
        bool const bind = has_authored_connection(control.primitive_index, control.input_index)
            || !boundary_constant(control.primitive_index, control.input_index).has_value();
        auto requested = request_input_timeline(
            control.primitive_index, control.input_index, *position, bind);
        if (!requested) return std::unexpected(std::move(requested.error()));
    }

    // A Sequential input with sparse producers needs target-owned storage.
    // Keep the original connection binding as an operand: writing a fallback
    // into a shared producer alias would change its other consumers.
    for (auto const& connection : connections.sample_connections) {
        if (connection.destination_access != PlannedDestinationAccess::sequential) continue;
        auto const primitive = primitive_index_for_bundle(connection.target_port.node_bundle_handle);
        if (!primitive) continue;
        auto& binding = plan.primitives[*primitive].inputs[connection.target_port.port_index];
        if (!binding.resolves_coverage) continue;
        // The timeline request records the binding's layout and history. A
        // realtime-only connection otherwise receives them only after this
        // planning step, so take them from the connection now.
        binding.channel_layout = connection.target_layout;
        binding.history = connection.target_history;
        auto neutral = initialized_input_value(*primitive, connection.target_port.port_index,
            iv::details::SequentialInputInitialization::neutral);
        if (!neutral) return std::unexpected(std::move(neutral.error()));
        binding.neutral_value = std::move(*neutral);
        binding.fallback_value = binding.neutral_value;
        auto position = execution_position_for_bundle(connection.target_port.node_bundle_handle);
        if (!position) return std::unexpected(std::move(position.error()));
        auto requested = request_input_timeline(*primitive,
            connection.target_port.port_index, *position);
        if (!requested) return std::unexpected(std::move(requested.error()));
    }

    // A module-input resolution stage with a disconnected input and a settled
    // (or absent) knob outputs one constant. Its identity-mapped realtime
    // consumers bind that constant immutably, as a disconnected leaf input
    // with a settled knob does. The connection keeps its planned storage and
    // the stage keeps running, so its shared output stays current for
    // consumers that retain history. A dynamic variant aliases the same
    // storage, so activation remains a code-only replacement.
    std::vector<bool> settled_stage_connections(connections.sample_connections.size(), false);
    for (auto const& target : validated_targets) {
        auto const& connection = connections.sample_connections[target.connection_index];
        if (connection.destination_access != PlannedDestinationAccess::sequential
            || !is_entirely_realtime_delivery(connection)
            || !connection.projection_contributions.empty()
            || !connection.routed_contributions.empty()
            || !connection.transform.empty()
            || connection.detach
            || connection.source_type != connection.target_type
            || connection.source_channel_timings.empty()) continue;
        auto const stage_bundle = connection.source_channel_timings.front().source.bundle;
        bool one_stage_identity = true;
        for (std::size_t channel = 0; channel < connection.source_channel_timings.size(); ++channel) {
            auto const& timing = connection.source_channel_timings[channel];
            one_stage_identity = one_stage_identity && timing.source.bundle == stage_bundle
                && timing.source.channel == channel && !timing.detach;
        }
        if (!one_stage_identity) continue;
        auto const stage = primitive_index_for_bundle(stage_bundle);
        if (!stage || !analysis.primitives[*stage].implementation->resolved_module_input
            || has_authored_connection(*stage, 0)) continue;
        auto const& stage_input = plan.primitives[*stage].inputs[0];
        if (stage_input.geometry != plan.primitives[target.target_primitive]
                .inputs[target.target_port].geometry) continue;
        auto value = initialized_input_value(*stage, 0,
            iv::details::SequentialInputInitialization::disconnected);
        if (!value) return std::unexpected(std::move(value.error()));
        if (stage_input.manual_control) {
            auto const& control = plan.manual_controls[*stage_input.manual_control];
            if (control.configured.dynamic) continue;
            if (control.participates) {
                if (value->size() != sizeof(Sample)) continue;
                std::memcpy(value->data(), &control.configured.settled_value, sizeof(Sample));
            }
        }
        auto const position = execution_position_for_bundle(
            analysis.primitives[target.target_primitive].bundle.node_bundle);
        if (!position) return std::unexpected(std::move(position.error()));
        auto const& config = graph.node_bundles.resolve_sample_input(connection.target_port).config;
        disconnected_inputs.push_back(DisconnectedSampleInput{
            .primitive = target.target_primitive,
            .port = target.target_port,
            .config = config,
        });
        constant_input_requests.push_back(SampleConstantInputRequest{
            .channel_layout = config.channel_layout,
            .geometry = stage_input.geometry,
            .absolute_pace_exponent = plan.primitives[target.target_primitive]
                .inputs[target.target_port].absolute_pace_exponent,
            .default_value = std::move(*value),
            .execution_position = *position,
        });
        settled_stage_connections[target.connection_index] = true;
    }

    auto storage = build_sample_storage_plan(
        storage_connections,
        block_size,
        sink_requests,
        constant_input_requests,
        cost_model,
        timeline_requests,
        constant_producer_requests);
    if (!storage) return std::unexpected(std::move(storage.error()));
    plan.storage = std::move(*storage);

    for (auto const& source : validated_sources) {
        if (source.group_index >= plan.storage.producer_groups.size()
            || !plan.storage.producer_groups[source.group_index]) {
            return std::unexpected(
                "GraphJit sample source lost its producer storage");
        }
        auto const output =
            plan.storage.producer_groups[source.group_index]
                ->canonical_representation;
        if (output == no_sample_representation
            || output >= plan.storage.representations.size()) {
            return std::unexpected(
                "GraphJit sample source references an invalid storage");
        }
        auto& source_binding =
            plan.primitives[source.source_primitive].outputs[source.source_port];
        if (source_binding.storage
            && *source_binding.storage != output) {
            return std::unexpected(
                "GraphJit sample output fanout resolved to conflicting canonical ports");
        }
        if (source_binding.storage
            && (source_binding.history != source.source_history
                || source_binding.latency != source.source_latency)) {
            return std::unexpected(
                "GraphJit sample output fanout disagrees on authored output timing");
        }
        source_binding.storage = output;
        source_binding.history = source.source_history;
        source_binding.latency = source.source_latency;
    }

    for (std::size_t primitive_index = 0;
         primitive_index < constant_producer_primitives.size(); ++primitive_index) {
        if (!constant_producer_primitives[primitive_index]) continue;
        auto& primitive = plan.primitives[primitive_index];
        if (primitive.outputs.size() != 1 || !primitive.outputs.front().storage) {
            return std::unexpected(
                "GraphJit static sample producer lost its realtime output storage");
        }
        auto const representation = *primitive.outputs.front().storage;
        if (representation >= plan.storage.representations.size()
            || !plan.storage.representations[representation].constant_value) {
            return std::unexpected(
                "GraphJit static sample producer did not resolve to immutable storage");
        }
        primitive.tick_callback_elided = true;
    }

    for (auto const& target : validated_targets) {
        // Bound below as an immutable constant, with the disconnected inputs.
        if (settled_stage_connections[target.connection_index]) continue;
        if (target.connection_index
                >= plan.storage.connection_representations.size()
            || target.connection_index
                >= plan.storage.connection_channel_bindings.size()) {
            return std::unexpected(
                "GraphJit sample target lost its connection storage");
        }

        auto& target_binding =
            plan.primitives[target.target_primitive].inputs[target.target_port];
        if (!target_binding.channels.empty()) {
            return std::unexpected(
                "GraphJit sample input has more than one realized connection");
        }
        target_binding.channel_layout = target.target_layout;
        target_binding.history = target.target_history;
        target_binding.read_latency = target.read_latency;
        if (!is_entirely_realtime_delivery(
                connections.sample_connections[target.connection_index])) {
            target_binding.invocation_supplied = true;
            target_binding.combines_invocation_supplied = true;
        }

        auto const target_channel_count = channel_count(target.target_layout);
        if (plan.storage.connection_channel_bindings[target.connection_index]) {
            auto const& channel_bindings =
                *plan.storage.connection_channel_bindings[target.connection_index];
            if (channel_bindings.size() != target_channel_count) {
                return std::unexpected(
                    "GraphJit sample target storage channel count disagrees with its target layout");
            }
            target_binding.channels.reserve(channel_bindings.size());
            for (auto const& channel : channel_bindings) {
                if (channel.representation == no_sample_representation
                    || channel.representation >= plan.storage.representations.size()) {
                    return std::unexpected(
                        "GraphJit sample target channel references an invalid storage");
                }
                auto const& storage =
                    plan.storage.representations[channel.representation];
                if (channel.representation_channel
                        >= channel_count(storage.channel_layout)
                    || channel.frame_delay >= storage.frame_capacity) {
                    return std::unexpected(
                        "GraphJit sample target channel exceeds its storage");
                }
                target_binding.channels.push_back(
                    PrimitiveSampleInputChannelBindingPlan{
                        .storage = channel.representation,
                        .representation_channel = channel.representation_channel,
                        .frame_delay = channel.frame_delay,
                    });
            }
            continue;
        }

        if (!plan.storage.connection_representations[target.connection_index]) {
            return std::unexpected(
                "GraphJit sample target lost its connection storage");
        }
        auto const input_representation =
            *plan.storage.connection_representations[target.connection_index];
        if (input_representation >= plan.storage.representations.size()) {
            return std::unexpected(
                "GraphJit sample target references an invalid storage");
        }
        auto const& storage =
            plan.storage.representations[input_representation];
        if (channel_count(storage.channel_layout) != target_channel_count) {
            return std::unexpected(
                "GraphJit sample target storage channel count disagrees with its target layout");
        }
        target_binding.channels.reserve(target_channel_count);
        for (std::size_t channel = 0; channel < target_channel_count; ++channel) {
            target_binding.channels.push_back(
                PrimitiveSampleInputChannelBindingPlan{
                    .storage = input_representation,
                    .representation_channel = channel,
                    .frame_delay = 0,
                });
        }
    }

    if (plan.storage.constant_input_representations.size()
        != disconnected_inputs.size()) {
        return std::unexpected(
            "GraphJit disconnected sample input planning lost a constant buffer");
    }
    for (std::size_t i = 0; i < disconnected_inputs.size(); ++i) {
        auto const& disconnected = disconnected_inputs[i];
        auto const representation_index =
            plan.storage.constant_input_representations[i];
        if (representation_index >= plan.storage.representations.size()) {
            return std::unexpected(
                "GraphJit disconnected sample input references an invalid constant buffer");
        }
        auto const& representation = plan.storage.representations[representation_index];
        if (!representation.constant_value
            || representation.channel_layout != disconnected.config.channel_layout) {
            return std::unexpected(
                "GraphJit disconnected sample input lost its declared default buffer");
        }

        auto& binding = plan.primitives[disconnected.primitive]
            .inputs[disconnected.port];
        binding.channel_layout = disconnected.config.channel_layout;
        binding.history = realization.input(
            analysis.primitives[disconnected.primitive].bundle.node_bundle,
            disconnected.port).history;
        binding.read_latency = 0;
        auto const channels = channel_count(disconnected.config.channel_layout);
        binding.channels.reserve(channels);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            binding.channels.push_back(PrimitiveSampleInputChannelBindingPlan{
                .storage = representation_index,
                .representation_channel = channel,
                .frame_delay = 0,
            });
        }
    }

    if (plan.storage.sink_representations.size()
        != disconnected_outputs.size()) {
        return std::unexpected(
            "GraphJit disconnected sample output planning lost a writable buffer");
    }
    for (std::size_t i = 0; i < disconnected_outputs.size(); ++i) {
        auto const& disconnected = disconnected_outputs[i];
        auto const storage = plan.storage.sink_representations[i];
        if (storage >= plan.storage.representations.size()) {
            return std::unexpected(
                "GraphJit disconnected sample output references an invalid buffer");
        }
        auto& binding = plan.primitives[disconnected.primitive]
            .outputs[disconnected.port];
        IV_ASSERT(binding.realtime,
            "Tick sample storage disagrees with its port declaration");
        binding.storage = storage;
        auto const& resolved_output = realization.output(
            analysis.primitives[disconnected.primitive].bundle.node_bundle,
            disconnected.port);
        binding.history = resolved_output.history;
        binding.latency = resolved_output.latency;
    }

    if (plan.storage.input_timeline_representations.size()
        != timeline_inputs.size()) {
        return std::unexpected(
            "GraphJit sample input timeline planning lost a timeline");
    }
    for (std::size_t i = 0; i < timeline_inputs.size(); ++i) {
        auto const& timeline_input = timeline_inputs[i];
        auto const representation_index =
            plan.storage.input_timeline_representations[i];
        if (representation_index >= plan.storage.representations.size()
            || plan.storage.representations[representation_index]
                    .channel_layout
                != plan.primitives[timeline_input.primitive]
                    .inputs[timeline_input.port].channel_layout) {
            return std::unexpected(
                "GraphJit sample input timeline references an invalid buffer");
        }
        auto& binding = plan.primitives[timeline_input.primitive]
            .inputs[timeline_input.port];
        auto const channels = channel_count(binding.channel_layout);
        std::vector<PrimitiveSampleInputChannelBindingPlan> timeline_channels;
        timeline_channels.reserve(channels);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            timeline_channels.push_back(PrimitiveSampleInputChannelBindingPlan{
                .storage = representation_index,
                .representation_channel = channel,
                .frame_delay = 0,
            });
        }
        if (binding.manual_control) {
            binding.manual_timeline_channels = timeline_channels;
        }
        if (!timeline_input.bind) continue;
        // The callback reads only the timeline; a mixed input's connection
        // storage becomes the realtime operand of its per-block combination.
        if (binding.combines_invocation_supplied) {
            binding.realtime_operand_channels = std::move(binding.channels);
        } else if (binding.manual_control && !binding.invocation_supplied) {
            bool const boundary_only = plan.manual_controls[*binding.manual_control].boundary_source
                && !has_authored_connection(timeline_input.primitive, timeline_input.port);
            if (!boundary_only) {
                binding.manual_operand_channels = std::move(binding.channels);
                if (binding.resolves_coverage) {
                    for (auto& operand : binding.manual_operand_channels)
                        operand.frame_delay += binding.read_latency;
                }
            }
        }
        if (binding.resolves_coverage && !binding.invocation_supplied
            && binding.manual_operand_channels.empty()) {
            binding.coverage_operand_channels = std::move(binding.channels);
            for (auto& operand : binding.coverage_operand_channels)
                operand.frame_delay += binding.read_latency;
        }
        binding.channels.clear();
        binding.read_latency = 0;
        binding.channels = std::move(timeline_channels);
    }

    for (auto const& primitive : plan.primitives) {
        if (!std::ranges::all_of(
                primitive.inputs,
                [](auto const& binding) {
                    auto const channels = channel_count(binding.channel_layout);
                    auto const bound = !binding.channels.empty()
                        && binding.channels.size() == channels;
                    if (binding.combines_invocation_supplied) {
                        return binding.invocation_supplied && bound
                            && binding.realtime_operand_channels.size()
                                == channels;
                    }
                    if (binding.manual_control) {
                        return bound
                            && (binding.invocation_supplied
                                || binding.manual_operand_channels.empty()
                                || binding.manual_operand_channels.size()
                                    == channels);
                    }
                    if (binding.random_access) {
                        return binding.channels.empty() && channels != 0;
                    }
                    // A slot-fed input without history reads the slot
                    // directly. With history, or with sparse sources that
                    // its timeline resolves, it reads that timeline.
                    if (binding.invocation_supplied) {
                        return channels != 0
                            && binding.realtime_operand_channels.empty()
                            && (binding.history == 0 && !binding.resolves_coverage
                                    ? binding.channels.empty()
                                    : bound);
                    }
                    return bound;
                })
            || !std::ranges::all_of(
                primitive.outputs,
                [](auto const& binding) {
                    return !binding.realtime
                        || binding.storage.has_value();
                })) {
            return std::unexpected(
                "GraphJit sample port planning left an unresolved primitive port");
        }
    }

    auto align_up = [](std::size_t value, std::size_t alignment)
        -> std::optional<std::size_t> {
        if (alignment == 0 || !std::has_single_bit(alignment)) {
            return std::nullopt;
        }
        auto const mask = alignment - 1;
        if (value > std::numeric_limits<std::size_t>::max() - mask) {
            return std::nullopt;
        }
        return (value + mask) & ~mask;
    };

    std::vector<std::optional<std::size_t>> bundle_to_primitive(
        connections.schedule.bundle_execution_position.size());
    std::vector<ConnectionLiveIntervalPlan> primitive_scratch_lifetimes(
        plan.primitives.size());
    for (std::size_t primitive = 0; primitive < analysis.primitives.size(); ++primitive) {
        auto const bundle = analysis.primitives[primitive].bundle.node_bundle;
        if (bundle >= bundle_to_primitive.size()) continue;
        bundle_to_primitive[bundle] = primitive;
        if (connections.schedule.bundle_execution_position[bundle]) {
            auto const position =
                *connections.schedule.bundle_execution_position[bundle];
            primitive_scratch_lifetimes[primitive] = {
                .begin = position,
                .end = position,
            };
        }
    }
    auto widen_batch_lifetimes = [&](std::span<NodeBundleHandle const> bundles)
        -> std::expected<void, std::string> {
        if (bundles.empty()) return {};
        auto begin = std::numeric_limits<std::size_t>::max();
        std::size_t end = 0;
        for (auto const bundle : bundles) {
            if (bundle >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[bundle]) {
                return std::unexpected(
                    "GraphJit continuous input scratch batch has no execution position");
            }
            auto const position =
                *connections.schedule.bundle_execution_position[bundle];
            begin = std::min(begin, position);
            end = std::max(end, position);
        }
        for (auto const bundle : bundles) {
            if (bundle >= bundle_to_primitive.size()
                || !bundle_to_primitive[bundle]) {
                return std::unexpected(
                    "GraphJit continuous input scratch batch has no primitive");
            }
            primitive_scratch_lifetimes[*bundle_to_primitive[bundle]] = {
                .begin = begin,
                .end = end,
            };
        }
        return {};
    };
    for (auto const& group : connections.schedule.ready_groups) {
        if (group.size() < 2) continue;
        auto const& first = connections.schedule.regions[group.front()];
        if (first.cyclic) {
            for (std::size_t phase = 0; phase < first.execution_order.size(); ++phase) {
                std::vector<NodeBundleHandle> bundles;
                bundles.reserve(group.size());
                for (auto const region_index : group) {
                    if (region_index >= connections.schedule.regions.size()
                        || connections.schedule.regions[region_index]
                               .execution_order.size()
                            != first.execution_order.size()) {
                        return std::unexpected(
                            "GraphJit continuous input scratch batch has incompatible cyclic regions");
                    }
                    bundles.push_back(connections.schedule.regions[region_index]
                        .execution_order[phase]);
                }
                if (auto widened = widen_batch_lifetimes(bundles); !widened) {
                    return std::unexpected(std::move(widened.error()));
                }
            }
            continue;
        }
        std::vector<NodeBundleHandle> bundles;
        bundles.reserve(group.size());
        for (auto const region_index : group) {
            if (region_index >= connections.schedule.regions.size()
                || connections.schedule.regions[region_index]
                       .execution_order.size() != 1) {
                return std::unexpected(
                    "GraphJit continuous input scratch batch has an invalid acyclic region");
            }
            bundles.push_back(connections.schedule.regions[region_index]
                .execution_order.front());
        }
        if (auto widened = widen_batch_lifetimes(bundles); !widened) {
            return std::unexpected(std::move(widened.error()));
        }
    }

    std::vector<std::size_t> primitive_scratch_sizes(plan.primitives.size());
    std::vector<std::size_t> primitive_scratch_alignments(
        plan.primitives.size(), std::size_t{1});
    for (std::size_t primitive_index = 0;
         primitive_index < plan.primitives.size(); ++primitive_index) {
        auto& primitive = plan.primitives[primitive_index];
        auto const& analyzed = analysis.primitives[primitive_index];
        auto const* node = planned_node_for_bundle(
            analyzed.bundle.node_bundle);
        IV_ASSERT(node != nullptr && analyzed.implementation != nullptr,
            "continuous input scratch planning lost its primitive");
        std::size_t primitive_scratch_size = 0;
        for (auto& input_binding : primitive.inputs) {
            // A Random Access input has no Sequential window to expose.
            if (input_binding.random_access) continue;
            // A native block callback can request block() even with no
            // history. Pointwise tick() exposes only block_extended(), whose
            // zero-history, one-value view is directly ring-addressable.
            auto const requires_contiguous_workspace =
                analyzed.implementation->block_input_access
                || (analyzed.implementation->pointwise_tick
                    && input_binding.history != 0);
            // The node limit bounds one invocation; it is not the size of an
            // invocation. Most nodes retain MAX_BLOCK_SIZE as their authored
            // limit, while the realtime root can expose at most one kernel
            // block here (and may split that block further for an SCC).
            auto const maximum_invocation_extent = std::min(
                block_size,
                node->maximum_block_size);
            auto const current_values =
                FractionalGlobalIndexRange::maximum_projection_count(
                    maximum_invocation_extent,
                    input_binding.absolute_pace_exponent);
            if (!current_values
                || *current_values
                    > std::numeric_limits<std::size_t>::max()
                        - input_binding.history) {
                return std::unexpected(
                    "GraphJit continuous input scratch value count overflows size_t");
            }
            auto const value_capacity =
                input_binding.history + *current_values;
            input_binding.contiguous_value_capacity = value_capacity;
            input_binding.contiguous_storage_capacity = value_capacity;
            if (!requires_contiguous_workspace) continue;

            auto const channels = channel_count(input_binding.channel_layout);
            if (channels == 0
                || input_binding.contiguous_storage_capacity
                    > std::numeric_limits<std::size_t>::max()
                        / input_binding.geometry.value_size_bytes
                || channels
                    > std::numeric_limits<std::size_t>::max()
                        / (input_binding.contiguous_storage_capacity
                            * input_binding.geometry.value_size_bytes)) {
                return std::unexpected(
                    "GraphJit continuous input scratch byte size overflows size_t");
            }
            auto const aligned = align_up(
                primitive_scratch_size,
                input_binding.geometry.element_alignment);
            if (!aligned) {
                return std::unexpected(
                    "GraphJit continuous input scratch alignment overflows size_t");
            }
            auto const bytes = channels
                * input_binding.contiguous_storage_capacity
                * input_binding.geometry.value_size_bytes;
            if (bytes > std::numeric_limits<std::size_t>::max() - *aligned) {
                return std::unexpected(
                    "GraphJit continuous input scratch extent overflows size_t");
            }
            input_binding.contiguous_scratch_offset = *aligned;
            input_binding.has_contiguous_scratch = true;
            primitive_scratch_size = *aligned + bytes;
            primitive_scratch_alignments[primitive_index] = std::max(
                primitive_scratch_alignments[primitive_index],
                input_binding.geometry.element_alignment);
        }
        primitive_scratch_sizes[primitive_index] = primitive_scratch_size;
    }

    std::vector<TransientArenaAllocationRequest> scratch_requests;
    std::vector<std::size_t> scratch_primitives;
    for (std::size_t primitive = 0;
         primitive < primitive_scratch_sizes.size();
         ++primitive) {
        if (primitive_scratch_sizes[primitive] == 0) continue;
        scratch_requests.push_back(TransientArenaAllocationRequest{
            .size_bytes = primitive_scratch_sizes[primitive],
            .alignment = primitive_scratch_alignments[primitive],
            .live_interval = primitive_scratch_lifetimes[primitive],
        });
        scratch_primitives.push_back(primitive);
    }
    auto scratch_arena = plan_transient_arena(scratch_requests);
    if (!scratch_arena) {
        return std::unexpected(std::move(scratch_arena.error()));
    }
    if (scratch_arena->size_bytes != 0) {
        auto const scratch_base = align_up(
            plan.storage.transient_arena_size,
            scratch_arena->alignment);
        if (!scratch_base
            || scratch_arena->size_bytes
                > std::numeric_limits<std::size_t>::max() - *scratch_base) {
            return std::unexpected(
                "GraphJit continuous input scratch arena overflows size_t");
        }
        for (std::size_t request = 0; request < scratch_primitives.size(); ++request) {
            auto& primitive = plan.primitives[scratch_primitives[request]];
            auto const allocation = scratch_arena->allocations[request].offset;
            for (auto& input_binding : primitive.inputs) {
                if (!input_binding.has_contiguous_scratch) continue;
                input_binding.contiguous_scratch_offset += *scratch_base + allocation;
            }
        }
        plan.contiguous_input_scratch_size = scratch_arena->size_bytes;
        plan.contiguous_input_scratch_alignment = scratch_arena->alignment;
        plan.storage.transient_arena_size =
            *scratch_base + scratch_arena->size_bytes;
        plan.storage.transient_arena_alignment = std::max(
            plan.storage.transient_arena_alignment,
            scratch_arena->alignment);
    }

    return plan;
}

} // namespace iv::graph_jit::detail
