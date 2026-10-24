#include <intravenous/graph_jit/lowering_plan.h>
#include <intravenous/graph_jit/activity_runtime.h>
#include <intravenous/graph_jit/sample_storage_plan.h>
#include <intravenous/graph_jit/transient_arena_plan.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <llvm/IR/Function.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace iv::graph_jit::detail {
namespace {
void initialize_zero_raw_region(
    std::span<std::byte> storage,
    std::span<std::byte const> data)
{
    (void)data;
    std::ranges::fill(storage, std::byte{});
}

struct PrimitiveBundle {
    std::size_t node_bundle = 0;
    std::size_t node_size = 0;
    std::size_t node_alignment = 1;
    std::size_t maximum_block_size = 0;
    bool block_skippable = false;
    std::optional<std::size_t> ttl_samples{};
};

struct PrimitiveAnalysis {
    PrimitiveBundle bundle{};
    NodeImplementation const* implementation = nullptr;
};

struct GraphAnalysis {
    bool empty = false;
    // Stable primitive inventory consumed by all later phases. For the current
    // disconnected zero-port slice this is configured-bundle order; later
    // schedule/SCC planning may choose a different execution order.
    std::vector<PrimitiveAnalysis> primitives{};
};

std::string primitive_callback_import_symbol(
    std::size_t primitive_index,
    std::string_view operation)
{
    return "__iv_graph_primitive_" + std::to_string(primitive_index) + "_"
        + std::string(operation);
}

std::string node_config_global_symbol(std::size_t primitive_index)
{
    return "iv.graph.node_config." + std::to_string(primitive_index);
}

std::string retained_global_import_symbol(
    std::size_t package_index,
    std::size_t retained_global_index)
{
    return "__iv_graph_retained_global_" + std::to_string(package_index) + "_"
        + std::to_string(retained_global_index);
}

std::string tick_context_global_symbol(std::size_t primitive_index)
{
    return "iv.graph.tick_context." + std::to_string(primitive_index);
}

bool is_structurally_empty(ConfiguredGraph const& graph)
{
    if (!graph.connections.configured_sample_connections().empty()
        || !graph.connections.configured_event_connections().empty()) {
        return false;
    }

    if (graph.node_bundles.size() == 0) return true;
    if (graph.node_bundles.size() != 1) return false;

    auto const& boundary = graph.node_bundles.bundle(0);
    return boundary.is_boundary()
        && graph.public_ports.boundary_handle() == 0
        && boundary.sample_input_count() == 0
        && boundary.sample_output_count() == 0
        && boundary.event_input_count() == 0
        && boundary.event_output_count() == 0;
}

bool is_power_of_two(std::size_t value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

std::expected<std::vector<PrimitiveBundle>, std::string> supported_primitives(
    LoweringInput const& input)
{
    std::vector<PrimitiveBundle> primitives;
    std::string structural_error;
    bool root_boundary_seen = false;
    std::size_t node_bundle = 0;

    input.graph.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            auto const current_bundle = node_bundle++;
            if (!structural_error.empty()) return;

            if (view.kind == ConfiguredNodeBundleKind::boundary) {
                root_boundary_seen = root_boundary_seen
                    || current_bundle
                        == input.graph.public_ports.boundary_handle();
                return;
            }
            if (view.kind == ConfiguredNodeBundleKind::tiled
                || view.kind == ConfiguredNodeBundleKind::subgraph) {
                return;
            }
            if (!view.ports) {
                structural_error =
                    "GraphJit sample-edge slice requires concrete primitive port metadata";
                return;
            }
            if (!is_power_of_two(view.node_alignment)) {
                structural_error =
                    "configured primitive has invalid node configuration alignment";
                return;
            }
            if (!is_power_of_two(view.maximum_block_size)) {
                structural_error =
                    "configured primitive has invalid maximum block size";
                return;
            }
            std::optional<std::size_t> ttl_samples;
            if (view.lifetime && view.lifetime->ttl_samples) {
                ttl_samples = view.lifetime->ttl_samples;
            } else if (view.default_ttl_samples) {
                ttl_samples = *view.default_ttl_samples;
            }
            if (ttl_samples == std::numeric_limits<std::size_t>::max()
                || !view.block_skippable) {
                ttl_samples.reset();
            }
            primitives.push_back(PrimitiveBundle{
                .node_bundle = current_bundle,
                .node_size = view.node_size,
                .node_alignment = view.node_alignment,
                .maximum_block_size = view.maximum_block_size,
                .block_skippable = view.block_skippable,
                .ttl_samples = ttl_samples,
            });
        });

    if (!structural_error.empty()) return std::unexpected(std::move(structural_error));
    if (!root_boundary_seen) {
        return std::unexpected(
            "GraphJit sample-edge slice requires exactly one project boundary");
    }
    if (primitives.empty()) {
        return std::unexpected(
            "GraphJit sample-edge slice found no concrete primitive");
    }
    return primitives;
}

std::expected<GraphAnalysis, std::string> analyze_graph(
    LoweringInput const& input)
{
    if (is_structurally_empty(input.graph)) {
        return GraphAnalysis{.empty = true};
    }

    auto primitive_bundles = supported_primitives(input);
    if (!primitive_bundles) {
        return std::unexpected(std::move(primitive_bundles.error()));
    }

    GraphAnalysis analysis;
    analysis.primitives.reserve(primitive_bundles->size());
    for (auto const& primitive : *primitive_bundles) {
        NodeImplementation const* implementation = nullptr;
        for (auto const& candidate : input.node_implementations) {
            if (candidate.node_bundle != primitive.node_bundle) continue;
            if (implementation) {
                return std::unexpected(
                    "GraphJit lowering received duplicate implementations for one primitive bundle");
            }
            implementation = &candidate;
        }
        if (!implementation) {
            return std::unexpected(
                "GraphJit lowering has no resolved implementation for a concrete primitive");
        }
        auto const has_tick = implementation->tick_block != nullptr;
        auto const has_tock = implementation->tock_coverage != nullptr;
        if (!implementation->package_module || has_tick == has_tock
            || !implementation->declare_node || !implementation->node_data) {
            return std::unexpected(
                "resolved primitive implementation is incomplete at the GraphJit lowering boundary");
        }
        if (has_tick && (!implementation->tick_block_batch
                || !implementation->skip_block
                || !implementation->skip_block_batch)) {
            return std::unexpected(
                "Tick primitive has an incomplete normalized scalar/batch callback family");
        }
        if (has_tock && (!implementation->tock_coverage_batch
                || !implementation->propagate_forward_coverage
                || !implementation->propagate_forward_coverage_batch)) {
            return std::unexpected(
                "Tock primitive has an incomplete normalized scalar/batch callback family");
        }
        if ((!has_tick && (implementation->tick_block_batch
                || implementation->skip_block
                || implementation->skip_block_batch))
            || (!has_tock && (implementation->tock_coverage_batch
                || implementation->propagate_forward_coverage
                || implementation->propagate_forward_coverage_batch
                || implementation->propagate_reverse_coverage
                || implementation->propagate_reverse_coverage_batch))
            || (static_cast<bool>(implementation->propagate_reverse_coverage)
                != static_cast<bool>(
                    implementation->propagate_reverse_coverage_batch))
            || (has_tick
                && implementation->tick_block->getParent()
                    != implementation->package_module)
            || (implementation->skip_block
                && implementation->skip_block->getParent()
                    != implementation->package_module)
            || (implementation->tick_block_batch
                && implementation->tick_block_batch->getParent()
                    != implementation->package_module)
            || (implementation->skip_block_batch
                && implementation->skip_block_batch->getParent()
                    != implementation->package_module)
            || (implementation->tock_coverage
                && implementation->tock_coverage->getParent()
                    != implementation->package_module)
            || (implementation->tock_coverage_batch
                && implementation->tock_coverage_batch->getParent()
                    != implementation->package_module)
            || (implementation->propagate_forward_coverage
                && implementation->propagate_forward_coverage->getParent()
                    != implementation->package_module)
            || (implementation->propagate_forward_coverage_batch
                && implementation->propagate_forward_coverage_batch->getParent()
                    != implementation->package_module)
            || (implementation->propagate_reverse_coverage
                && implementation->propagate_reverse_coverage->getParent()
                    != implementation->package_module)
            || (implementation->propagate_reverse_coverage_batch
                && implementation->propagate_reverse_coverage_batch->getParent()
                    != implementation->package_module)) {
            return std::unexpected(
                "resolved primitive callbacks do not belong to the selected package LLVM module");
        }
        if (primitive.node_size == 0) {
            return std::unexpected("configured primitive has empty node configuration storage");
        }

        auto selected_bundle = primitive;
        if (!has_tick) selected_bundle.ttl_samples.reset();
        analysis.primitives.push_back(PrimitiveAnalysis{
            .bundle = std::move(selected_bundle),
            .implementation = implementation,
        });
    }

    if (input.node_implementations.size() != analysis.primitives.size()) {
        return std::unexpected(
            "GraphJit sample-edge slice requires one resolved implementation per concrete primitive");
    }
    return analysis;
}

std::optional<std::size_t> align_activity_offset(
    std::size_t offset,
    std::size_t alignment) noexcept
{
    if (alignment == 0 || !std::has_single_bit(alignment)) return std::nullopt;
    auto const mask = alignment - 1;
    if (offset > std::numeric_limits<std::size_t>::max() - mask) {
        return std::nullopt;
    }
    return (offset + mask) & ~mask;
}

// Locates the TickSequentialBindingPlan slot through which background
// evaluation supplies this Sequential input in the generated root's
// TickInvocationCall, as an index in the kind-specific TickRuntimePlan array.
// Invalid retained ranges are plan corruption, not evidence of an external
// binding.
std::expected<std::optional<std::size_t>, std::string> external_tick_sequential_slot(
    ConnectionAnalysisPlan const& connections,
    NodeBundleHandle bundle,
    PortKind kind,
    std::size_t input_index)
{
    if (kind != PortKind::sample && kind != PortKind::event) {
        return std::unexpected(
            "GraphJit Tick invocation Sequential input has an invalid port kind");
    }
    auto const& background = connections.background;
    if (bundle >= background.bundle_to_background_node.size()
        || !background.bundle_to_background_node[bundle]) {
        return std::optional<std::size_t>{};
    }
    auto const node = *background.bundle_to_background_node[bundle];
    if (node >= background.tick_runtime.nodes.size()) {
        return std::unexpected(
            "GraphJit background bundle references a missing Tick invocation");
    }
    auto const& invocation = background.tick_runtime.nodes[node];
    auto const& inputs = kind == PortKind::sample
        ? background.tick_runtime.sequential_sample_inputs
        : background.tick_runtime.sequential_event_inputs;
    auto const begin = kind == PortKind::sample
        ? invocation.sequential_sample_begin
        : invocation.sequential_event_begin;
    auto const count = kind == PortKind::sample
        ? invocation.sequential_sample_count
        : invocation.sequential_event_count;
    if (begin > inputs.size() || count > inputs.size() - begin) {
        return std::unexpected(
            "GraphJit Tick invocation has an invalid Sequential input range");
    }
    std::optional<std::size_t> found;
    for (std::size_t slot = begin; slot < begin + count; ++slot) {
        auto const& binding = inputs[slot];
        if (binding.port >= background.ports.size()) {
            return std::unexpected(
                "GraphJit Tick invocation references a missing Sequential port");
        }
        auto const& port = background.ports[binding.port];
        if (port.node != node) {
            return std::unexpected(
                "GraphJit Tick invocation Sequential port belongs to another node");
        }
        if (port.kind == kind
            && port.configured_port.port_index == input_index) {
            found = slot;
        }
    }
    return found;
}

// Builds the additive Tick contribution of one mixed Tick/background sample
// connection. Each selected source channel becomes an explicit mono -> target
// projection, so the resulting full-layout value can be combined with the
// separately materialized background contribution at the callback boundary.
std::expected<SampleConnectionPlan, std::string>
realtime_sample_partition(SampleConnectionPlan const& connection)
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
            });
        return {};
    };

    auto partition_projection = [&](ChannelTypeId source_type,
                                    std::span<std::size_t const> source_indices,
                                    ChannelTypeId target_type,
                                    std::span<std::size_t const> target_channels,
                                    SampleChannelTransformPlan const& transform)
        -> std::expected<void, std::string> {
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
    return result;
}

std::string activity_migration_identity(
    PrimitiveAnalysis const& primitive,
    PlannedGraphNode const& planned,
    PrimitiveActivityPlan const& activity,
    PrimitiveSamplePortPlan const& sample_ports,
    SampleStoragePlan const& sample_storage,
    EventPortBindingPlan const& event_ports,
    PrimitiveEventPortPlan const& primitive_event_ports)
{
    if (!planned.stable_identity) return {};
    auto const& stable = *planned.stable_identity;
    auto const& implementation = *primitive.implementation;
    std::ostringstream out;
    out << "graphjit.activity:graph=" << stable.graph.size() << ':'
        << stable.graph
        << ":node=" << stable.virtual_node.size() << ':'
        << stable.virtual_node
        << ":member=" << stable.direct_member
        << ":type=" << implementation.identity.node_type_id.size() << ':'
        << implementation.identity.node_type_id
        << ":provider=" << implementation.provider_version
        << ":code=" << implementation.code_key.low << '.'
        << implementation.code_key.high
        << ":config="
        << reinterpret_cast<std::uintptr_t>(
            implementation.configuration_identity.get())
        << ":ttl=" << activity.ttl_samples
        << ":storage=" << activity.storage_size
        << '.' << activity.storage_alignment;
    for (std::size_t input = 0;
         input < activity.continuous_inputs.size(); ++input) {
        auto const& activity_input = activity.continuous_inputs[input];
        auto const& binding = sample_ports.inputs[input];
        out << ":input=" << input
            << '.' << static_cast<unsigned>(binding.geometry.value_type)
            << '.' << binding.geometry.elements_per_value
            << '.' << binding.absolute_pace_exponent
            << '.' << binding.read_latency
            << '.' << static_cast<unsigned>(binding.channel_layout.channel_type)
            << '.' << static_cast<unsigned>(binding.channel_layout.sample_layout)
            << '.' << activity_input.static_constant
            << '.' << activity_input.channels.size();
        for (auto const& channel : binding.channels) {
            out << '.' << channel.representation_channel
                << '.' << channel.frame_delay;
        }
        if (activity_input.static_constant) {
            for (auto const& channel : binding.channels) {
                if (channel.storage >= sample_storage.representations.size()
                    || !sample_storage.representations[channel.storage]
                            .constant_value) {
                    out << ":constant=invalid";
                    continue;
                }
                auto const& value = *sample_storage
                    .representations[channel.storage].constant_value;
                out << ":constant=" << value.size();
                for (auto const byte : value) {
                    out << '.' << std::to_integer<unsigned>(byte);
                }
            }
        }
    }
    for (std::size_t output = 0;
         output < sample_ports.outputs.size(); ++output) {
        auto const& binding = sample_ports.outputs[output];
        out << ":output=" << output
            << '.' << static_cast<unsigned>(binding.geometry.value_type)
            << '.' << binding.geometry.elements_per_value
            << '.' << binding.absolute_pace_exponent
            << '.' << binding.history
            << '.' << binding.latency
            << '.' << binding.realtime;
    }
    auto append_event_type = [&](std::optional<std::size_t> storage) {
        if (!storage || *storage >= event_ports.ports.size()) {
            out << ".invalid";
        } else {
            out << '.' << static_cast<unsigned>(
                event_ports.ports[*storage].type);
        }
    };
    for (std::size_t input = 0;
         input < primitive_event_ports.inputs.size(); ++input) {
        out << ":event_input=" << input;
        append_event_type(primitive_event_ports.inputs[input].storage);
    }
    for (std::size_t output = 0;
         output < primitive_event_ports.outputs.size(); ++output) {
        auto const& binding = primitive_event_ports.outputs[output];
        out << ":event_output=" << output
            << '.' << static_cast<unsigned>(binding.source_type)
            << '.' << binding.history
            << '.' << binding.latency
            << '.' << binding.realtime;
        append_event_type(binding.storage);
    }
    return std::move(out).str();
}

std::expected<DeclarationPlan, std::string> plan_declarations(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    ResolvedPortRealization const& port_realization,
    SamplePortBindingPlan& sample_ports,
    EventPortBindingPlan& event_ports)
{
    NodeLayoutBuilder layout_builder(input.specialization.block_size);
    if (analysis.empty) {
        return DeclarationPlan{
            .node_layout = std::move(layout_builder).build(),
            .tock_state_layout = {},
        };
    }

    std::vector<std::size_t> declared_node_indices;
    declared_node_indices.reserve(analysis.primitives.size());
    for (auto const& primitive : analysis.primitives) {
        auto const& implementation = *primitive.implementation;
        auto const bundle = primitive.bundle.node_bundle;
        auto const& configured = input.graph.node_bundles.bundle(bundle);
        std::vector<ReflectedContinuousPortRealization> inputs;
        std::vector<ReflectedContinuousPortRealization> outputs;
        inputs.reserve(configured.sample_input_count());
        outputs.reserve(configured.sample_output_count());
        auto reflect = [](ResolvedContinuousPortRealization const& resolved)
            -> std::expected<ReflectedContinuousPortRealization, std::string> {
            auto geometry = continuous_value_storage_geometry(
                resolved.value_type, resolved.size);
            if (!geometry) {
                return std::unexpected(
                    "GraphJit declaration received invalid resolved continuous-value geometry");
            }
            return ReflectedContinuousPortRealization{
                .geometry = *geometry,
                .sample_support = resolved.sample_support.value_or(0),
                .has_sample_support = resolved.sample_support.has_value(),
                .local_pace_exponent = resolved.local_pace.exponent(),
                .absolute_pace_exponent = resolved.absolute_pace.exponent(),
                .history = resolved.history,
                .latency = resolved.latency,
                .ordering = resolved.ordering,
            };
        };
        for (std::size_t port = 0;
             port < configured.sample_input_count(); ++port) {
            auto reflected = reflect(port_realization.input(bundle, port));
            if (!reflected) return std::unexpected(std::move(reflected.error()));
            inputs.push_back(*reflected);
        }
        for (std::size_t port = 0;
             port < configured.sample_output_count(); ++port) {
            auto reflected = reflect(port_realization.output(bundle, port));
            if (!reflected) return std::unexpected(std::move(reflected.error()));
            outputs.push_back(*reflected);
        }
        ReflectedNodePortRealization reflected_ports{
            .inputs = inputs,
            .outputs = outputs,
        };
        declared_node_indices.push_back(implementation.declare_node(
            implementation.node_data,
            implementation.state_structures,
            layout_builder,
            reflected_ports));
    }

    for (auto const& primitive : sample_ports.primitives) {
        for (auto const& binding : primitive.inputs) {
            // A slot-fed input without history binds only its slot.
            auto const valid_invocation_binding = binding.invocation_supplied
                && !binding.combines_invocation_supplied
                && binding.history == 0
                && binding.channels.empty()
                && channel_count(binding.channel_layout) != 0;
            auto const valid_storage_binding = !binding.channels.empty()
                && binding.channels.size()
                    == channel_count(binding.channel_layout)
                && (!binding.combines_invocation_supplied
                    || (binding.invocation_supplied
                        && binding.realtime_operand_channels.size()
                            == binding.channels.size()));
            auto const valid_random_access_binding = binding.random_access
                && !binding.invocation_supplied
                && binding.channels.empty();
            if (!valid_invocation_binding && !valid_storage_binding
                && !valid_random_access_binding) {
                return std::unexpected(
                    "GraphJit sample runtime declaration has an unbound input port");
            }
        }
        for (auto const& binding : primitive.outputs) {
            if (binding.realtime && !binding.storage) {
                return std::unexpected(
                    "GraphJit sample runtime declaration has an unbound output port");
            }
        }
    }

    for (auto const& primitive : event_ports.primitives) {
        for (auto const& binding : primitive.inputs) {
            auto const valid_invocation_binding = binding.invocation_supplied
                && !binding.combines_invocation_supplied
                && !binding.storage
                && !binding.realtime_operand_storage
                && !binding.combined_storage;
            auto const valid_storage_binding = binding.storage
                && (!binding.combines_invocation_supplied
                    || (binding.invocation_supplied
                        && binding.realtime_operand_storage
                        && binding.combined_storage));
            auto const valid_random_access_binding = binding.random_access
                && !binding.invocation_supplied
                && !binding.storage
                && !binding.combined_storage;
            if (!valid_invocation_binding && !valid_storage_binding
                && !valid_random_access_binding) {
                return std::unexpected(
                    "GraphJit event runtime declaration has an unbound input port");
            }
        }
        for (auto const& binding : primitive.outputs) {
            if (binding.realtime && !binding.storage) {
                return std::unexpected(
                    "GraphJit event runtime declaration has an unbound output port");
            }
        }
    }

    auto declared_sample_storage = declare_sample_storage(
        layout_builder, sample_ports.storage);
    if (!declared_sample_storage) {
        return std::unexpected(std::move(declared_sample_storage.error()));
    }
    if (event_ports.transient_allocations.empty()) {
        if (event_ports.transient_arena_size != 0) {
            return std::unexpected(
                "GraphJit empty transient event plan has a non-zero arena size");
        }
    } else if (event_ports.transient_arena_size == 0
        || !is_power_of_two(event_ports.transient_arena_alignment)) {
        return std::unexpected(
            "GraphJit transient event arena has invalid size/alignment");
    }
    for (auto const& allocation : event_ports.transient_allocations) {
        if (allocation.representation_index
                >= event_ports.ports.size()
            || allocation.size_bytes == 0
            || !is_power_of_two(allocation.alignment)
            || allocation.alignment > event_ports.transient_arena_alignment
            || allocation.region_relative_offset % allocation.alignment != 0
            || allocation.live_interval.begin > allocation.live_interval.end
            || allocation.region_relative_offset
                > event_ports.transient_arena_size
            || allocation.size_bytes > event_ports.transient_arena_size
                    - allocation.region_relative_offset) {
            return std::unexpected(
                "GraphJit transient event allocation lies outside its stack arena");
        }
    }
    for (auto& storage : event_ports.ports) {
        if (storage.immutable_empty) {
            if (storage.persistent
                || storage.transient_allocation
                    != no_event_transient_allocation
                || !storage.migration_identity.empty()
                || storage.has_producer_overflow_counter
                || storage.event_capacity != 0) {
                return std::unexpected(
                    "GraphJit immutable empty event storage has invalid ownership");
            }
            continue;
        }
        if (storage.persistent) {
            if (storage.transient_allocation
                    != no_event_transient_allocation
                || storage.migration_identity.empty()) {
                return std::unexpected(
                    "GraphJit persistent event storage has invalid ownership");
            }
            storage.region = layout_builder.declare_raw_region(
                storage.size_bytes,
                storage.alignment,
                storage.migration_identity,
                initialize_zero_raw_region);
        } else if (storage.transient_allocation
                       >= event_ports.transient_allocations.size()
            || !storage.migration_identity.empty()) {
            return std::unexpected(
                "GraphJit transient event storage has invalid ownership");
        }
        if (storage.has_producer_overflow_counter) {
            storage.overflow_region = layout_builder.declare_raw_region(
                sizeof(std::uint64_t),
                alignof(std::uint64_t),
                {},
                initialize_zero_raw_region);
        }
    }

    std::vector<std::optional<PrimitiveActivityPlan>> primitive_activity(
        analysis.primitives.size());
    std::vector<std::size_t> activity_storage_sizes(
        analysis.primitives.size(), sizeof(PrimitiveActivityState));
    std::vector<std::size_t> activity_storage_alignments(
        analysis.primitives.size(), alignof(PrimitiveActivityState));
    std::vector<ContinuousActivityFrontierPlan> activity_frontiers;
    if (sample_ports.primitives.size() != analysis.primitives.size()
        || event_ports.primitives.size() != analysis.primitives.size()) {
        return std::unexpected(
            "GraphJit activity planning lost a primitive port plan");
    }
    for (std::size_t primitive_index = 0;
         primitive_index < analysis.primitives.size(); ++primitive_index) {
        auto const& primitive = analysis.primitives[primitive_index];
        if (!primitive.bundle.ttl_samples) continue;

        auto const& sample_plan = sample_ports.primitives[primitive_index];
        // TTL dormancy needs proof that every input is unchanged. Published
        // Random Access data changes without a realtime writer, so a
        // primitive with a Random Access input always ticks.
        auto const has_random_access_input =
            std::ranges::any_of(
                sample_plan.inputs,
                [](auto const& binding) { return binding.random_access; })
            || std::ranges::any_of(
                event_ports.primitives[primitive_index].inputs,
                [](auto const& binding) { return binding.random_access; });
        if (has_random_access_input) continue;
        auto const schedule_region =
            primitive.bundle.node_bundle
                    < connections.schedule.bundle_to_region.size()
                ? connections.schedule.bundle_to_region[
                      primitive.bundle.node_bundle]
                : std::nullopt;
        auto const shares_root_invocation_window = schedule_region
            && *schedule_region < connections.schedule.regions.size()
            && !connections.schedule.regions[*schedule_region].cyclic
            && primitive.bundle.maximum_block_size
                >= input.specialization.block_size;
        PrimitiveActivityPlan activity{
            .ttl_samples = *primitive.bundle.ttl_samples,
        };
        activity.continuous_inputs.reserve(sample_plan.inputs.size());
        for (std::size_t input_index = 0;
             input_index < sample_plan.inputs.size(); ++input_index) {
            auto const& input_plan = sample_plan.inputs[input_index];
            auto const externally_bound = input_plan.invocation_supplied;
            auto static_constant = !externally_bound;
            for (auto const& channel : input_plan.channels) {
                if (channel.storage >= sample_ports.storage.representations.size()
                    || !sample_ports.storage.representations[channel.storage]
                            .constant_value) {
                    static_constant = false;
                    break;
                }
            }

            ContinuousInputActivityPlan input_activity{
                .static_constant = static_constant,
            };
            if (!static_constant) {
                // An invocation-supplied input has no realtime channel
                // storage. Each declared channel still owns a frontier whose
                // remembered value tracks the supplied window.
                auto const channel_total = externally_bound
                    ? channel_count(input_plan.channel_layout)
                    : input_plan.channels.size();
                input_activity.channels.reserve(channel_total);
                for (std::size_t channel = 0; channel < channel_total; ++channel) {
                    auto const binding = externally_bound
                        ? PrimitiveSampleInputChannelBindingPlan{
                            .representation_channel = channel,
                        }
                        : input_plan.channels[channel];
                    auto same_frontier = [&](
                        ContinuousActivityFrontierPlan const& candidate) {
                        return candidate.representation == binding.storage
                            && candidate.representation_channel
                                == binding.representation_channel
                            && candidate.frame_delay == binding.frame_delay
                            && candidate.absolute_pace_exponent
                                == input_plan.absolute_pace_exponent
                            && candidate.read_latency == input_plan.read_latency
                            && candidate.geometry == input_plan.geometry;
                    };
                    auto frontier = activity_frontiers.end();
                    // An external Sequential binding is patched from the Tick
                    // invocation frame. Its planned realtime representation is
                    // not enough to prove that another port observes the same
                    // runtime source.
                    if (!externally_bound
                        && shares_root_invocation_window) {
                        frontier = std::ranges::find_if(
                            activity_frontiers, same_frontier);
                    }
                    if (frontier == activity_frontiers.end()) {
                        auto& owner_size = activity_storage_sizes[primitive_index];
                        auto& owner_alignment =
                            activity_storage_alignments[primitive_index];
                        auto const valid_offset = owner_size;
                        if (owner_size
                            == std::numeric_limits<std::size_t>::max()) {
                            return std::unexpected(
                                "GraphJit activity validity storage overflows size_t");
                        }
                        ++owner_size;
                        auto remembered_offset = align_activity_offset(
                            owner_size,
                            input_plan.geometry.element_alignment);
                        if (!remembered_offset
                            || input_plan.geometry.value_size_bytes
                                > std::numeric_limits<std::size_t>::max()
                                    - *remembered_offset) {
                            return std::unexpected(
                                "GraphJit activity remembered-value storage overflows size_t");
                        }
                        owner_size = *remembered_offset
                            + input_plan.geometry.value_size_bytes;
                        owner_alignment = std::max(
                            owner_alignment,
                            input_plan.geometry.element_alignment);
                        activity_frontiers.push_back(
                            ContinuousActivityFrontierPlan{
                                .representation = binding.storage,
                                .representation_channel =
                                    binding.representation_channel,
                                .frame_delay = binding.frame_delay,
                                .absolute_pace_exponent =
                                    input_plan.absolute_pace_exponent,
                                .read_latency = input_plan.read_latency,
                                .geometry = input_plan.geometry,
                                .owner_primitive = primitive_index,
                                .remembered_valid_relative_offset =
                                    valid_offset,
                                .remembered_value_relative_offset =
                                    *remembered_offset,
                            });
                        frontier = std::prev(activity_frontiers.end());
                    }
                    input_activity.channels.push_back(
                        ContinuousActivityChannelStatePlan{
                            .frontier = static_cast<std::size_t>(
                                std::distance(
                                    activity_frontiers.begin(), frontier)),
                        });
                }
            }
            activity.continuous_inputs.push_back(std::move(input_activity));
        }
        for (auto const& output : sample_plan.outputs) {
            if (!output.realtime) continue;
            if (!output.storage
                || *output.storage
                    >= sample_ports.storage.representations.size()) {
                return std::unexpected(
                    "GraphJit activity output lost its storage representation");
            }
            activity.continuous_output_channel_counts.push_back(
                channel_count(sample_ports.storage
                    .representations[*output.storage].channel_layout));
        }
        primitive_activity[primitive_index] = std::move(activity);
    }

    for (std::size_t primitive_index = 0;
         primitive_index < analysis.primitives.size(); ++primitive_index) {
        auto& selected = primitive_activity[primitive_index];
        if (!selected) continue;
        auto& activity = *selected;
        auto aligned_size = align_activity_offset(
            activity_storage_sizes[primitive_index],
            activity_storage_alignments[primitive_index]);
        if (!aligned_size) {
            return std::unexpected(
                "GraphJit activity state alignment overflows size_t");
        }
        activity.storage_size = *aligned_size;
        activity.storage_alignment =
            activity_storage_alignments[primitive_index];
        auto const& primitive = analysis.primitives[primitive_index];
        auto const planned_node = std::ranges::find(
            connections.nodes,
            primitive.bundle.node_bundle,
            &PlannedGraphNode::bundle);
        if (planned_node == connections.nodes.end()) {
            return std::unexpected(
                "GraphJit activity planning lost its scheduled primitive node");
        }
        auto identity = activity_migration_identity(
            primitive,
            *planned_node,
            activity,
            sample_ports.primitives[primitive_index],
            sample_ports.storage,
            event_ports,
            event_ports.primitives[primitive_index]);
        activity.region = layout_builder.declare_raw_region(
            activity.storage_size,
            activity.storage_alignment,
            std::move(identity),
            initialize_zero_raw_region);
    }

    auto tock_state_layout = layout_builder.build_tock_state_layout();
    auto node_layout = std::move(layout_builder).build();
    if (declared_node_indices.size() != analysis.primitives.size()
        || tock_state_layout.nodes.size() != node_layout.nodes.size()) {
        return std::unexpected(
            "GraphJit declaration layout lost its top-level primitive mapping");
    }
    std::vector<bool> mapped_layout_nodes(node_layout.nodes.size(), false);
    for (auto const node : declared_node_indices) {
        if (node >= node_layout.nodes.size() || mapped_layout_nodes[node]) {
            return std::unexpected(
                "GraphJit declaration returned an invalid top-level layout node");
        }
        mapped_layout_nodes[node] = true;
    }
    auto sample_storage_owns_region =
        [&](std::size_t region_index) {
            auto owns = [region_index](auto const& records) {
                return std::ranges::any_of(
                    records,
                    [region_index](auto const& record) {
                        return record.region.valid()
                            && record.region.index == region_index;
                    });
            };
            return owns(sample_ports.storage.persistent_allocations)
                || owns(sample_ports.storage.producer_authored_extents)
                || owns(sample_ports.storage.sequential_pace_cursors);
        };

    for (std::size_t region_index = 0;
         region_index < node_layout.regions.size();
         ++region_index) {
        auto const& region = node_layout.regions[region_index];
        if (region.kind == NodeLayout::Region::Kind::state
            || region.kind == NodeLayout::Region::Kind::local_array
            || region.kind == NodeLayout::Region::Kind::nested_node_states) {
            continue;
        }
        auto const event_storage_owns_region = std::ranges::any_of(
            event_ports.ports,
            [&](EventRepresentationPlan const& storage) {
                return (storage.region.valid()
                           && storage.region.index == region_index)
                    || (storage.overflow_region.valid()
                        && storage.overflow_region.index == region_index);
            });
        auto const activity_owns_region = std::ranges::any_of(
            primitive_activity,
            [&](std::optional<PrimitiveActivityPlan> const& activity) {
                return activity && activity->region.valid()
                    && activity->region.index == region_index;
            });
        if (region.kind == NodeLayout::Region::Kind::raw
            && (sample_storage_owns_region(region_index)
                || event_storage_owns_region
                || activity_owns_region)) {
            continue;
        }
        return std::unexpected(
            "GraphJit lowering does not yet support declaration-owned auxiliary storage regions");
    }

    auto finalized_sample_storage = finalize_sample_storage(
        node_layout, sample_ports.storage);
    if (!finalized_sample_storage) {
        return std::unexpected(std::move(finalized_sample_storage.error()));
    }
    for (std::size_t representation_index = 0;
         representation_index < event_ports.ports.size();
         ++representation_index) {
        auto& storage = event_ports.ports[representation_index];
        if (storage.immutable_empty) continue;
        if (storage.persistent) {
            if (!storage.region.valid()
                || storage.region.index >= node_layout.regions.size()) {
                return std::unexpected(
                    "GraphJit persistent event storage lost its raw region");
            }
            auto const& region = node_layout.regions[storage.region.index];
            if (region.kind != NodeLayout::Region::Kind::raw
                || region.size != storage.size_bytes
                || region.alignment != storage.alignment
                || region.migration_identity != storage.migration_identity
                || region.raw_initialize_fn != initialize_zero_raw_region
                || !region.raw_initialize_data.empty()) {
                return std::unexpected(
                    "GraphJit persistent event storage disagrees with finalized NodeLayout");
            }
            storage.storage_offset = region.storage_offset;
        } else {
            if (storage.region.valid()
                || storage.transient_allocation
                    >= event_ports.transient_allocations.size()) {
                return std::unexpected(
                    "GraphJit transient event storage lost its stack allocation");
            }
            auto const& allocation = event_ports.transient_allocations[
                storage.transient_allocation];
            if (allocation.representation_index != representation_index
                || allocation.size_bytes != storage.size_bytes
                || allocation.alignment != storage.alignment
                || allocation.region_relative_offset
                    > event_ports.transient_arena_size
                || allocation.size_bytes > event_ports.transient_arena_size
                        - allocation.region_relative_offset) {
                return std::unexpected(
                    "GraphJit transient event storage disagrees with its stack arena");
            }
        }
        if (storage.count_relative_offset > storage.size_bytes
            || storage.read_index_relative_offset > storage.size_bytes
            || storage.write_index_relative_offset > storage.size_bytes
            || (storage.has_sequential_timeline
                && (storage.timeline_initialized_relative_offset
                        >= storage.size_bytes
                    || storage.expected_next_time_relative_offset
                        > storage.size_bytes
                    || sizeof(GlobalIndex) > storage.size_bytes
                        - storage.expected_next_time_relative_offset))
            || storage.events_relative_offset > storage.size_bytes
            || (storage.has_source_indices
                && storage.source_indices_relative_offset
                    > storage.size_bytes)) {
            return std::unexpected(
                "GraphJit event storage contains an invalid relative field offset");
        }
        if (!storage.has_source_indices
            && storage.source_indices_relative_offset != 0) {
            return std::unexpected(
                "GraphJit ordinary event storage unexpectedly has source indices");
        }
        if (storage.has_producer_overflow_counter) {
            if (!storage.overflow_region.valid()
                || storage.overflow_region.index
                    >= node_layout.regions.size()) {
                return std::unexpected(
                    "GraphJit event producer telemetry lost its persistent region");
            }
            auto const& overflow =
                node_layout.regions[storage.overflow_region.index];
            if (overflow.kind != NodeLayout::Region::Kind::raw
                || overflow.size != sizeof(std::uint64_t)
                || overflow.alignment != alignof(std::uint64_t)
                || !overflow.migration_identity.empty()
                || overflow.raw_initialize_fn != initialize_zero_raw_region
                || !overflow.raw_initialize_data.empty()) {
                return std::unexpected(
                    "GraphJit event producer telemetry disagrees with finalized NodeLayout");
            }
            storage.overflow_count_storage_offset = overflow.storage_offset;
        } else if (storage.overflow_region.valid()) {
            return std::unexpected(
                "GraphJit derived event storage unexpectedly owns producer telemetry");
        }
    }


    DeclarationPlan plan{
        .node_layout = std::move(node_layout),
        .tock_state_layout = std::move(tock_state_layout),
        .primitive_layout_nodes = std::move(declared_node_indices),
        .primitive_activity = std::move(primitive_activity),
        .activity_frontiers = std::move(activity_frontiers),
    };
    for (auto& activity : plan.primitive_activity) {
        if (!activity) continue;
        if (!activity->region.valid()
            || activity->region.index >= plan.node_layout.regions.size()) {
            return std::unexpected(
                "GraphJit activity state lost its persistent raw region");
        }
        auto const& region = plan.node_layout.regions[activity->region.index];
        if (region.kind != NodeLayout::Region::Kind::raw
            || region.size != activity->storage_size
            || region.alignment != activity->storage_alignment
            || region.raw_initialize_fn != initialize_zero_raw_region
            || !region.raw_initialize_data.empty()) {
            return std::unexpected(
                "GraphJit activity state disagrees with finalized NodeLayout");
        }
        activity->storage_offset = region.storage_offset;
    }
    for (auto& frontier : plan.activity_frontiers) {
        // An invocation-supplied frontier has no realtime representation.
        if ((frontier.representation != no_sample_representation
                && frontier.representation
                    >= sample_ports.storage.representations.size())
            || frontier.owner_primitive
                >= plan.primitive_activity.size()
            || !plan.primitive_activity[frontier.owner_primitive]) {
            return std::unexpected(
                "GraphJit activity frontier lost its storage owner");
        }
        auto const& owner = *plan.primitive_activity[
            frontier.owner_primitive];
        if (frontier.remembered_valid_relative_offset
                >= owner.storage_size
            || frontier.remembered_value_relative_offset
                > owner.storage_size
            || frontier.geometry.value_size_bytes
                > owner.storage_size
                    - frontier.remembered_value_relative_offset
            || owner.storage_offset
                > std::numeric_limits<std::size_t>::max()
                    - frontier.remembered_valid_relative_offset
            || owner.storage_offset
                > std::numeric_limits<std::size_t>::max()
                    - frontier.remembered_value_relative_offset) {
            return std::unexpected(
                "GraphJit activity frontier lies outside its persistent region");
        }
        frontier.remembered_valid_storage_offset = owner.storage_offset
            + frontier.remembered_valid_relative_offset;
        frontier.remembered_value_storage_offset = owner.storage_offset
            + frontier.remembered_value_relative_offset;
    }
    plan.primitive_storage.reserve(analysis.primitives.size());
    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const& implementation = *analysis.primitives[i].implementation;
        auto const layout_node = plan.primitive_layout_nodes[i];
        auto const& node_record = plan.node_layout.nodes[layout_node];
        auto const& tock_record = plan.tock_state_layout.nodes[layout_node];
        if (node_record.state_size != implementation.state_size
            || node_record.state_alignment != implementation.state_alignment
            || tock_record.state_size != implementation.background_state_size
            || tock_record.state_alignment
                != implementation.background_state_alignment) {
            return std::unexpected(
                "declared primitive State/TockState layout disagrees with finalized compiler metadata");
        }
        if (node_record.state_size != 0 && node_record.state_offset < 0) {
            return std::unexpected(
                "declared primitive State has no canonical NodeLayout storage offset");
        }
        PrimitiveStoragePlan storage;
        if (node_record.state_size != 0) {
            storage.has_state = true;
            storage.state_offset = static_cast<std::size_t>(node_record.state_offset);
            if (storage.state_offset > plan.node_layout.storage_size
                || node_record.state_size
                    > plan.node_layout.storage_size - storage.state_offset) {
                return std::unexpected(
                    "declared primitive State lies outside canonical NodeStorage");
            }
            storage.state_size = node_record.state_size;
        }
        plan.primitive_storage.push_back(storage);
    }
    return plan;
}

std::expected<RealtimePortStateRealizations, std::string>
plan_sample_realtime_port_state_realizations(
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    DeclarationPlan const& declarations,
    SamplePortBindingPlan const& sample_ports)
{
    RealtimePortStateRealizations result;
    result.sample_states.reserve(
        std::ranges::count_if(
            connections.realtime_port_states.states,
            [](RealtimePortStateRequirement const& state) {
                return state.configured_port.port_kind == PortKind::sample;
            }));

    auto primitive_index_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
            if (analysis.primitives[i].bundle.node_bundle == bundle) return i;
        }
        return std::nullopt;
    };

    auto storage_view = [&] (
        std::size_t representation_index,
        std::size_t representation_channel,
        std::int64_t timeline_offset,
        bool retained)
        -> std::expected<
            std::optional<SampleRealtimePortStateStorageView>,
            std::string> {
        if (representation_index == no_sample_representation
            || representation_index
                >= sample_ports.storage.representations.size()) {
            return std::unexpected(
                "GraphJit sample port-state binding names an invalid representation");
        }
        auto const& representation =
            sample_ports.storage.representations[representation_index];
        if (representation_channel
            >= channel_count(representation.channel_layout)) {
            return std::unexpected(
                "GraphJit sample port-state channel lies outside its representation");
        }
        auto const cursor = std::ranges::find(
            sample_ports.storage.sequential_pace_cursors,
            representation.absolute_pace_exponent,
            &SequentialPaceCursorPlan::absolute_pace_exponent);
        if (cursor == sample_ports.storage.sequential_pace_cursors.end()
            || cursor->storage_offset > declarations.node_layout.storage_size
            || sizeof(std::uint64_t)
                > declarations.node_layout.storage_size
                    - cursor->storage_offset) {
            return std::unexpected(
                "GraphJit sample port-state representation lost its Sequential pace cursor");
        }

        SampleRealtimePortStateStorageView view{
            .channel_layout = representation.channel_layout,
            .geometry = representation.geometry,
            .absolute_pace_exponent = representation.absolute_pace_exponent,
            .sequential_cursor_node_storage_offset = cursor->storage_offset,
            .representation_channel = representation_channel,
            .timeline_offset_frames = timeline_offset,
            .working_frame_capacity = representation.frame_capacity,
        };

        if (representation.constant_value) {
            if (representation.persistent_allocation
                    != no_sample_persistent_allocation
                || representation.transient_allocation
                    != no_sample_transient_allocation) {
                return std::unexpected(
                    "GraphJit constant sample state unexpectedly owns mutable storage");
            }
            view.storage =
                SampleRealtimePortStateStorage::immutable_constant;
            view.storage_frame_count = representation.frame_capacity;
            view.constant_value = representation.constant_value;
            return std::optional<SampleRealtimePortStateStorageView>{
                std::move(view)};
        }

        auto persistent_view = [&] ()
            -> std::expected<
                std::optional<SampleRealtimePortStateStorageView>,
                std::string> {
            if (representation.persistent_allocation
                == no_sample_persistent_allocation) {
                return std::optional<SampleRealtimePortStateStorageView>{};
            }
            auto const allocation_index =
                representation.persistent_allocation;
            if (allocation_index
                >= sample_ports.storage.persistent_allocations.size()) {
                return std::unexpected(
                    "GraphJit sample port-state representation lost its persistent allocation");
            }
            auto const& allocation =
                sample_ports.storage.persistent_allocations[allocation_index];
            if (allocation.representation_index != representation_index
                || allocation.channel_layout
                    != representation.channel_layout
                || allocation.storage_offset
                    > declarations.node_layout.storage_size
                || allocation.size_bytes
                    > declarations.node_layout.storage_size
                        - allocation.storage_offset) {
                return std::unexpected(
                    "GraphJit sample port-state persistent allocation disagrees with finalized NodeStorage");
            }
            view.node_storage_offset = allocation.storage_offset;
            view.storage_size_bytes = allocation.size_bytes;
            view.storage_frame_count =
                allocation.kind == SamplePersistentStorageKind::compact_carry
                ? allocation.retained_frames
                : allocation.frame_capacity;
            if (allocation.kind == SamplePersistentStorageKind::ring) {
                view.storage = SampleRealtimePortStateStorage::ring;
            } else {
                view.storage =
                    SampleRealtimePortStateStorage::compact_carry;
                auto carry = sample_ports.storage.carry_operations.end();
                for (auto candidate =
                         sample_ports.storage.carry_operations.begin();
                     candidate != sample_ports.storage.carry_operations.end();
                     ++candidate) {
                    if (candidate->persistent_allocation
                        != allocation_index) {
                        continue;
                    }
                    if (carry
                        != sample_ports.storage.carry_operations.end()) {
                        return std::unexpected(
                            "GraphJit compact sample state has more than one carry operation");
                    }
                    carry = candidate;
                }
                if (carry == sample_ports.storage.carry_operations.end()
                    || carry->representation_index != representation_index
                    || carry->retained_frames
                        != allocation.retained_frames) {
                    return std::unexpected(
                        "GraphJit compact sample state lost its carry operation");
                }
                view.carry_future_frames = carry->future_frames;
            }
            return std::optional<SampleRealtimePortStateStorageView>{
                std::move(view)};
        };

        if (retained
            || representation.storage
                == RealtimeBufferStorageKind::full_node_storage) {
            return persistent_view();
        }

        if (representation.transient_allocation
                >= sample_ports.storage.transient_allocations.size()) {
            return std::unexpected(
                "GraphJit callback sample port state lost its transient allocation");
        }
        auto const& allocation = sample_ports.storage.transient_allocations[
            representation.transient_allocation];
        if (allocation.representation_index != representation_index
            || allocation.region_relative_offset
                > sample_ports.storage.transient_arena_size
            || allocation.size_bytes
                > sample_ports.storage.transient_arena_size
                    - allocation.region_relative_offset) {
            return std::unexpected(
                "GraphJit sample port-state callback allocation lies outside its arena");
        }
        view.storage = SampleRealtimePortStateStorage::callback_transient;
        view.callback_arena_offset = allocation.region_relative_offset;
        view.storage_size_bytes = allocation.size_bytes;
        view.storage_frame_count = representation.frame_capacity;
        return std::optional<SampleRealtimePortStateStorageView>{
            std::move(view)};
    };

    auto materialization_for_target = [&](std::size_t representation)
        -> std::expected<SampleMaterializationPlan const*, std::string> {
        SampleMaterializationPlan const* selected = nullptr;
        for (auto const& materialization :
             sample_ports.storage.materializations) {
            if (materialization.target_representation != representation) {
                continue;
            }
            if (selected) {
                return std::unexpected(
                    "GraphJit sample port state has more than one materialization producer");
            }
            selected = &materialization;
        }
        return selected;
    };

    auto composition_for_target = [&](std::size_t representation)
        -> std::expected<SampleCompositionPlan const*, std::string> {
        SampleCompositionPlan const* selected = nullptr;
        for (auto const& composition : sample_ports.storage.compositions) {
            if (composition.target_representation != representation) continue;
            if (selected) {
                return std::unexpected(
                    "GraphJit sample port state has more than one composition producer");
            }
            selected = &composition;
        }
        return selected;
    };

    for (std::size_t requirement_index = 0;
         requirement_index
            < connections.realtime_port_states.states.size();
         ++requirement_index) {
        auto const& requirement =
            connections.realtime_port_states.states[requirement_index];
        if (requirement.configured_port.port_kind != PortKind::sample) continue;
        if (!requirement.channel || requirement.extent_samples == 0) {
            return std::unexpected(
                "GraphJit sample port-state requirement has no channel or extent");
        }

        auto const primitive = primitive_index_for_bundle(
            requirement.configured_port.node_bundle_handle);
        if (!primitive || *primitive >= sample_ports.primitives.size()) {
            return std::unexpected(
                "GraphJit sample port-state requirement lost its concrete primitive");
        }

        std::size_t representation_index = no_sample_representation;
        std::size_t representation_channel = 0;
        std::int64_t timeline_offset = 0;
        auto const port = requirement.configured_port.port_index;
        auto const& primitive_ports = sample_ports.primitives[*primitive];
        if (requirement.direction == PortDirection::input) {
            if (requirement.role
                    != RealtimePortStateRole::sequential_input_history
                || port >= primitive_ports.inputs.size()) {
                return std::unexpected(
                    "GraphJit sample input state disagrees with its port binding");
            }
            // A slot-fed or mixed input with history binds its node-owned
            // timeline like any other retained input storage.
            auto const& binding = primitive_ports.inputs[port];
            if (*requirement.channel >= binding.channels.size()
                || binding.history != requirement.extent_samples) {
                return std::unexpected(
                    "GraphJit sample input state disagrees with its semantic extent");
            }
            auto const& channel = binding.channels[*requirement.channel];
            representation_index = channel.storage;
            representation_channel = channel.representation_channel;
            if (channel.frame_delay
                > std::numeric_limits<std::size_t>::max()
                    - binding.read_latency) {
                return std::unexpected(
                    "GraphJit sample input state timeline delay overflows size_t");
            }
            auto const timeline_delay =
                binding.read_latency + channel.frame_delay;
            if (timeline_delay
                > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                return std::unexpected(
                    "GraphJit sample input state timeline offset is not representable");
            }
            timeline_offset = -static_cast<std::int64_t>(timeline_delay);
        } else {
            if (port >= primitive_ports.outputs.size()) {
                return std::unexpected(
                    "GraphJit sample output state lost its port binding");
            }
            auto const& binding = primitive_ports.outputs[port];
            if (!binding.realtime || !binding.storage) {
                return std::unexpected(
                    "GraphJit Tick output state has no realtime storage binding");
            }
            std::size_t expected_extent = 0;
            if (requirement.role
                == RealtimePortStateRole::tick_output_history) {
                expected_extent = binding.history;
            } else if (requirement.role
                       == RealtimePortStateRole::tick_output_latency) {
                expected_extent = binding.latency;
            }
            if (expected_extent == 0
                || expected_extent != requirement.extent_samples) {
                return std::unexpected(
                    "GraphJit sample output state disagrees with its semantic extent");
            }
            representation_index = *binding.storage;
            representation_channel = *requirement.channel;
            if (binding.latency
                > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                return std::unexpected(
                    "GraphJit sample output state timeline offset is not representable");
            }
            timeline_offset = static_cast<std::int64_t>(binding.latency);
        }

        auto authored = storage_view(
            representation_index,
            representation_channel,
            timeline_offset,
            false);
        if (!authored) return std::unexpected(std::move(authored.error()));
        if (!*authored) {
            return std::unexpected(
                "GraphJit sample port state has no callback-facing realization");
        }
        SampleRealtimePortStateRealization realization{
            .requirement_index = requirement_index,
            .authored_storage = std::move(**authored),
        };

        if (realization.authored_storage.storage
            == SampleRealtimePortStateStorage::immutable_constant) {
            realization.retained_access =
                SampleRealtimePortStateAccess::immutable_constant;
            result.sample_states.push_back(std::move(realization));
            continue;
        }

        // Retained Tick output storage is already indexed in the semantic
        // output timeline. The positive authored latency belongs only to the
        // callback-facing view; applying it again during migration would move
        // both history and future state twice.
        auto const retained_timeline_offset =
            requirement.direction == PortDirection::output
            ? std::int64_t{0}
            : timeline_offset;
        auto retained = storage_view(
            representation_index,
            representation_channel,
            retained_timeline_offset,
            true);
        if (!retained) return std::unexpected(std::move(retained.error()));
        if (*retained) {
            realization.retained_access =
                SampleRealtimePortStateAccess::direct;
            realization.retained_sources.push_back(std::move(**retained));
            result.sample_states.push_back(std::move(realization));
            continue;
        }

        if (requirement.direction == PortDirection::input) {
            auto materialization = materialization_for_target(
                representation_index);
            if (!materialization) {
                return std::unexpected(std::move(materialization.error()));
            }
            if (*materialization) {
                auto const& operation = **materialization;
                if (operation.target_layout
                        != realization.authored_storage.channel_layout
                    || operation.source_representation
                        >= sample_ports.storage.representations.size()) {
                    return std::unexpected(
                        "GraphJit sample port-state materialization disagrees with its callback view");
                }
                auto const& source = sample_ports.storage.representations[
                    operation.source_representation];
                if (source.channel_layout != operation.source_layout) {
                    return std::unexpected(
                        "GraphJit sample port-state materialization lost its source layout");
                }
                for (std::size_t source_channel = 0;
                     source_channel < channel_count(operation.source_layout);
                     ++source_channel) {
                    auto source_view = storage_view(
                        operation.source_representation,
                        source_channel,
                        timeline_offset,
                        true);
                    if (!source_view) {
                        return std::unexpected(
                            std::move(source_view.error()));
                    }
                    if (!*source_view) {
                        realization.retained_sources.clear();
                        break;
                    }
                    realization.retained_sources.push_back(
                        std::move(**source_view));
                }
                if (!realization.retained_sources.empty()) {
                    realization.retained_access =
                        SampleRealtimePortStateAccess::materialized;
                    realization.materialization =
                        SampleRealtimePortStateMaterialization{
                            .source_layout = operation.source_layout,
                            .target_layout = operation.target_layout,
                            .retained_before = operation.retained_before,
                            .latest_read_latency =
                                operation.latest_read_latency,
                        };
                }
            } else {
                auto composition = composition_for_target(
                    representation_index);
                if (!composition) {
                    return std::unexpected(std::move(composition.error()));
                }
                if (*composition) {
                    bool selected_target = false;
                    for (auto const& contribution : (**composition).contributions) {
                        for (std::size_t converted_channel = 0;
                             converted_channel
                                < contribution.target_channels.size();
                             ++converted_channel) {
                            if (contribution.target_channels[converted_channel]
                                != representation_channel) {
                                continue;
                            }
                            if (selected_target
                                || contribution.sources.size()
                                    != channel_count(
                                        contribution.source_layout)) {
                                return std::unexpected(
                                    "GraphJit composed port state has an ambiguous target channel");
                            }
                            selected_target = true;
                            for (std::size_t source_channel = 0;
                                 source_channel
                                    < contribution.sources.size();
                                 ++source_channel) {
                                auto const coefficient =
                                    channel_conversion_coefficient(
                                        contribution.source_layout.channel_type,
                                        contribution.converted_layout.channel_type,
                                        source_channel,
                                        converted_channel);
                                if (coefficient == 0.0f) continue;
                                auto const& source =
                                    contribution.sources[source_channel];
                                if (source.read_latency
                                    > static_cast<std::size_t>(
                                        std::numeric_limits<std::int64_t>::max())) {
                                    return std::unexpected(
                                        "GraphJit composed port-state latency is not representable");
                                }
                                auto source_view = storage_view(
                                    source.source_representation,
                                    source.source_channel,
                                    -static_cast<std::int64_t>(
                                        source.read_latency),
                                    true);
                                if (!source_view) {
                                    return std::unexpected(
                                        std::move(source_view.error()));
                                }
                                if (!*source_view) {
                                    realization.composition_sources.clear();
                                    selected_target = false;
                                    break;
                                }
                                realization.composition_sources.push_back({
                                    .storage = std::move(**source_view),
                                    .coefficient = coefficient,
                                });
                            }
                        }
                    }
                    if (selected_target
                        && !realization.composition_sources.empty()) {
                        realization.retained_access =
                            SampleRealtimePortStateAccess::composed;
                    }
                }
            }
        }
        result.sample_states.push_back(std::move(realization));
    }
    return result;
}

std::expected<std::vector<EventRealtimePortStateRealization>, std::string>
plan_event_realtime_port_state_realizations(
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    DeclarationPlan const& declarations,
    EventPortBindingPlan const& event_ports)
{
    std::vector<EventRealtimePortStateRealization> result;
    result.reserve(
        std::ranges::count_if(
            connections.realtime_port_states.states,
            [](RealtimePortStateRequirement const& state) {
                return state.configured_port.port_kind == PortKind::event;
            }));

    auto primitive_index_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
            if (analysis.primitives[i].bundle.node_bundle == bundle) return i;
        }
        return std::nullopt;
    };

    auto storage_view = [&](std::size_t representation_index)
        -> std::expected<EventRealtimePortStateStorageView, std::string> {
        if (representation_index >= event_ports.ports.size()) {
            return std::unexpected(
                "GraphJit event port state names an invalid representation");
        }
        auto const& representation = event_ports.ports[representation_index];
        EventRealtimePortStateStorageView view{
            .type = representation.type,
            .event_capacity = representation.event_capacity,
            .size_bytes = representation.size_bytes,
            .alignment = representation.alignment,
            .count_relative_offset = representation.count_relative_offset,
            .read_index_relative_offset =
                representation.read_index_relative_offset,
            .write_index_relative_offset =
                representation.write_index_relative_offset,
            .has_sequential_timeline =
                representation.has_sequential_timeline,
            .timeline_initialized_relative_offset =
                representation.timeline_initialized_relative_offset,
            .expected_next_time_relative_offset =
                representation.expected_next_time_relative_offset,
            .events_relative_offset = representation.events_relative_offset,
            .has_source_indices = representation.has_source_indices,
            .source_indices_relative_offset =
                representation.source_indices_relative_offset,
        };
        if (representation.immutable_empty) {
            view.storage = EventRealtimePortStateStorage::immutable_empty;
            return view;
        }
        if (representation.persistent) {
            if (representation.transient_allocation
                    != no_event_transient_allocation
                || representation.storage_offset
                    > declarations.node_layout.storage_size
                || representation.size_bytes
                    > declarations.node_layout.storage_size
                        - representation.storage_offset) {
                return std::unexpected(
                    "GraphJit persistent event port state disagrees with finalized NodeStorage");
            }
            view.storage = representation.persistent_ring
                ? EventRealtimePortStateStorage::ring
                : EventRealtimePortStateStorage::compact_carry;
            view.node_storage_offset = representation.storage_offset;
            return view;
        }
        if (representation.persistent_ring
            || representation.transient_allocation
                >= event_ports.transient_allocations.size()) {
            return std::unexpected(
                "GraphJit callback event port state lost its transient allocation");
        }
        auto const& allocation = event_ports.transient_allocations[
            representation.transient_allocation];
        if (allocation.representation_index != representation_index
            || allocation.region_relative_offset
                > event_ports.transient_arena_size
            || allocation.size_bytes
                > event_ports.transient_arena_size
                    - allocation.region_relative_offset
            || allocation.size_bytes != representation.size_bytes
            || allocation.alignment != representation.alignment) {
            return std::unexpected(
                "GraphJit callback event port state lies outside its arena");
        }
        view.storage = EventRealtimePortStateStorage::callback_transient;
        view.callback_arena_offset = allocation.region_relative_offset;
        return view;
    };

    auto retained_representation = [&](std::size_t working_representation)
        -> std::expected<std::optional<std::size_t>, std::string> {
        if (working_representation >= event_ports.ports.size()) {
            return std::unexpected(
                "GraphJit event port state lost its working representation");
        }
        if (event_ports.ports[working_representation].persistent) {
            return std::optional<std::size_t>{working_representation};
        }
        std::optional<std::size_t> retained;
        for (auto const& carry : event_ports.carry_operations) {
            if (carry.working_representation != working_representation) continue;
            if (retained) {
                return std::unexpected(
                    "GraphJit event port state has more than one compact carry");
            }
            if (carry.persistent_representation >= event_ports.ports.size()
                || !event_ports.ports[carry.persistent_representation].persistent
                || event_ports.ports[carry.persistent_representation]
                       .persistent_ring) {
                return std::unexpected(
                    "GraphJit event port state has an invalid compact carry backing");
            }
            retained = carry.persistent_representation;
        }
        return retained;
    };

    auto materialization_for_target = [&](std::size_t representation)
        -> std::expected<EventMaterializationPlan const*, std::string> {
        EventMaterializationPlan const* selected = nullptr;
        for (auto const& materialization : event_ports.materializations) {
            if (materialization.target_representation != representation) continue;
            if (selected) {
                return std::unexpected(
                    "GraphJit event port state has more than one materialization producer");
            }
            selected = &materialization;
        }
        return selected;
    };

    for (std::size_t requirement_index = 0;
         requirement_index
            < connections.realtime_port_states.states.size();
         ++requirement_index) {
        auto const& requirement =
            connections.realtime_port_states.states[requirement_index];
        if (requirement.configured_port.port_kind != PortKind::event) continue;
        if (requirement.channel || requirement.extent_samples == 0) {
            return std::unexpected(
                "GraphJit event port-state requirement has a channel or no extent");
        }

        auto const primitive = primitive_index_for_bundle(
            requirement.configured_port.node_bundle_handle);
        if (!primitive || *primitive >= event_ports.primitives.size()) {
            return std::unexpected(
                "GraphJit event port-state requirement lost its concrete primitive");
        }
        auto const& primitive_ports = event_ports.primitives[*primitive];
        auto const port = requirement.configured_port.port_index;

        std::size_t authored_representation = 0;
        if (requirement.direction == PortDirection::input) {
            if (requirement.role
                    != RealtimePortStateRole::sequential_input_history
                || port >= primitive_ports.inputs.size()) {
                return std::unexpected(
                    "GraphJit event input state disagrees with its port binding");
            }
            // A slot-fed or mixed input with history binds its node-owned
            // timeline ring like any other retained input sequence.
            if (!primitive_ports.inputs[port].storage) {
                return std::unexpected(
                    "GraphJit event input state disagrees with its port binding");
            }
            authored_representation = *primitive_ports.inputs[port].storage;
        } else {
            if (port >= primitive_ports.outputs.size()) {
                return std::unexpected(
                    "GraphJit event output state lost its port binding");
            }
            auto const& binding = primitive_ports.outputs[port];
            if (!binding.realtime || !binding.storage) {
                return std::unexpected(
                    "GraphJit Tick event output state has no realtime storage binding");
            }
            std::size_t expected_extent = 0;
            if (requirement.role
                == RealtimePortStateRole::tick_output_history) {
                expected_extent = binding.history;
            } else if (requirement.role
                       == RealtimePortStateRole::tick_output_latency) {
                expected_extent = binding.latency;
            }
            if (expected_extent == 0
                || expected_extent != requirement.extent_samples) {
                return std::unexpected(
                    "GraphJit event output state disagrees with its semantic extent");
            }
            authored_representation = binding.canonical_storage.value_or(
                *binding.storage);
        }

        auto authored = storage_view(authored_representation);
        if (!authored) return std::unexpected(std::move(authored.error()));
        EventRealtimePortStateRealization realization{
            .requirement_index = requirement_index,
            .authored_storage = std::move(*authored),
        };

        if (requirement.direction == PortDirection::input) {
            auto materialization = materialization_for_target(
                authored_representation);
            if (!materialization) {
                return std::unexpected(std::move(materialization.error()));
            }
            auto source_representation = authored_representation;
            if (*materialization) {
                source_representation = (*materialization)->source_representation;
                realization.materialization_conversion =
                    (*materialization)->conversion;
                realization.materialization_history_samples =
                    (*materialization)->history_samples;
                realization.materialization_selects_invocation_window =
                    (*materialization)->select_invocation_window;
            }
            auto retained = retained_representation(source_representation);
            if (!retained) return std::unexpected(std::move(retained.error()));
            if (*retained) {
                auto view = storage_view(**retained);
                if (!view) return std::unexpected(std::move(view.error()));
                realization.retained_storage = std::move(*view);
                realization.retained_access = *materialization
                    ? EventRealtimePortStateAccess::materialized
                    : EventRealtimePortStateAccess::direct;

                auto const group_index = event_ports.ports[
                    source_representation].producer_group_index;
                if (group_index
                        < connections.event_producer_groups.size()
                    && group_index
                        < event_ports.producer_group_representations.size()
                    && event_ports.producer_group_representations[group_index]
                        == source_representation) {
                    auto const& group =
                        connections.event_producer_groups[group_index];
                    if (group.sources.size()
                        != group.source_conversions.size()) {
                        return std::unexpected(
                            "GraphJit event input state lost producer-group source conversions");
                    }
                    realization.retained_future_sources.reserve(
                        group.sources.size());
                    for (std::size_t source_index = 0;
                         source_index < group.sources.size();
                         ++source_index) {
                        auto const source = group.sources[source_index];
                        auto const latency_requirement = std::ranges::find_if(
                            connections.realtime_port_states.states,
                            [&](RealtimePortStateRequirement const& candidate) {
                                return candidate.configured_port
                                        == NodeBundlePortId{
                                            source.bundle,
                                            PortKind::event,
                                            source.port}
                                    && candidate.direction
                                        == PortDirection::output
                                    && candidate.role
                                        == RealtimePortStateRole::
                                            tick_output_latency;
                            });
                        if (latency_requirement
                            == connections.realtime_port_states.states.end()) {
                            continue;
                        }
                        realization.retained_future_sources.push_back(
                            EventRealtimePortStateFutureSource{
                                .requirement_index =
                                    static_cast<std::size_t>(std::distance(
                                        connections.realtime_port_states.states
                                            .begin(),
                                        latency_requirement)),
                                .conversion =
                                    group.source_conversions[source_index],
                                .target_source_index =
                                    realization.retained_storage
                                            ->has_source_indices
                                    ? std::optional<std::size_t>{source_index}
                                    : std::optional<std::size_t>{},
                            });
                    }
                }
            } else if (realization.authored_storage.event_capacity == 0) {
                realization.retained_access =
                    EventRealtimePortStateAccess::immutable_empty;
            }
            result.push_back(std::move(realization));
            continue;
        }

        auto retained = retained_representation(authored_representation);
        if (!retained) return std::unexpected(std::move(retained.error()));
        if (*retained) {
            auto view = storage_view(**retained);
            if (!view) return std::unexpected(std::move(view.error()));
            realization.retained_storage = std::move(*view);
            realization.retained_access = EventRealtimePortStateAccess::direct;
        }
        result.push_back(std::move(realization));
    }
    return result;
}

std::expected<PackageImportPlan, std::string> plan_package_imports(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    BackgroundEvaluationPlan const& background)
{
    PackageImportPlan plan;
    if (analysis.empty && input.config_relocations.empty()) return plan;

    auto find_package_index = [&](llvm::Module const* module)
        -> std::expected<std::size_t, std::string> {
        if (!module) {
            return std::unexpected(
                "GraphJit lowering import source has no package LLVM module");
        }
        std::optional<std::size_t> selected_package_index;
        for (std::size_t i = 0; i < input.packages.size(); ++i) {
            if (input.packages[i].module.get() != module) continue;
            if (selected_package_index) {
                return std::unexpected(
                    "GraphJit lowering received duplicate ownership for one package LLVM module");
            }
            selected_package_index = i;
        }
        if (!selected_package_index || !input.packages[*selected_package_index].module) {
            return std::unexpected(
                "resolved package LLVM is not available for lowering consumption");
        }
        return *selected_package_index;
    };

    auto package_group = [&](std::size_t package_index) -> PackageImportGroup& {
        for (auto& package : plan.packages) {
            if (package.package_index == package_index) return package;
        }
        plan.packages.push_back(PackageImportGroup{.package_index = package_index});
        return plan.packages.back();
    };

    auto add_callback = [&](
                            PackageImportGroup& package,
                            std::string source_symbol,
                            std::string import_symbol,
                            std::string role,
                            CallbackImportAbi abi) -> std::string {
        for (auto const& callback : package.callbacks) {
            if (callback.source_symbol == source_symbol) {
                return callback.import_symbol;
            }
        }
        package.callbacks.push_back(CallbackImportPlan{
            .source_symbol = std::move(source_symbol),
            .import_symbol = std::move(import_symbol),
            .role = std::move(role),
            .abi = abi,
        });
        return package.callbacks.back().import_symbol;
    };

    auto add_retained_global = [&](
                                   std::size_t package_index,
                                   PackageImportGroup& package,
                                   llvm::GlobalVariable const& source,
                                   std::size_t size)
        -> std::expected<std::string, std::string> {
        if (source.getName().empty()) {
            return std::unexpected(
                "configuration relocation retained LLVM global has no symbol name");
        }
        for (auto const& global : package.retained_globals) {
            if (global.source_symbol == source.getName().str()) {
                if (global.size != size) {
                    return std::unexpected(
                        "configuration relocation retained LLVM global size is inconsistent");
                }
                return global.import_symbol;
            }
        }
        auto import_symbol = retained_global_import_symbol(
            package_index, package.retained_globals.size());
        package.retained_globals.push_back(RetainedGlobalImportPlan{
            .source_symbol = source.getName().str(),
            .import_symbol = import_symbol,
            .size = size,
        });
        return import_symbol;
    };

    plan.primitive_callbacks.reserve(analysis.primitives.size());
    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const& primitive = analysis.primitives[i];
        auto const& implementation = *primitive.implementation;
        auto package_index = find_package_index(implementation.package_module);
        if (!package_index) return std::unexpected(std::move(package_index.error()));
        auto& package = package_group(*package_index);

        PrimitiveCallbackPlan callbacks{
            .bundle = primitive.bundle.node_bundle,
        };
        if (implementation.tick_block) {
            callbacks.tick_block = add_callback(
                package,
                implementation.tick_block->getName().str(),
                primitive_callback_import_symbol(i, "tick_block"),
                "tick_block",
                CallbackImportAbi::block);
            callbacks.tick_block_batch = add_callback(
                package,
                implementation.tick_block_batch->getName().str(),
                primitive_callback_import_symbol(i, "tick_block_batch"),
                "tick_block_batch",
                CallbackImportAbi::batch);
            if (primitive.bundle.block_skippable) {
                callbacks.skip_block = add_callback(
                    package,
                    implementation.skip_block->getName().str(),
                    primitive_callback_import_symbol(i, "skip_block"),
                    "skip_block",
                    CallbackImportAbi::block);
                callbacks.skip_block_batch = add_callback(
                    package,
                    implementation.skip_block_batch->getName().str(),
                    primitive_callback_import_symbol(i, "skip_block_batch"),
                    "skip_block_batch",
                    CallbackImportAbi::batch);
            }
            if (implementation.combine_sequential_input_values) {
                callbacks.combine_sequential_input_values = add_callback(
                    package,
                    implementation.combine_sequential_input_values->getName().str(),
                    primitive_callback_import_symbol(
                        i, "combine_sequential_input_values"),
                    "combine_sequential_input_values",
                    CallbackImportAbi::sequential_combine);
            }
        }

        auto const bundle = primitive.bundle.node_bundle;
        auto const background_node = bundle < background.bundle_to_background_node.size()
            ? background.bundle_to_background_node[bundle]
            : std::optional<BackgroundNodeIndex>{};
        if (background_node) {
            if (*background_node >= background.nodes.size()) {
                return std::unexpected(
                    "GraphJit background callback import references a missing background node");
            }
            auto const& node = background.nodes[*background_node];
            if (node.authored_tock_execution) {
                if (!implementation.tock_coverage
                    || !implementation.propagate_forward_coverage) {
                    return std::unexpected(
                        "GraphJit authored Tock node has incomplete background callbacks");
                }
                callbacks.tock_coverage = add_callback(
                    package,
                    implementation.tock_coverage->getName().str(),
                    primitive_callback_import_symbol(i, "tock_coverage"),
                    "tock_coverage",
                    CallbackImportAbi::background);
                callbacks.tock_coverage_batch = add_callback(
                    package,
                    implementation.tock_coverage_batch->getName().str(),
                    primitive_callback_import_symbol(i, "tock_coverage_batch"),
                    "tock_coverage_batch",
                    CallbackImportAbi::batch);
                callbacks.propagate_forward_coverage = add_callback(
                    package,
                    implementation.propagate_forward_coverage->getName().str(),
                    primitive_callback_import_symbol(
                        i, "propagate_forward_coverage"),
                    "propagate_forward_coverage",
                    CallbackImportAbi::background);
                callbacks.propagate_forward_coverage_batch = add_callback(
                    package,
                    implementation.propagate_forward_coverage_batch->getName().str(),
                    primitive_callback_import_symbol(
                        i, "propagate_forward_coverage_batch"),
                    "propagate_forward_coverage_batch",
                    CallbackImportAbi::batch);
                if (node.accumulators.input_requirement_count != 0) {
                    if (!implementation.propagate_reverse_coverage) {
                        return std::unexpected(
                            "GraphJit authored Tock node with background inputs has no reverse callback");
                    }
                    callbacks.propagate_reverse_coverage = add_callback(
                        package,
                        implementation.propagate_reverse_coverage->getName().str(),
                        primitive_callback_import_symbol(
                            i, "propagate_reverse_coverage"),
                        "propagate_reverse_coverage",
                        CallbackImportAbi::background);
                    callbacks.propagate_reverse_coverage_batch = add_callback(
                        package,
                        implementation.propagate_reverse_coverage_batch->getName().str(),
                        primitive_callback_import_symbol(
                            i, "propagate_reverse_coverage_batch"),
                        "propagate_reverse_coverage_batch",
                        CallbackImportAbi::batch);
                }
            }
        }
        plan.primitive_callbacks.push_back(std::move(callbacks));
    }

    for (auto const& relocation : input.config_relocations) {
        if (!relocation.relocation) {
            return std::unexpected(
                "GraphJit lowering received an empty configuration relocation record");
        }
        if (!relocation.relocation->retained_global_index) {
            if (relocation.revision || relocation.retained_global) {
                return std::unexpected(
                    "explicit-null configuration relocation unexpectedly names a retained global");
            }
            continue;
        }
        if (!relocation.revision || !relocation.retained_global) {
            return std::unexpected(
                "non-null configuration relocation has no retained package LLVM global");
        }
        auto const index = *relocation.relocation->retained_global_index;
        if (index >= relocation.revision->retained_globals.size()) {
            return std::unexpected(
                "configuration relocation retained-global index is out of range at lowering");
        }
        auto const& accepted = relocation.revision->retained_globals[index];
        if (accepted.index != index || accepted.size == 0
            || relocation.relocation->addend >= accepted.size) {
            return std::unexpected(
                "configuration relocation retained-global metadata is invalid at lowering");
        }
        auto package_index = find_package_index(relocation.retained_global->getParent());
        if (!package_index) return std::unexpected(std::move(package_index.error()));
        auto const& package_input = input.packages[*package_index];
        if (package_input.revision.get() != relocation.revision.get()) {
            return std::unexpected(
                "configuration relocation retained global belongs to the wrong package revision");
        }
        if (std::ranges::find(
                package_input.retained_globals, relocation.retained_global)
            == package_input.retained_globals.end()) {
            return std::unexpected(
                "configuration relocation retained global is absent from package metadata");
        }
        if (!relocation.retained_global->isConstant()
            || relocation.retained_global->isDeclaration()
            || !relocation.retained_global->hasInitializer()) {
            return std::unexpected(
                "configuration relocation target is not an immutable defined LLVM global");
        }

        auto& package = package_group(*package_index);
        auto imported = add_retained_global(
            *package_index, package, *relocation.retained_global, accepted.size);
        if (!imported) return std::unexpected(std::move(imported.error()));
    }
    return plan;
}

std::expected<ConfigurationPlan, std::string> plan_node_configurations(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    PackageImportPlan const& imports)
{
    ConfigurationPlan plan;
    if (analysis.empty) {
        if (!input.config_relocations.empty()) {
            return std::unexpected(
                "empty GraphJit graph unexpectedly contains configuration relocations");
        }
        return plan;
    }

    auto imported_global_symbol = [&](ConfigRelocation const& relocation)
        -> std::expected<std::string, std::string> {
        if (!relocation.relocation->retained_global_index) return std::string{};
        if (!relocation.retained_global) {
            return std::unexpected(
                "non-null configuration relocation lost its retained LLVM global");
        }
        for (auto const& package : imports.packages) {
            if (package.package_index >= input.packages.size()) continue;
            auto const& package_input = input.packages[package.package_index];
            if (package_input.module.get() != relocation.retained_global->getParent()) continue;
            for (auto const& global : package.retained_globals) {
                if (global.source_symbol == relocation.retained_global->getName().str()) {
                    if (relocation.relocation->addend >= global.size) {
                        return std::unexpected(
                            "configuration relocation addend lies outside retained LLVM global");
                    }
                    return global.import_symbol;
                }
            }
        }
        return std::unexpected(
            "configuration relocation has no planned retained-global import");
    };

    plan.nodes.reserve(analysis.primitives.size());
    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const& primitive = analysis.primitives[i];
        auto const* first = static_cast<std::byte const*>(
            primitive.implementation->node_data);
        NodeConfigurationPlan node{
            .bytes = std::vector<std::byte>(
                first,
                first + primitive.bundle.node_size),
            .alignment = primitive.bundle.node_alignment,
            .tick_context_template = ReflectedNodeTickContext{
                .sample_rate = input.specialization.sample_rate,
                .scc_feedback_latency = [&] {
                    auto const bundle = primitive.bundle.node_bundle;
                    if (bundle >= connections.schedule.bundle_to_region.size()
                        || !connections.schedule.bundle_to_region[bundle]) {
                        return std::size_t{0};
                    }
                    auto const region =
                        *connections.schedule.bundle_to_region[bundle];
                    return region < connections.schedule.regions.size()
                        ? connections.schedule.regions[region].scc_feedback_latency
                        : std::size_t{0};
                }(),
            },
            .node_global_symbol = node_config_global_symbol(i),
            .tick_context_global_symbol = tick_context_global_symbol(i),
        };

        for (auto const& relocation : input.config_relocations) {
            if (!relocation.relocation
                || relocation.node_bundle != primitive.bundle.node_bundle) {
                continue;
            }
            auto const offset = relocation.relocation->byte_offset;
            if (offset > node.bytes.size()
                || sizeof(void*) > node.bytes.size() - offset) {
                return std::unexpected(
                    "configuration relocation lies outside planned node bytes");
            }
            auto imported = imported_global_symbol(relocation);
            if (!imported) return std::unexpected(std::move(imported.error()));
            if (imported->empty() && relocation.relocation->addend != 0) {
                return std::unexpected(
                    "explicit-null configuration relocation has a non-zero addend");
            }

            std::fill_n(node.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                sizeof(void*), std::byte{0});
            node.relocations.push_back(NodeConfigurationRelocationPlan{
                .byte_offset = offset,
                .addend = relocation.relocation->addend,
                .retained_global_symbol = std::move(*imported),
            });
        }
        std::ranges::sort(node.relocations, {}, &NodeConfigurationRelocationPlan::byte_offset);
        for (std::size_t relocation_index = 1;
             relocation_index < node.relocations.size(); ++relocation_index) {
            auto const previous_end = node.relocations[relocation_index - 1].byte_offset
                + sizeof(void*);
            if (previous_end > node.relocations[relocation_index].byte_offset) {
                return std::unexpected(
                    "node configuration contains overlapping pointer relocations");
            }
        }
        plan.nodes.push_back(std::move(node));
    }

    for (auto const& relocation : input.config_relocations) {
        auto const found = std::ranges::find_if(
            analysis.primitives,
            [&](PrimitiveAnalysis const& primitive) {
                return primitive.bundle.node_bundle == relocation.node_bundle;
            });
        if (found == analysis.primitives.end()) {
            return std::unexpected(
                "configuration relocation does not belong to an analyzed concrete primitive");
        }
    }
    return plan;
}


std::expected<SamplePortBindingPlan, std::string> plan_sample_ports(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    ResolvedPortRealization const& realization,
    RealtimeStorageCostModel const& cost_model)
{
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
        if (analysis.primitives[i].implementation->tick_block == nullptr) {
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
            auto const config = input.graph.node_bundles.resolve_sample_output(
                {bundle, PortKind::sample, port}).config;
            plan.primitives[i].outputs[port].realtime =
                is_tick(config);
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
        if (!group.storage_plan) {
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

    for (std::size_t connection_index = 0;
         connection_index < connections.sample_connections.size();
         ++connection_index) {
        auto const& connection = connections.sample_connections[connection_index];
        // Background evaluation delivers a connection with no Tick ->
        // Sequential channel. A Sequential target then receives its block
        // through its TickSequentialBindingPlan slot and is planned below as
        // invocation_supplied; a Random Access target reads its
        // TickRandomAccessBindingPlan view.
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
        auto const target = input.graph.node_bundles
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
            auto const source = input.graph.node_bundles
                .resolve_sample_output(source_port).config;
            auto const& group = connections.sample_producer_groups[*group_index];
            if (!is_tick(source)
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
            auto const source = input.graph.node_bundles
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
                        if (target_channel >= populated_targets.size()
                            || populated_targets[target_channel]) {
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
    };
    std::vector<DisconnectedSampleInput> disconnected_inputs;
    std::vector<DisconnectedSampleOutput> disconnected_outputs;
    std::vector<SampleConstantInputRequest> constant_input_requests;
    std::vector<SampleSinkStorageRequest> sink_requests;
    std::vector<TimelineSampleInput> timeline_inputs;
    std::vector<SampleInputTimelineRequest> timeline_requests;

    auto initialized_input_value = [&](
        std::size_t primitive_index,
        std::size_t port,
        iv::details::SequentialInputInitialization initialization)
        -> std::expected<std::vector<std::byte>, std::string> {
        auto const& geometry =
            plan.primitives[primitive_index].inputs[port].geometry;
        auto const* implementation =
            analysis.primitives[primitive_index].implementation;
        if (!implementation
            || !implementation->host_initialize_sequential_input) {
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
            || !implementation->host_initialize_sequential_input(
                port,
                geometry.value_type,
                initialization,
                static_cast<std::byte*>(aligned),
                geometry.value_size_bytes,
                geometry.elements_per_value)) {
            return std::unexpected(
                "GraphJit continuous input initializer rejected its resolved geometry");
        }
        std::vector<std::byte> value(geometry.value_size_bytes);
        std::memcpy(value.data(), aligned, geometry.value_size_bytes);
        return value;
    };
    auto request_input_timeline = [&](
        std::size_t primitive_index,
        std::size_t port,
        std::size_t execution_position)
        -> std::expected<void, std::string> {
        auto const& binding = plan.primitives[primitive_index].inputs[port];
        auto neutral = initialized_input_value(
            primitive_index,
            port,
            iv::details::SequentialInputInitialization::neutral);
        if (!neutral) return std::unexpected(std::move(neutral.error()));
        auto const bundle =
            analysis.primitives[primitive_index].bundle.node_bundle;
        timeline_inputs.push_back({.primitive = primitive_index, .port = port});
        timeline_requests.push_back(SampleInputTimelineRequest{
            .channel_layout = binding.channel_layout,
            .geometry = binding.geometry,
            .absolute_pace_exponent = binding.absolute_pace_exponent,
            .history = binding.history,
            .neutral_value = std::move(*neutral),
            .execution_position = execution_position,
            .migration_identity = "graphjit.sample.input_timeline:"
                + std::to_string(bundle) + "." + std::to_string(port),
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
        if (analysis.primitives[primitive_index].implementation->tick_block
            == nullptr) {
            continue;
        }
        auto const bundle = analysis.primitives[primitive_index].bundle.node_bundle;
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

            auto const config = input.graph.node_bundles.resolve_sample_input(
                NodeBundlePortId{bundle, PortKind::sample, port}).config;
            auto invocation_supplied = external_tick_sequential_slot(
                connections, bundle, PortKind::sample, port);
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
            // its TickRandomAccessBindingPlan slot, whether the input is
            // connected or disconnected (empty coverage).
            if (!is_sequential(config)) {
                auto& binding = plan.primitives[primitive_index].inputs[port];
                binding.random_access = true;
                binding.channel_layout = config.channel_layout;
                continue;
            }
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
            constant_input_requests.push_back(SampleConstantInputRequest{
                .channel_layout = config.channel_layout,
                .geometry = input_plan.geometry,
                .absolute_pace_exponent = input_plan.absolute_pace_exponent,
                .default_value = std::move(*default_value),
                .execution_position = *execution_position,
            });
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

            auto const config = input.graph.node_bundles.resolve_sample_output(
                NodeBundlePortId{bundle, PortKind::sample, port}).config;
            if (!is_tick(config)) continue;
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
                    + std::to_string(bundle) + "." + std::to_string(port),
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

    auto storage_connections = connections;
    for (std::size_t connection_index = 0;
         connection_index < storage_connections.sample_connections.size();
         ++connection_index) {
        auto& connection =
            storage_connections.sample_connections[connection_index];
        if (has_realtime_delivery(connection)
            && !is_entirely_realtime_delivery(connection)) {
            auto partitioned = realtime_sample_partition(connection);
            if (!partitioned) {
                return std::unexpected(std::move(partitioned.error()));
            }
            connection = std::move(*partitioned);
        }
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
        auto const target_primitive = primitive_index_for_bundle(
            connection.target_port.node_bundle_handle);
        if (!target_primitive) {
            return std::unexpected(
                "GraphJit detached continuous input has no concrete target initializer");
        }
        auto const* implementation =
            analysis.primitives[*target_primitive].implementation;
        if (!implementation
            || !implementation->host_initialize_sequential_input) {
            return std::unexpected(
                "GraphJit detached continuous input has no type-owned neutral initializer");
        }
        auto const& geometry = connection.geometry;
        std::vector<std::byte> unaligned(
            geometry.value_size_bytes + geometry.element_alignment - 1);
        void* aligned = unaligned.data();
        auto space = unaligned.size();
        if (!std::align(
                geometry.element_alignment,
                geometry.value_size_bytes,
                aligned,
                space)
            || !implementation->host_initialize_sequential_input(
                connection.target_port.port_index,
                geometry.value_type,
                iv::details::SequentialInputInitialization::neutral,
                static_cast<std::byte*>(aligned),
                geometry.value_size_bytes,
                geometry.elements_per_value)) {
            return std::unexpected(
                "GraphJit detached continuous neutral initializer rejected its resolved geometry");
        }
        for (auto const initial_value : initial_values) {
            if (*initial_value) continue;
            initial_value->emplace(geometry.value_size_bytes);
            std::memcpy(
                (*initial_value)->data(),
                aligned,
                geometry.value_size_bytes);
        }
    }

    auto storage = build_sample_storage_plan(
        storage_connections,
        input.specialization.block_size,
        sink_requests,
        constant_input_requests,
        cost_model,
        timeline_requests);
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

    for (auto const& target : validated_targets) {
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
        // The callback reads only the timeline; a mixed input's connection
        // storage becomes the realtime operand of its per-block combination.
        if (binding.combines_invocation_supplied) {
            binding.realtime_operand_channels = std::move(binding.channels);
        }
        binding.channels.clear();
        binding.read_latency = 0;
        auto const channels = channel_count(binding.channel_layout);
        binding.channels.reserve(channels);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            binding.channels.push_back(PrimitiveSampleInputChannelBindingPlan{
                .storage = representation_index,
                .representation_channel = channel,
                .frame_delay = 0,
            });
        }
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
                    if (binding.random_access) {
                        return binding.channels.empty() && channels != 0;
                    }
                    // A slot-fed input without history reads the slot
                    // directly; with history it reads its timeline.
                    if (binding.invocation_supplied) {
                        return channels != 0
                            && binding.realtime_operand_channels.empty()
                            && (binding.history == 0
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

    std::size_t shared_scratch_size = 0;
    std::size_t shared_scratch_alignment = 1;
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
            // limit, while the generated root can expose at most one kernel
            // block here (and may split that block further for an SCC).
            auto const maximum_invocation_extent = std::min(
                input.specialization.block_size,
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
            shared_scratch_alignment = std::max(
                shared_scratch_alignment,
                input_binding.geometry.element_alignment);
        }
        shared_scratch_size = std::max(
            shared_scratch_size, primitive_scratch_size);
    }

    if (shared_scratch_size != 0) {
        auto const scratch_base = align_up(
            plan.storage.transient_arena_size,
            shared_scratch_alignment);
        if (!scratch_base
            || shared_scratch_size
                > std::numeric_limits<std::size_t>::max() - *scratch_base) {
            return std::unexpected(
                "GraphJit continuous input scratch arena overflows size_t");
        }
        for (auto& primitive : plan.primitives) {
            for (auto& input_binding : primitive.inputs) {
                if (!input_binding.has_contiguous_scratch) continue;
                input_binding.contiguous_scratch_offset += *scratch_base;
            }
        }
        plan.contiguous_input_scratch_size = shared_scratch_size;
        plan.contiguous_input_scratch_alignment = shared_scratch_alignment;
        plan.storage.transient_arena_size = *scratch_base + shared_scratch_size;
        plan.storage.transient_arena_alignment = std::max(
            plan.storage.transient_arena_alignment,
            shared_scratch_alignment);
    }

    return plan;
}

struct EventGroupPlanningDecision {
    EventConnectionStoragePlan storage{};
};

struct EventFeedbackPlanningDecision {
    EventConnectionStorageRequirements requirements{};
    EventConnectionStoragePlan storage{};
};

struct EventPlanningDecisions {
    std::vector<EventGroupPlanningDecision> producer_groups{};
    std::vector<EventFeedbackPlanningDecision> feedback{};
};

std::expected<EventPortBindingPlan, std::string> plan_event_ports_once(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    ResolvedPortRealization const& realization,
    RealtimeStorageCostModel const& cost_model,
    EventPlanningDecisions const* decisions)
{
    EventPortBindingPlan plan;
    if (analysis.empty) return plan;

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

    plan.primitives.resize(analysis.primitives.size());
    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const* node = planned_node_for_bundle(
            analysis.primitives[i].bundle.node_bundle);
        if (!node) {
            return std::unexpected(
                "GraphJit event planning lost concrete-node port metadata");
        }
        if (analysis.primitives[i].implementation->tick_block == nullptr) {
            // Tock ports use the caller-supplied background batch bindings.
            continue;
        }
        plan.primitives[i].inputs.resize(node->event_input_count);
        plan.primitives[i].outputs.resize(node->event_output_count);
        auto const bundle = analysis.primitives[i].bundle.node_bundle;
        for (std::size_t port = 0; port < node->event_output_count; ++port) {
            auto const config = input.graph.node_bundles.resolve_event_output(
                {bundle, PortKind::event, port}).config;
            plan.primitives[i].outputs[port].realtime =
                is_tick(config);
        }
    }

    plan.producer_group_storage_plans.resize(
        connections.event_producer_groups.size());
    plan.producer_group_representations.resize(
        connections.event_producer_groups.size());
    plan.producer_group_source_representations.resize(
        connections.event_producer_groups.size());
    std::vector<EventConnectionStorageRequirements> group_cost_requirements(
        connections.event_producer_groups.size());
    if (decisions
        && decisions->producer_groups.size()
            != connections.event_producer_groups.size()) {
        return std::unexpected(
            "GraphJit event costing decisions disagree with producer groups");
    }

    auto align_up = [](std::size_t value, std::size_t alignment)
        -> std::optional<std::size_t> {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return std::nullopt;
        }
        auto const mask = alignment - 1;
        if (value > std::numeric_limits<std::size_t>::max() - mask) {
            return std::nullopt;
        }
        return (value + mask) & ~mask;
    };
    auto append_representation = [&] (
        std::size_t group_index,
        EventTypeId type,
        std::size_t capacity,
        bool producer_telemetry = false,
        bool persistent = false,
        std::string migration_identity = {},
        bool persistent_ring = false,
        bool source_indices = false)
        -> std::expected<std::size_t, std::string> {
        if (persistent_ring && !persistent) {
            return std::unexpected(
                "GraphJit persistent event ring must use persistent storage");
        }
        std::size_t const read_index_relative = 0;
        std::size_t const write_index_relative = persistent_ring
            ? sizeof(std::size_t)
            : 0;
        std::size_t header_end = persistent_ring
            ? 2 * sizeof(std::size_t)
            : sizeof(std::size_t);
        std::size_t alignment = std::max(
            alignof(std::size_t), alignof(TimedEvent));
        std::size_t timeline_initialized_relative = 0;
        std::size_t expected_next_time_relative = 0;
        if (persistent) {
            timeline_initialized_relative = header_end;
            auto const expected_relative = align_up(
                header_end + sizeof(std::uint8_t), alignof(GlobalIndex));
            if (!expected_relative
                || *expected_relative
                    > std::numeric_limits<std::size_t>::max()
                        - sizeof(GlobalIndex)) {
                return std::unexpected(
                    "GraphJit event timeline header size overflows size_t");
            }
            expected_next_time_relative = *expected_relative;
            header_end = expected_next_time_relative + sizeof(GlobalIndex);
            alignment = std::max(alignment, alignof(GlobalIndex));
        }
        auto const events_relative = align_up(header_end, alignof(TimedEvent));
        if (!events_relative
            || capacity > (std::numeric_limits<std::size_t>::max()
                    - *events_relative) / sizeof(TimedEvent)) {
            return std::unexpected(
                "GraphJit event storage size overflows size_t");
        }
        auto const events_end = *events_relative + capacity * sizeof(TimedEvent);
        std::size_t source_indices_relative = 0;
        auto size_bytes = events_end;
        if (source_indices) {
            auto const indices_relative = align_up(
                events_end, alignof(std::size_t));
            if (!indices_relative
                || capacity > (std::numeric_limits<std::size_t>::max()
                        - *indices_relative) / sizeof(std::size_t)) {
                return std::unexpected(
                    "GraphJit event source-index storage size overflows size_t");
            }
            source_indices_relative = *indices_relative;
            size_bytes = source_indices_relative
                + capacity * sizeof(std::size_t);
            alignment = std::max(alignment, alignof(std::size_t));
        }
        auto const representation_index = plan.ports.size();
        plan.ports.push_back(EventRepresentationPlan{
            .producer_group_index = group_index,
            .type = type,
            .event_capacity = capacity,
            .persistent = persistent,
            .persistent_ring = persistent_ring,
            .migration_identity = std::move(migration_identity),
            .has_producer_overflow_counter = producer_telemetry,
            .count_relative_offset = 0,
            .read_index_relative_offset = read_index_relative,
            .write_index_relative_offset = write_index_relative,
            .has_sequential_timeline = persistent,
            .timeline_initialized_relative_offset =
                timeline_initialized_relative,
            .expected_next_time_relative_offset =
                expected_next_time_relative,
            .events_relative_offset = *events_relative,
            .has_source_indices = source_indices,
            .source_indices_relative_offset = source_indices_relative,
            .size_bytes = size_bytes,
            .alignment = alignment,
        });
        return representation_index;
    };

    // One read-only zero-count sequence per event type serves every
    // disconnected Sequential input of that type.
    std::vector<std::pair<EventTypeId, std::size_t>>
        immutable_empty_representations;
    auto immutable_empty_representation = [&](EventTypeId type)
        -> std::expected<std::size_t, std::string> {
        auto const found = std::ranges::find(
            immutable_empty_representations,
            type,
            &std::pair<EventTypeId, std::size_t>::first);
        if (found != immutable_empty_representations.end()) {
            return found->second;
        }
        auto representation = append_representation(
            connections.event_producer_groups.size(), type, 0);
        if (!representation) return representation;
        plan.ports[*representation].immutable_empty = true;
        immutable_empty_representations.emplace_back(type, *representation);
        return representation;
    };

    auto rounded_event_capacity = [](std::size_t required)
        -> std::expected<std::size_t, std::string> {
        if (required == 0) return 0;
        constexpr auto highest_power_of_two = std::size_t{1}
            << (std::numeric_limits<std::size_t>::digits - 1);
        if (required > highest_power_of_two) {
            return std::unexpected(
                "GraphJit event aggregate exceeds representable static capacity");
        }
        return next_power_of_2(required);
    };
    auto event_group_identity = [](EventProducerGroupPlan const& group) {
        std::string identity =
            "graphjit.event:"
            + std::to_string(static_cast<unsigned>(group.source_type));
        for (std::size_t source_index = 0;
             source_index < group.sources.size(); ++source_index) {
            auto const source = group.sources[source_index];
            identity += ':' + std::to_string(source.bundle) + '.'
                + std::to_string(source.port);
            if (source_index < group.source_conversions.size()) {
                auto const& conversion = group.source_conversions[source_index];
                identity += "[" + std::to_string(
                    static_cast<unsigned>(conversion.source_type));
                for (std::size_t step = 0;
                     step < conversion.step_count; ++step) {
                    identity += "," + std::to_string(
                        static_cast<unsigned>(conversion.steps[step]));
                }
                identity += ">" + std::to_string(
                    static_cast<unsigned>(conversion.target_type)) + "]";
            }
        }
        return identity;
    };

    auto region_for_bundle = [&](NodeBundleHandle bundle)
        -> SccRegionPlan const* {
        if (bundle >= connections.schedule.bundle_to_region.size()
            || !connections.schedule.bundle_to_region[bundle]) {
            return nullptr;
        }
        auto const region = *connections.schedule.bundle_to_region[bundle];
        return region < connections.schedule.regions.size()
            ? &connections.schedule.regions[region]
            : nullptr;
    };
    auto region_index_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<std::size_t> {
        if (bundle >= connections.schedule.bundle_to_region.size()) {
            return std::nullopt;
        }
        return connections.schedule.bundle_to_region[bundle];
    };
    auto primitive_scope = [](std::size_t position, EventOperationPhase phase) {
        return EventOperationScope{
            .kind = EventOperationScopeKind::primitive,
            .phase = phase,
            .index = position,
        };
    };
    auto region_scope = [](std::size_t region, EventOperationPhase phase) {
        return EventOperationScope{
            .kind = EventOperationScopeKind::region,
            .phase = phase,
            .index = region,
        };
    };
    auto materialization_scope_for_target = [&] (
        std::span<EventOutputPortId const> sources,
        std::size_t producer_position,
        EventInputPortId target) -> std::expected<EventOperationScope, std::string> {
        std::optional<std::size_t> source_cyclic_region;
        for (auto const source : sources) {
            auto const region_index = region_index_for_bundle(source.bundle);
            if (!region_index
                || *region_index >= connections.schedule.regions.size()
                || !connections.schedule.regions[*region_index].cyclic) {
                source_cyclic_region.reset();
                break;
            }
            if (source_cyclic_region
                && *source_cyclic_region != *region_index) {
                source_cyclic_region.reset();
                break;
            }
            source_cyclic_region = *region_index;
        }
        auto const target_region = region_index_for_bundle(target.bundle);
        if (source_cyclic_region) {
            if (target_region && *target_region == *source_cyclic_region) {
                return primitive_scope(
                    producer_position, EventOperationPhase::after);
            }
            return region_scope(
                *source_cyclic_region, EventOperationPhase::after);
        }
        if (target_region
            && *target_region < connections.schedule.regions.size()
            && connections.schedule.regions[*target_region].cyclic) {
            return region_scope(*target_region, EventOperationPhase::before);
        }
        return primitive_scope(
            producer_position, EventOperationPhase::after);
    };
    struct EventMaterializationCostBound {
        std::size_t output_writes = 0;
        std::size_t source_reads = 0;
    };
    auto materialization_event_bound = [&] (
        double max_events_per_index,
        std::size_t history_samples,
        EventOperationScope scope,
        bool select_invocation_window,
        std::size_t complete_source_bound)
        -> std::expected<EventMaterializationCostBound, std::string> {
        if (!select_invocation_window) {
            return EventMaterializationCostBound{
                .output_writes = complete_source_bound,
                .source_reads = complete_source_bound,
            };
        }

        auto block_samples = input.specialization.block_size;
        std::size_t execution_count = 1;
        if (scope.kind == EventOperationScopeKind::primitive) {
            for (auto const& node : connections.nodes) {
                if (node.bundle
                        >= connections.schedule.bundle_execution_position.size()
                    || !connections.schedule.bundle_execution_position[
                        node.bundle]
                    || *connections.schedule.bundle_execution_position[
                        node.bundle] != scope.index) {
                    continue;
                }
                auto const region = region_index_for_bundle(node.bundle);
                if (region && *region < connections.schedule.regions.size()
                    && connections.schedule.regions[*region].cyclic) {
                    block_samples = connections.schedule.regions[*region]
                        .maximum_block_size;
                    if (block_samples == 0) {
                        return std::unexpected(
                            "GraphJit event materialization has no SCC quantum");
                    }
                    execution_count = input.specialization.block_size
                        / block_samples
                        + (input.specialization.block_size % block_samples != 0);
                }
                break;
            }
        }
        if (history_samples
            > std::numeric_limits<std::size_t>::max() - block_samples) {
            return std::unexpected(
                "GraphJit event materialization window overflows size_t");
        }
        std::size_t output_writes = 0;
        auto remaining = input.specialization.block_size;
        for (std::size_t execution = 0;
             execution < execution_count; ++execution) {
            auto const current_block = execution_count == 1
                ? block_samples
                : std::min(block_samples, remaining);
            auto const count = event_count_for_sample_span(
                max_events_per_index, current_block + history_samples);
            if (!count) {
                return std::unexpected(
                    "GraphJit event materialization bound is not representable");
            }
            output_writes = iv::detail::saturating_add(
                output_writes, std::min(*count, complete_source_bound));
            remaining -= current_block;
        }
        return EventMaterializationCostBound{
            .output_writes = output_writes,
            .source_reads = iv::detail::saturating_multiply(
                complete_source_bound, execution_count),
        };
    };

    // A concrete Tick event output has one callback-facing raw sequence even
    // when several connection fan-ins consume it. Fan-in groups own only
    // derived converted/merged sequences.
    auto ensure_raw_event_output = [&] (EventOutputPortId output)
        -> std::expected<std::size_t, std::string> {
        auto const primitive = primitive_index_for_bundle(output.bundle);
        if (!primitive
            || output.port >= plan.primitives[*primitive].outputs.size()) {
            return std::unexpected(
                "GraphJit event source requires an internal concrete output");
        }
        auto& binding = plan.primitives[*primitive].outputs[output.port];
        if (!binding.realtime) {
            return std::unexpected(
                "GraphJit realtime event source is not a Tick output");
        }
        if (binding.storage) {
            return *binding.storage;
        }
        if (output.bundle
                >= connections.schedule.bundle_execution_position.size()
            || !connections.schedule.bundle_execution_position[output.bundle]) {
            return std::unexpected(
                "GraphJit event source has no schedule position");
        }

        NodeBundlePortId const port_id{
            output.bundle, PortKind::event, output.port};
        auto const config = input.graph.node_bundles
            .resolve_event_output(port_id).config;
        if (!is_tick(config)) {
            return std::unexpected(
                "GraphJit realtime event source disagrees with its declaration");
        }
        auto const& resolved_output = realization.event_output(
            output.bundle, output.port);
        auto const history = resolved_output.history;
        auto const latency = resolved_output.latency;
        if (history > std::numeric_limits<std::size_t>::max() - latency) {
            return std::unexpected(
                "GraphJit raw event output window overflows size_t");
        }
        auto const retained_window = history + latency;
        auto const region = region_index_for_bundle(output.bundle);
        auto const cyclic = region
            && *region < connections.schedule.regions.size()
            && connections.schedule.regions[*region].cyclic;
        auto callback_window = input.specialization.block_size;
        std::size_t aggregate_window = input.specialization.block_size;
        if (cyclic) {
            auto const quantum = connections.schedule.regions[*region]
                .maximum_block_size;
            if (quantum == 0) {
                return std::unexpected(
                    "GraphJit raw event output SCC has no execution quantum");
            }
            callback_window = std::min(
                input.specialization.block_size, quantum);
            auto const slice_count = input.specialization.block_size / quantum
                + (input.specialization.block_size % quantum != 0);
            if (retained_window != 0
                && slice_count
                    > (std::numeric_limits<std::size_t>::max()
                        - aggregate_window) / retained_window) {
                return std::unexpected(
                    "GraphJit raw event output aggregate window overflows size_t");
            }
            aggregate_window += slice_count * retained_window;
        } else {
            if (retained_window
                > std::numeric_limits<std::size_t>::max()
                    - aggregate_window) {
                return std::unexpected(
                    "GraphJit raw event output aggregate window overflows size_t");
            }
            aggregate_window += retained_window;
        }
        if (retained_window
            > std::numeric_limits<std::size_t>::max() - callback_window) {
            return std::unexpected(
                "GraphJit raw event callback window overflows size_t");
        }
        callback_window += retained_window;
        auto const current_events = event_count_for_sample_span(
            config.max_events_per_index, aggregate_window);
        auto const retained_events = event_count_for_sample_span(
            config.max_events_per_index, retained_window);
        auto const callback_current_events = event_count_for_sample_span(
            config.max_events_per_index, callback_window);
        auto const callback_events = event_sequence_capacity_for_sample_span(
            config.max_events_per_index, callback_window);
        if (!current_events || !retained_events || !callback_current_events
            || !callback_events
            || *current_events > std::numeric_limits<std::size_t>::max()
                    - *retained_events) {
            return std::unexpected(
                "GraphJit raw event output capacity is not representable");
        }
        // Retained storage must not be exposed to EventOutputPort because the
        // callback is allowed to author timestamps that overlap an earlier
        // invocation. Without history or latency, the callback sequence itself
        // is the raw EventOutputPortId representation. Cyclic fan-in operations
        // consume it after each primitive invocation and before the next slice
        // clears it.
        auto const retained_output = retained_window != 0;
        auto const canonical_current_events = retained_output
            ? *current_events
            : *callback_current_events;
        auto const requirements = EventConnectionStorageRequirements{
            .current_window_samples = retained_output
                ? input.specialization.block_size
                : callback_window,
            .retained_window_samples = retained_window,
            .current_event_capacity = canonical_current_events,
            .retained_event_capacity = *retained_events,
            .value_size_bytes = sizeof(TimedEvent),
            .operations = RealtimeStorageOperationCounts{
                .invariant_copied_values = retained_output
                    ? *current_events
                    : 0,
                .full_extra_copied_values = retained_output
                    ? 0
                    : *current_events,
                .full_ring_addressed_values = *current_events,
                .transient_extra_stack_values = retained_output
                    ? *callback_events
                    : 0,
                .carry_extra_stack_values = *callback_events,
                .full_extra_stack_values = *callback_events,
            },
        };
        auto const storage_plan = choose_event_connection_storage_plan(
            requirements, cost_model);
        auto const needs_callback_local = retained_output
            || storage_plan.kind != RealtimeBufferStorageKind::transient_stack;
        if (canonical_current_events
            > std::numeric_limits<std::size_t>::max() - *retained_events) {
            return std::unexpected(
                "GraphJit raw event output working capacity overflows size_t");
        }
        auto working_capacity = rounded_event_capacity(
            canonical_current_events + *retained_events);
        if (!working_capacity) {
            return std::unexpected(std::move(working_capacity.error()));
        }

        auto const owner = connections.event_producer_groups.size();
        auto const position = *connections.schedule.bundle_execution_position[
            output.bundle];
        auto const before_scope = cyclic
            ? region_scope(*region, EventOperationPhase::before)
            : primitive_scope(position, EventOperationPhase::before);
        auto const after_scope = cyclic
            ? region_scope(*region, EventOperationPhase::after)
            : primitive_scope(position, EventOperationPhase::after);
        auto const merge_scope = primitive_scope(
            position, EventOperationPhase::after);
        auto const identity_base =
            "graphjit.event.raw_output:"
            + std::to_string(output.bundle) + "."
            + std::to_string(output.port)
            + ":history=" + std::to_string(history)
            + ":latency=" + std::to_string(latency);
        std::expected<std::size_t, std::string> canonical =
            std::unexpected(std::string{"unselected canonical event storage"});
        switch (storage_plan.kind) {
        case RealtimeBufferStorageKind::transient_stack:
            canonical = append_representation(
                owner,
                config.type,
                *working_capacity,
                !needs_callback_local);
            if (canonical && needs_callback_local) {
                plan.sequence_resets.push_back(EventSequenceResetPlan{
                    .storage = *canonical,
                    .scope = before_scope,
                });
            }
            break;
        case RealtimeBufferStorageKind::stack_with_persistent_carry: {
            canonical = append_representation(
                owner, config.type, *working_capacity);
            if (!canonical) break;
            auto carry_capacity = event_sequence_capacity_for_sample_span(
                config.max_events_per_index, retained_window);
            if (!carry_capacity) {
                return std::unexpected(
                    "GraphJit raw event output carry capacity is not representable");
            }
            auto persistent = append_representation(
                owner,
                config.type,
                *carry_capacity,
                false,
                true,
                identity_base + ":kind=compact_carry:capacity="
                    + std::to_string(*carry_capacity));
            if (!persistent) {
                return std::unexpected(std::move(persistent.error()));
            }
            plan.carry_operations.push_back(EventCarryPlan{
                .working_representation = *canonical,
                .persistent_representation = *persistent,
                .restore_scope = before_scope,
                .commit_scope = after_scope,
                .retained_history_samples = history,
                .retained_latency_samples = latency,
            });
            break;
        }
        case RealtimeBufferStorageKind::full_node_storage:
            canonical = append_representation(
                owner,
                config.type,
                *working_capacity,
                false,
                true,
                identity_base + ":kind=persistent_ring:capacity="
                    + std::to_string(*working_capacity),
                true);
            if (canonical) {
                plan.persistent_rings.push_back(EventPersistentRingPlan{
                    .storage = *canonical,
                    .prune_scope = before_scope,
                    .retained_history_samples = history,
                });
            }
            break;
        }
        if (!canonical) {
            return std::unexpected(std::move(canonical.error()));
        }
        auto callback_storage = *canonical;
        if (needs_callback_local) {
            auto local = append_representation(
                owner, config.type, *callback_events, true);
            if (!local) {
                return std::unexpected(std::move(local.error()));
            }
            callback_storage = *local;
            plan.merges.push_back(EventMergePlan{
                .source_representations = {callback_storage},
                .target_representation = *canonical,
                .scope = merge_scope,
                .ordering_history_samples = history,
                .preserve_existing_target = true,
                .before_materialization = true,
            });
        }
        binding = PrimitiveEventOutputBindingPlan{
            .realtime = true,
            .storage = callback_storage,
            .canonical_storage = *canonical,
            .source_type = config.type,
            .history = history,
            .latency = latency,
            .append_existing = !needs_callback_local
                    && !cyclic
                    && analysis.primitives[*primitive]
                           .bundle.maximum_block_size
                        < input.specialization.block_size,
        };
        return callback_storage;
    };
    auto source_conversions_match = [] (
        EventProducerGroupPlan const& group,
        EventConnectionPlan const& connection) {
        if (group.source_conversions.size()
            != connection.source_plans.size()) return false;
        for (std::size_t i = 0; i < group.source_conversions.size(); ++i) {
            if (group.source_conversions[i]
                != connection.source_plans[i].conversion) return false;
        }
        return true;
    };
    auto realtime_sources_match = [] (
        EventProducerGroupPlan const& group,
        EventConnectionPlan const& connection) {
        std::vector<EventOutputPortId> sources;
        std::vector<EventConversionPlan> conversions;
        for (std::size_t source_index = 0;
             source_index < connection.source_plans.size();
             ++source_index) {
            auto const realtime = std::ranges::any_of(
                connection.deliveries,
                [&](EventDeliveryPlan const& delivery) {
                    return delivery.source_index == source_index
                        && uses_realtime_storage(delivery.mechanism);
                });
            if (!realtime) continue;
            sources.push_back(connection.source_plans[source_index].source);
            conversions.push_back(
                connection.source_plans[source_index].conversion);
        }
        if (sources != group.realtime_sources
            || sources.size() != conversions.size()) {
            return false;
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            auto const group_source = std::ranges::find(
                group.sources, sources[index]);
            if (group_source == group.sources.end()) return false;
            auto const group_index = static_cast<std::size_t>(
                std::distance(group.sources.begin(), group_source));
            if (group_index >= group.source_conversions.size()
                || group.source_conversions[group_index]
                    != conversions[index]) {
                return false;
            }
        }
        return true;
    };
    auto realtime_targets = [] (EventConnectionPlan const& connection) {
        std::vector<EventInputPortId> targets;
        for (auto const& delivery : connection.deliveries) {
            if (!uses_realtime_storage(delivery.mechanism)
                || delivery.target_index >= connection.target_plans.size()) {
                continue;
            }
            auto const target =
                connection.target_plans[delivery.target_index].target;
            if (!std::ranges::contains(targets, target)) {
                targets.push_back(target);
            }
        }
        return targets;
    };
    // Mixed delivery is a property of one target: the connection may also
    // deliver other targets, for example Random Access inputs, entirely in
    // the background.
    auto target_receives_background_delivery = [](
        EventConnectionPlan const& connection, EventInputPortId target) {
        return std::ranges::any_of(
            connection.deliveries,
            [&](EventDeliveryPlan const& delivery) {
                return delivery.target_index < connection.target_plans.size()
                    && connection.target_plans[delivery.target_index].target
                        == target
                    && !uses_realtime_storage(delivery.mechanism);
            });
    };
    // A slot-fed input with history, and every mixed input, owns a
    // persistent timeline ring. The root inserts each delivered window into
    // it before the callback, so its history is the input's own record of
    // earlier deliveries in Tick invocation order.
    auto append_input_timeline = [&] (
        PrimitiveEventInputBindingPlan& binding,
        EventInputPortId target,
        EventTypeId type,
        std::size_t history,
        std::size_t required_capacity)
        -> std::expected<void, std::string> {
        auto const position = target.bundle
                < connections.schedule.bundle_execution_position.size()
            ? connections.schedule.bundle_execution_position[target.bundle]
            : std::nullopt;
        if (!position) {
            return std::unexpected(
                "GraphJit event input timeline has no execution position");
        }
        auto const capacity = rounded_event_capacity(
            std::max<std::size_t>(required_capacity, 1));
        if (!capacity) return std::unexpected(std::move(capacity.error()));
        auto timeline = append_representation(
            connections.event_producer_groups.size(),
            type,
            *capacity,
            false,
            true,
            "graphjit.event.input_timeline:"
                + std::to_string(static_cast<unsigned>(type)) + ":"
                + std::to_string(target.bundle) + "."
                + std::to_string(target.port) + ":capacity="
                + std::to_string(*capacity),
            true);
        if (!timeline) return std::unexpected(std::move(timeline.error()));
        // Like a raw Tick output, a primitive sliced inside a cyclic region
        // prunes once before the region rather than before every slice.
        auto const region = region_index_for_bundle(target.bundle);
        auto const cyclic = region
            && *region < connections.schedule.regions.size()
            && connections.schedule.regions[*region].cyclic;
        plan.persistent_rings.push_back(EventPersistentRingPlan{
            .storage = *timeline,
            .prune_scope = cyclic
                ? region_scope(*region, EventOperationPhase::before)
                : primitive_scope(*position, EventOperationPhase::before),
            .retained_history_samples = history,
        });
        binding.storage = *timeline;
        binding.timeline_history_samples = history;
        return {};
    };
    auto configure_mixed_input = [&] (
        PrimitiveEventInputBindingPlan& binding,
        EventConnectionPlan const& connection,
        EventInputPortId target,
        std::size_t group_index,
        std::optional<std::size_t> fixed_realtime_source_index = {})
        -> std::expected<void, std::string> {
        auto const target_plan = std::ranges::find(
            connection.target_plans, target, &EventTargetPlan::target);
        if (target_plan == connection.target_plans.end()) {
            return std::unexpected(
                "GraphJit mixed event input lost its connection target");
        }
        if (!target_receives_background_delivery(connection, target)) {
            return {};
        }
        if (!binding.storage || *binding.storage >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit mixed event input has no realtime representation");
        }
        if (target_plan->history
            > std::numeric_limits<std::size_t>::max()
                - input.specialization.block_size) {
            return std::unexpected(
                "GraphJit mixed event input window is not representable");
        }
        // The slot carries only the current window; the timeline also
        // retains the background contribution's history.
        auto const window_capacity = event_sequence_capacity_for_sample_span(
            connection.max_events_per_index,
            input.specialization.block_size);
        auto const retained_capacity = event_sequence_capacity_for_sample_span(
            connection.max_events_per_index,
            input.specialization.block_size + target_plan->history);
        auto const operand_capacity =
            plan.ports[*binding.storage].event_capacity;
        if (!window_capacity || !retained_capacity
            || operand_capacity
                > std::numeric_limits<std::size_t>::max()
                    - *retained_capacity) {
            return std::unexpected(
                "GraphJit mixed event input capacity is not representable");
        }
        auto const capacity = rounded_event_capacity(
            operand_capacity + *window_capacity);
        if (!capacity) return std::unexpected(std::move(capacity.error()));
        auto combined = append_representation(
            group_index, connection.target_type, *capacity);
        if (!combined) {
            return std::unexpected(std::move(combined.error()));
        }
        binding.invocation_supplied = true;
        binding.combines_invocation_supplied = true;
        binding.realtime_operand_storage = binding.storage;
        binding.combined_storage = *combined;
        binding.realtime_semantic_source_index = fixed_realtime_source_index;
        return append_input_timeline(
            binding,
            target,
            connection.target_type,
            target_plan->history,
            operand_capacity + *retained_capacity);
    };

    for (std::size_t group_index = 0;
         group_index < connections.event_producer_groups.size();
         ++group_index) {
        auto const& group = connections.event_producer_groups[group_index];
        // A group without realtime connections is delivered entirely by
        // background evaluation. Its Tick sources, if any, receive raw output
        // sinks below, which persisted capture reads.
        if (!group.has_realtime_connections) continue;
        if (!group.storage_plan) {
            return std::unexpected(
                "GraphJit event planning has no internal realtime storage plan");
        }
        auto selected_group_plan = decisions
            ? decisions->producer_groups[group_index].storage
            : *group.storage_plan;
        auto storage_kind = selected_group_plan.kind;
        plan.producer_group_storage_plans[group_index] = selected_group_plan;
        auto compact_carry =
            storage_kind
            == RealtimeBufferStorageKind::stack_with_persistent_carry;
        auto persistent_ring =
            storage_kind == RealtimeBufferStorageKind::full_node_storage;
        auto retained_storage = compact_carry || persistent_ring;
        auto aggregate_sequence =
            group.requires_invocation_aggregate || retained_storage;

        plan.producer_group_source_representations[group_index].resize(
            group.sources.size());

        if (group.source_conversions.size() != group.sources.size()) {
            return std::unexpected(
                "GraphJit event producer group lost its source conversions");
        }

        if (group.realtime_sources.size() > 1) {
            std::optional<std::size_t> cyclic_source_region;
            bool has_acyclic_source = false;
            bool incompatible_cyclic_sources = false;
            for (auto const source : group.realtime_sources) {
                auto const region_index = region_index_for_bundle(source.bundle);
                if (!region_index
                    || *region_index >= connections.schedule.regions.size()
                    || !connections.schedule.regions[*region_index].cyclic) {
                    has_acyclic_source = true;
                    continue;
                }
                if (cyclic_source_region
                    && *cyclic_source_region != *region_index) {
                    incompatible_cyclic_sources = true;
                }
                cyclic_source_region = *region_index;
            }
            auto const staged_fan_in = incompatible_cyclic_sources
                || (cyclic_source_region && has_acyclic_source);

            // Producer streams are contractually time-sorted, but independent
            // producers cannot append into one sequence without disturbing
            // global order. Each EventOutputPortId therefore keeps its raw
            // callback sequence. Lowering converts those sequences as needed
            // and merges them into a separate fan-in sequence.
            struct ValidatedSource {
                EventOutputPortId id{};
                std::size_t semantic_source_index = 0;
                std::size_t primitive = 0;
                std::size_t execution_position = 0;
                EventOutputConfig config{};
                EventConversionPlan conversion{};
                std::size_t raw_representation = 0;
                std::size_t capacity = 0;
                std::size_t aggregate_event_bound = 0;
                std::optional<std::size_t> cyclic_region{};
            };
            std::vector<ValidatedSource> validated_sources;
            validated_sources.reserve(group.realtime_sources.size());
            std::size_t total_local_capacity = 0;
            std::size_t total_invocation_local_capacity = 0;
            std::size_t producer_execution_position = 0;
            double observed_rate = 0.0;

            for (std::size_t group_source_index = 0;
                 group_source_index < group.sources.size();
                 ++group_source_index) {
                auto const source_id = group.sources[group_source_index];
                if (!std::ranges::contains(
                        group.realtime_sources, source_id)) {
                    continue;
                }
                auto const& source_conversion =
                    group.source_conversions[group_source_index];
                auto const source_primitive = primitive_index_for_bundle(
                    source_id.bundle);
                if (!source_primitive
                    || source_id.port
                        >= plan.primitives[*source_primitive].outputs.size()) {
                    return std::unexpected(
                        "GraphJit event fan-in requires internal concrete producers");
                }
                if (source_id.bundle
                        >= connections.schedule.bundle_execution_position.size()
                    || !connections.schedule.bundle_execution_position[
                        source_id.bundle]) {
                    return std::unexpected(
                        "GraphJit event fan-in producer is absent from the execution schedule");
                }
                NodeBundlePortId const source_port{
                    source_id.bundle, PortKind::event, source_id.port};
                auto const source = input.graph.node_bundles
                    .resolve_event_output(source_port).config;
                if (!is_tick(source.production)
                    || source.type != source_conversion.source_type
                    || source_conversion.target_type != group.source_type) {
                    return std::unexpected(
                        "GraphJit event fan-in producer disagrees with its declaration");
                }
                auto raw_representation = ensure_raw_event_output(source_id);
                if (!raw_representation) {
                    return std::unexpected(
                        std::move(raw_representation.error()));
                }
                auto const& resolved_source =
                    realization.event_output(source_id.bundle, source_id.port);
                auto const source_history = resolved_source.history;
                auto const source_latency = resolved_source.latency;
                auto const source_region = region_index_for_bundle(
                    source_id.bundle);
                auto const source_cyclic_region = source_region
                        && *source_region < connections.schedule.regions.size()
                        && connections.schedule.regions[*source_region].cyclic
                    ? source_region
                    : std::optional<std::size_t>{};
                auto producer_window_samples = input.specialization.block_size;
                auto aggregate_window_samples = input.specialization.block_size;
                if (source_cyclic_region) {
                    auto const quantum = connections.schedule.regions[
                        *source_cyclic_region].maximum_block_size;
                    if (quantum == 0) {
                        return std::unexpected(
                            "GraphJit event fan-in source region has no SCC quantum");
                    }
                    auto const slice_count = input.specialization.block_size
                        / quantum
                        + (input.specialization.block_size % quantum != 0);
                    producer_window_samples = std::min(
                        input.specialization.block_size, quantum);
                    if (source_history
                            > std::numeric_limits<std::size_t>::max()
                                - source_latency
                        || ((source_history + source_latency) != 0
                            && slice_count
                                > (std::numeric_limits<std::size_t>::max()
                                    - aggregate_window_samples)
                                    / (source_history + source_latency))) {
                        return std::unexpected(
                            "GraphJit cyclic event fan-in authored window overflows size_t");
                    }
                    aggregate_window_samples += slice_count
                        * (source_history + source_latency);
                }
                if (source_history > std::numeric_limits<std::size_t>::max()
                        - producer_window_samples
                    || source_latency > std::numeric_limits<std::size_t>::max()
                        - producer_window_samples - source_history) {
                    return std::unexpected(
                        "GraphJit event fan-in producer temporal window overflows size_t");
                }
                producer_window_samples += source_history + source_latency;
                auto const capacity = event_sequence_capacity_for_sample_span(
                    source.max_events_per_index, producer_window_samples);
                auto const aggregate_event_bound = source_cyclic_region
                    ? event_count_for_sample_span(
                        source.max_events_per_index,
                        aggregate_window_samples)
                    : capacity;
                if (!capacity || !aggregate_event_bound
                    || *aggregate_event_bound
                        > std::numeric_limits<std::size_t>::max()
                        - total_local_capacity) {
                    return std::unexpected(
                        "GraphJit event fan-in producer capacity is not representable");
                }
                total_local_capacity += *aggregate_event_bound;
                if (source_conversion.size() != 0) {
                    if (*capacity
                        > std::numeric_limits<std::size_t>::max()
                            - total_invocation_local_capacity) {
                        return std::unexpected(
                            "GraphJit event fan-in conversion capacity overflows size_t");
                    }
                    total_invocation_local_capacity += *capacity;
                }
                observed_rate += source.max_events_per_index;
                if (!std::isfinite(observed_rate)) {
                    return std::unexpected(
                        "GraphJit event fan-in aggregate rate is not representable");
                }
                auto const execution_position =
                    *connections.schedule.bundle_execution_position[source_id.bundle];
                producer_execution_position = std::max(
                    producer_execution_position, execution_position);
                validated_sources.push_back(ValidatedSource{
                    .id = source_id,
                    .semantic_source_index = group_source_index,
                    .primitive = *source_primitive,
                    .execution_position = execution_position,
                    .config = source,
                    .conversion = source_conversion,
                    .raw_representation = *raw_representation,
                    .capacity = *capacity,
                    .aggregate_event_bound = *aggregate_event_bound,
                    .cyclic_region = source_cyclic_region,
                });
            }
            if (observed_rate != group.max_events_per_index) {
                return std::unexpected(
                    "GraphJit event fan-in aggregate max_events_per_index disagrees with connection analysis");
            }

            auto source_representation_for_group = [&] (
                ValidatedSource const& source,
                EventOperationScope scope)
                -> std::expected<std::size_t, std::string> {
                if (source.conversion.size() == 0) {
                    if (plan.ports[source.raw_representation].type
                        != group.source_type) {
                        return std::unexpected(
                            "GraphJit identity event source conversion changed type");
                    }
                    return source.raw_representation;
                }
                auto existing = std::ranges::find_if(
                    plan.materializations,
                    [&](EventMaterializationPlan const& candidate) {
                        return candidate.source_representation
                                == source.raw_representation
                            && candidate.conversion == source.conversion
                            && candidate.scope == scope
                            && candidate.before_merge;
                    });
                if (existing != plan.materializations.end()) {
                    return existing->target_representation;
                }
                auto converted = append_representation(
                    group_index,
                    group.source_type,
                    plan.ports[source.raw_representation].event_capacity);
                if (!converted) {
                    return std::unexpected(std::move(converted.error()));
                }
                plan.materializations.push_back(EventMaterializationPlan{
                    .source_representation = source.raw_representation,
                    .target_representation = *converted,
                    .conversion = source.conversion,
                    .maximum_output_event_count =
                        source.aggregate_event_bound,
                    .maximum_source_event_reads =
                        source.aggregate_event_bound,
                    .before_merge = true,
                    .scope = scope,
                });
                return *converted;
            };

            // Source indices let a composed connection select its ordinary
            // branches from retained fan-in storage. Detached branches read
            // the per-source representations recorded below. Output history
            // and latency remain in the raw EventOutputPortId representations.
            auto const requires_source_identity =
                std::ranges::any_of(
                    group.connection_indices,
                    [&](std::size_t connection_index) {
                        return connection_index
                                < connections.event_connections.size()
                            && has_realtime_delivery(
                                connections.event_connections[
                                    connection_index])
                            && (connections.event_connections[connection_index]
                                    .requires_source_composition
                                || !is_entirely_realtime_delivery(
                                    connections.event_connections[
                                        connection_index]));
                    });

            std::size_t retained_history = 0;
            std::size_t retained_latency = 0;
            for (auto const connection_index : group.connection_indices) {
                if (connection_index >= connections.event_connections.size()) {
                    return std::unexpected(
                        "GraphJit event producer group references an invalid connection");
                }
                auto const& connection =
                    connections.event_connections[connection_index];
                if (!has_realtime_delivery(connection)) continue;
                retained_history = std::max(
                    retained_history,
                    std::max(connection.source_history, connection.target_history));
                retained_latency = std::max(
                    retained_latency, connection.source_latency);
            }

            if (staged_fan_in) {
                struct FanInStage {
                    std::optional<std::size_t> cyclic_region{};
                    std::size_t execution_position = 0;
                    std::vector<std::size_t> source_indices{};
                };
                std::vector<FanInStage> stages;
                for (std::size_t source_index = 0;
                     source_index < validated_sources.size(); ++source_index) {
                    auto const& source = validated_sources[source_index];
                    auto stage = source.cyclic_region
                        ? std::ranges::find_if(
                            stages,
                            [&](FanInStage const& candidate) {
                                return candidate.cyclic_region
                                    == source.cyclic_region;
                            })
                        : stages.end();
                    if (stage == stages.end()) {
                        stages.push_back(FanInStage{
                            .cyclic_region = source.cyclic_region,
                            .execution_position = source.execution_position,
                            .source_indices = {source_index},
                        });
                    } else {
                        stage->execution_position = std::max(
                            stage->execution_position,
                            source.execution_position);
                        stage->source_indices.push_back(source_index);
                    }
                }
                std::ranges::sort(
                    stages,
                    {},
                    &FanInStage::execution_position);
                if (stages.empty()) {
                    return std::unexpected(
                        "GraphJit staged event fan-in has no execution stages");
                }

                std::size_t staged_local_stack = 0;
                for (auto const& source : validated_sources) {
                    if (source.conversion.size() == 0) continue;
                    auto const capacity = source.cyclic_region
                        ? source.capacity
                        : source.aggregate_event_bound;
                    staged_local_stack = iv::detail::saturating_add(
                        staged_local_stack, capacity);
                }

                struct StagedMergeCost {
                    std::size_t writes = 0;
                    std::size_t ring_accesses = 0;
                };
                auto staged_merge_cost = [&](std::size_t initial_values) {
                    auto target_values = initial_values;
                    StagedMergeCost result;
                    for (auto const& stage : stages) {
                        std::size_t executions = 1;
                        if (stage.cyclic_region) {
                            auto const quantum = connections.schedule.regions[
                                *stage.cyclic_region].maximum_block_size;
                            executions = input.specialization.block_size / quantum
                                + (input.specialization.block_size % quantum != 0);
                        }
                        std::vector<std::size_t> remaining;
                        remaining.reserve(stage.source_indices.size());
                        for (auto const source_index : stage.source_indices) {
                            remaining.push_back(
                                validated_sources[source_index]
                                    .aggregate_event_bound);
                        }
                        for (std::size_t execution = 0;
                             execution < executions; ++execution) {
                            for (std::size_t stage_source = 0;
                                 stage_source < stage.source_indices.size();
                                 ++stage_source) {
                                auto const& source = validated_sources[
                                    stage.source_indices[stage_source]];
                                auto const produced = std::min(
                                    source.cyclic_region
                                        ? source.capacity
                                        : source.aggregate_event_bound,
                                    remaining[stage_source]);
                                remaining[stage_source] -= produced;
                                auto const previous_target = target_values;
                                target_values = iv::detail::saturating_add(
                                    target_values, produced);
                                result.writes = iv::detail::saturating_add(
                                    result.writes, target_values);
                                result.ring_accesses = iv::detail::saturating_add(
                                    result.ring_accesses,
                                    iv::detail::saturating_add(
                                        previous_target, target_values));
                            }
                        }
                    }
                    return result;
                };

                auto const retained_values =
                    group.storage_requirements.retained_event_capacity;
                auto const transient_merge = staged_merge_cost(0);
                auto const retained_merge = staged_merge_cost(retained_values);
                auto staged_requirements = group.storage_requirements;
                staged_requirements.operations = RealtimeStorageOperationCounts{
                    .transient_extra_copied_values = transient_merge.writes,
                    .carry_extra_copied_values = retained_merge.writes,
                    .full_extra_copied_values = retained_merge.writes,
                    .full_ring_addressed_values =
                        retained_merge.ring_accesses,
                    .transient_extra_stack_values = staged_local_stack,
                    .carry_extra_stack_values = staged_local_stack,
                    .full_extra_stack_values = staged_local_stack,
                };
                group_cost_requirements[group_index] = staged_requirements;

                auto selected_plan = decisions
                    ? decisions->producer_groups[group_index].storage
                    : choose_event_connection_storage_plan(
                        staged_requirements, cost_model);
                storage_kind = selected_plan.kind;
                plan.producer_group_storage_plans[group_index] = selected_plan;
                compact_carry = storage_kind
                    == RealtimeBufferStorageKind::stack_with_persistent_carry;
                persistent_ring = storage_kind
                    == RealtimeBufferStorageKind::full_node_storage;
                retained_storage = compact_carry || persistent_ring;
                aggregate_sequence = true;

                std::size_t carry_capacity = 0;
                std::size_t canonical_capacity = 0;
                if (compact_carry || persistent_ring) {
                    if (retained_history
                            > std::numeric_limits<std::size_t>::max()
                                - retained_latency) {
                        return std::unexpected(
                            "GraphJit staged event fan-in retained window overflows size_t");
                    }
                    auto retained = event_sequence_capacity_for_sample_span(
                        group.max_events_per_index,
                        retained_history + retained_latency);
                    if (!retained
                        || *retained
                            > std::numeric_limits<std::size_t>::max()
                                - total_local_capacity) {
                        return std::unexpected(
                            "GraphJit staged event fan-in retained capacity is not representable");
                    }
                    if (compact_carry) carry_capacity = *retained;
                    auto rounded = rounded_event_capacity(
                        total_local_capacity + *retained);
                    if (!rounded) {
                        return std::unexpected(std::move(rounded.error()));
                    }
                    canonical_capacity = *rounded;
                } else {
                    auto rounded = rounded_event_capacity(total_local_capacity);
                    if (!rounded) {
                        return std::unexpected(std::move(rounded.error()));
                    }
                    canonical_capacity = *rounded;
                }

                auto const identity_base = event_group_identity(group);
                auto canonical = append_representation(
                    group_index,
                    group.source_type,
                    canonical_capacity,
                    false,
                    persistent_ring,
                    persistent_ring
                        ? identity_base
                            + ":kind=ordered_persistent_ring:history="
                            + std::to_string(retained_history) + ":latency="
                            + std::to_string(retained_latency) + ":capacity="
                            + std::to_string(canonical_capacity)
                        : std::string{},
                    persistent_ring,
                    true);
                if (!canonical) {
                    return std::unexpected(std::move(canonical.error()));
                }
                plan.producer_group_representations[group_index] = *canonical;

                auto const first_scope = stages.front().cyclic_region
                    ? region_scope(
                        *stages.front().cyclic_region,
                        EventOperationPhase::before)
                    : primitive_scope(
                        stages.front().execution_position,
                        EventOperationPhase::before);
                auto const last_scope = stages.back().cyclic_region
                    ? region_scope(
                        *stages.back().cyclic_region,
                        EventOperationPhase::after)
                    : primitive_scope(
                        stages.back().execution_position,
                        EventOperationPhase::after);

                if (compact_carry) {
                    auto persistent = append_representation(
                        group_index,
                        group.source_type,
                        carry_capacity,
                        false,
                        true,
                        identity_base
                            + ":kind=ordered_compact_carry:history="
                            + std::to_string(retained_history) + ":latency="
                            + std::to_string(retained_latency) + ":capacity="
                            + std::to_string(carry_capacity),
                        false,
                        true);
                    if (!persistent) {
                        return std::unexpected(std::move(persistent.error()));
                    }
                    plan.carry_operations.push_back(EventCarryPlan{
                        .working_representation = *canonical,
                        .persistent_representation = *persistent,
                        .restore_scope = first_scope,
                        .commit_scope = last_scope,
                        .retained_history_samples = retained_history,
                        .retained_latency_samples = retained_latency,
                    });
                } else if (persistent_ring) {
                    plan.persistent_rings.push_back(EventPersistentRingPlan{
                        .storage = *canonical,
                        .prune_scope = first_scope,
                        .retained_history_samples = retained_history,
                    });
                } else {
                    plan.sequence_resets.push_back(EventSequenceResetPlan{
                        .storage = *canonical,
                        .scope = first_scope,
                    });
                }

                for (auto const& stage : stages) {
                    EventMergePlan merge{
                        .target_representation = *canonical,
                        .scope = primitive_scope(
                            stage.execution_position,
                            EventOperationPhase::after),
                        .ordering_history_samples = retained_history,
                        .preserve_existing_target = true,
                    };
                    merge.source_representations.reserve(
                        stage.source_indices.size());
                    merge.source_indices.reserve(stage.source_indices.size());
                    for (auto const source_index : stage.source_indices) {
                        auto const& source = validated_sources[source_index];
                        auto local = source_representation_for_group(
                            source, merge.scope);
                        if (!local) {
                            return std::unexpected(
                                std::move(local.error()));
                        }
                        plan.producer_group_source_representations[
                            group_index][source.semantic_source_index] = *local;
                        merge.source_representations.push_back(*local);
                        merge.source_indices.push_back(
                            source.semantic_source_index);
                    }
                    plan.merges.push_back(std::move(merge));
                }

                for (auto const connection_index : group.connection_indices) {
                    auto const& connection =
                        connections.event_connections[connection_index];
                    if (!has_realtime_delivery(connection)
                        || has_detached_source(connection)) continue;
                    auto const retained_connection =
                        connection.source_history != 0
                        || connection.source_latency != 0
                        || connection.target_history != 0;
                    if (connection.external_boundary
                        || !realtime_sources_match(group, connection)
                        || connection.source_type != group.source_type
                        || connection.conversion.source_type
                            != connection.source_type
                        || connection.conversion.target_type
                            != connection.target_type
                        || (retained_connection && !retained_storage)) {
                        return std::unexpected(
                            "GraphJit staged event fan-in connection requires unsupported retention, feedback, external, or source semantics");
                    }

                    for (auto const target_id : realtime_targets(connection)) {
                        auto const target_primitive = primitive_index_for_bundle(
                            target_id.bundle);
                        if (!target_primitive
                            || target_id.port
                                >= plan.primitives[*target_primitive]
                                       .inputs.size()) {
                            return std::unexpected(
                                "GraphJit staged event fan-in requires internal concrete consumers");
                        }
                        NodeBundlePortId const target_port{
                            target_id.bundle, PortKind::event, target_id.port};
                        auto const target = input.graph.node_bundles
                            .resolve_event_input(target_port).config;
                        if (!is_sequential(target.access)
                            || target.type != connection.target_type) {
                            return std::unexpected(
                                "GraphJit staged event fan-in consumer disagrees with its declaration");
                        }

                        auto target_representation = *canonical;
                        if (connection.conversion.size() != 0) {
                            auto const preserve_source_indices =
                                target_receives_background_delivery(
                                    connection, target_id);
                            auto const target_region = region_index_for_bundle(
                                target_id.bundle);
                            EventOperationScope scope = last_scope;
                            if (target_region
                                && *target_region
                                    < connections.schedule.regions.size()
                                && connections.schedule.regions[*target_region]
                                       .cyclic) {
                                auto const source_stage = std::ranges::find_if(
                                    stages,
                                    [&](FanInStage const& candidate) {
                                        return candidate.cyclic_region
                                            == target_region;
                                    });
                                scope = source_stage == stages.end()
                                    ? region_scope(
                                        *target_region,
                                        EventOperationPhase::before)
                                    : primitive_scope(
                                        source_stage->execution_position,
                                        EventOperationPhase::after);
                            }
                            auto const select_invocation_window =
                                retained_connection
                                || (target_region
                                    && *target_region
                                        < connections.schedule.regions.size()
                                    && connections.schedule.regions[
                                           *target_region].cyclic);
                            auto materialized_events =
                                materialization_event_bound(
                                    group.max_events_per_index,
                                    connection.target_history,
                                    scope,
                                    select_invocation_window,
                                    iv::detail::saturating_add(
                                        total_local_capacity,
                                        group.storage_requirements
                                            .retained_event_capacity));
                            if (!materialized_events) {
                                return std::unexpected(
                                    std::move(materialized_events.error()));
                            }
                            auto existing = std::ranges::find_if(
                                plan.materializations,
                                [&](EventMaterializationPlan const& candidate) {
                                    return candidate.source_representation
                                            == *canonical
                                        && !candidate.source_index_filter
                                        && candidate.conversion
                                            == connection.conversion
                                        && candidate.scope == scope
                                        && candidate.target_representation
                                            < plan.ports.size()
                                        && plan.ports[
                                               candidate.target_representation]
                                               .type
                                            == connection.target_type
                                        && plan.ports[
                                               candidate.target_representation]
                                               .has_source_indices
                                            == preserve_source_indices;
                                });
                            if (existing != plan.materializations.end()) {
                                target_representation =
                                    existing->target_representation;
                                existing->history_samples = std::max(
                                    existing->history_samples,
                                    connection.target_history);
                                existing->maximum_output_event_count = std::max(
                                    existing->maximum_output_event_count,
                                    materialized_events->output_writes);
                                existing->maximum_source_event_reads = std::max(
                                    existing->maximum_source_event_reads,
                                    materialized_events->source_reads);
                            } else {
                                auto derived = append_representation(
                                    group_index,
                                    connection.target_type,
                                    plan.ports[*canonical]
                                        .event_capacity,
                                    false,
                                    false,
                                    {},
                                    false,
                                    preserve_source_indices);
                                if (!derived) {
                                    return std::unexpected(
                                        std::move(derived.error()));
                                }
                                target_representation = *derived;
                                plan.materializations.push_back(
                                    EventMaterializationPlan{
                                        .source_representation = *canonical,
                                        .target_representation =
                                            target_representation,
                                        .conversion = connection.conversion,
                                        .preserve_source_indices =
                                            preserve_source_indices,
                                        .history_samples =
                                            connection.target_history,
                                        .maximum_output_event_count =
                                            materialized_events->output_writes,
                                        .maximum_source_event_reads =
                                            materialized_events->source_reads,
                                        .select_invocation_window =
                                            select_invocation_window,
                                        .scope = scope,
                                    });
                            }
                        }
                        auto& target_binding = plan.primitives[
                            *target_primitive].inputs[target_id.port];
                        if (target_binding.storage) {
                            return std::unexpected(
                                "GraphJit event input has more than one realized connection");
                        }
                        target_binding.storage = target_representation;
                        auto mixed = configure_mixed_input(
                            target_binding, connection, target_id, group_index);
                        if (!mixed) {
                            return std::unexpected(std::move(mixed.error()));
                        }
                    }
                }
                continue;
            }

            // Build and cost fan-in storage here, next to the exact raw
            // callback buffers and merge operation that lowering emits.
            // Connection analysis intentionally does not reconstruct these
            // implementation details.
            auto const saturating_add_values = [](std::size_t lhs,
                                                  std::size_t rhs) {
                return iv::detail::saturating_add(lhs, rhs);
            };
            auto const retained_values =
                group.storage_requirements.retained_event_capacity;

            std::size_t slice_count = 1;
            if (cyclic_source_region) {
                auto const quantum = connections.schedule.regions[
                    *cyclic_source_region].maximum_block_size;
                slice_count = input.specialization.block_size / quantum
                    + (input.specialization.block_size % quantum != 0);
            }

            auto sequential_merge_writes = [&](std::size_t initial_values) {
                auto target_values = initial_values;
                std::size_t writes = 0;
                if (!cyclic_source_region) {
                    for (std::size_t source_index = 0;
                         source_index < validated_sources.size(); ++source_index) {
                        target_values = saturating_add_values(
                            target_values,
                            validated_sources[source_index].aggregate_event_bound);
                        writes = saturating_add_values(writes, target_values);
                    }
                    return writes;
                }

                std::vector<std::size_t> remaining;
                remaining.reserve(validated_sources.size());
                for (auto const& source : validated_sources) {
                    remaining.push_back(source.aggregate_event_bound);
                }
                for (std::size_t slice = 0; slice < slice_count; ++slice) {
                    for (std::size_t source_index = 0;
                         source_index < validated_sources.size(); ++source_index) {
                        auto const produced = std::min(
                            validated_sources[source_index].capacity,
                            remaining[source_index]);
                        remaining[source_index] -= produced;
                        target_values = saturating_add_values(
                            target_values, produced);
                        writes = saturating_add_values(writes, target_values);
                    }
                }
                return writes;
            };

            auto sequential_ring_accesses = [&](std::size_t initial_values) {
                auto target_values = initial_values;
                std::size_t accesses = 0;
                auto account_source = [&](std::size_t produced) {
                    auto const previous_target = target_values;
                    target_values = saturating_add_values(
                        target_values, produced);
                    // event_sequence_merge() reads the existing target sequence
                    // and writes the complete merged target sequence. Source
                    // reads remain in raw callback stack buffers.
                    accesses = saturating_add_values(
                        accesses,
                        saturating_add_values(previous_target, target_values));
                };

                if (!cyclic_source_region) {
                    for (std::size_t source_index = 0;
                         source_index < validated_sources.size(); ++source_index) {
                        account_source(
                            validated_sources[source_index].aggregate_event_bound);
                    }
                    return accesses;
                }

                std::vector<std::size_t> remaining;
                remaining.reserve(validated_sources.size());
                for (auto const& source : validated_sources) {
                    remaining.push_back(source.aggregate_event_bound);
                }
                for (std::size_t slice = 0; slice < slice_count; ++slice) {
                    for (std::size_t source_index = 0;
                         source_index < validated_sources.size(); ++source_index) {
                        auto const produced = std::min(
                            validated_sources[source_index].capacity,
                            remaining[source_index]);
                        remaining[source_index] -= produced;
                        account_source(produced);
                    }
                }
                return accesses;
            };

            auto const transient_merge_writes =
                sequential_merge_writes(0);
            auto const retained_merge_writes =
                sequential_merge_writes(retained_values);
            auto const retained_ring_accesses =
                sequential_ring_accesses(retained_values);
            auto group_requirements = group.storage_requirements;
            group_requirements.operations = RealtimeStorageOperationCounts{
                .transient_extra_copied_values = transient_merge_writes,
                .carry_extra_copied_values = retained_merge_writes,
                .full_extra_copied_values = retained_merge_writes,
                .full_ring_addressed_values = retained_ring_accesses,
                .transient_extra_stack_values =
                    total_invocation_local_capacity,
                .carry_extra_stack_values = total_invocation_local_capacity,
                .full_extra_stack_values = total_invocation_local_capacity,
            };
            auto const group_plan = choose_event_connection_storage_plan(
                group_requirements, cost_model);
            group_cost_requirements[group_index] = group_requirements;

            auto selected_cost = [](EventConnectionStoragePlan const& candidate)
                -> std::optional<std::size_t> {
                auto const& cost = candidate.candidate_costs.for_kind(
                    candidate.kind);
                if (!cost.legal) return std::nullopt;
                return cost.weighted_cost;
            };
            auto const group_cost = selected_cost(group_plan);
            auto selected_plan = decisions
                ? decisions->producer_groups[group_index].storage
                : group_plan;
            if (!group_cost || !selected_cost(selected_plan)) {
                return std::unexpected(
                    "GraphJit event fan-in has no storage realization within the compile-time stack budget");
            }

            storage_kind = selected_plan.kind;
            plan.producer_group_storage_plans[group_index] = selected_plan;
            compact_carry = storage_kind
                == RealtimeBufferStorageKind::stack_with_persistent_carry;
            persistent_ring = storage_kind
                == RealtimeBufferStorageKind::full_node_storage;
            retained_storage = compact_carry || persistent_ring;
            aggregate_sequence = true;

            std::size_t carry_capacity = 0;
            std::size_t canonical_capacity = 0;
            if (compact_carry) {
                if (retained_latency > std::numeric_limits<std::size_t>::max()
                        - retained_history) {
                    return std::unexpected(
                        "GraphJit event fan-in retained window overflows size_t");
                }
                auto const retained_window = retained_history + retained_latency;
                auto carry = event_sequence_capacity_for_sample_span(
                    group.max_events_per_index, retained_window);
                if (!carry
                    || *carry > std::numeric_limits<std::size_t>::max()
                        - total_local_capacity) {
                    return std::unexpected(
                        "GraphJit event fan-in compact carry capacity is not representable");
                }
                carry_capacity = *carry;
                auto rounded = rounded_event_capacity(
                    total_local_capacity + carry_capacity);
                if (!rounded) return std::unexpected(std::move(rounded.error()));
                canonical_capacity = *rounded;
            } else if (persistent_ring) {
                if (retained_history > std::numeric_limits<std::size_t>::max()
                        - retained_latency) {
                    return std::unexpected(
                        "GraphJit event fan-in retained window overflows size_t");
                }
                auto retained = event_sequence_capacity_for_sample_span(
                    group.max_events_per_index,
                    retained_history + retained_latency);
                if (!retained
                    || *retained > std::numeric_limits<std::size_t>::max()
                        - total_local_capacity) {
                    return std::unexpected(
                        "GraphJit event fan-in persistent capacity is not representable");
                }
                auto rounded = rounded_event_capacity(
                    total_local_capacity + *retained);
                if (!rounded) return std::unexpected(std::move(rounded.error()));
                canonical_capacity = *rounded;
            } else {
                auto rounded = rounded_event_capacity(total_local_capacity);
                if (!rounded) return std::unexpected(std::move(rounded.error()));
                canonical_capacity = *rounded;
            }

            auto const preserve_source_identity = requires_source_identity;
            auto const identity_base = event_group_identity(group);
            auto canonical = append_representation(
                group_index,
                group.source_type,
                canonical_capacity,
                false,
                persistent_ring,
                persistent_ring
                    ? identity_base + ":kind=persistent_ring:history="
                        + std::to_string(retained_history) + ":latency="
                        + std::to_string(retained_latency) + ":capacity="
                        + std::to_string(canonical_capacity)
                    : std::string{},
                persistent_ring,
                preserve_source_identity);
            if (!canonical) {
                return std::unexpected(std::move(canonical.error()));
            }
            plan.producer_group_representations[group_index] = *canonical;

            if (compact_carry) {
                auto persistent = append_representation(
                    group_index,
                    group.source_type,
                    carry_capacity,
                    false,
                    true,
                    identity_base + ":kind=compact_carry:history="
                        + std::to_string(retained_history) + ":latency="
                        + std::to_string(retained_latency) + ":capacity="
                        + std::to_string(carry_capacity),
                    false,
                    preserve_source_identity);
                if (!persistent) {
                    return std::unexpected(std::move(persistent.error()));
                }
                auto const restore_scope = cyclic_source_region
                    ? region_scope(
                        *cyclic_source_region, EventOperationPhase::before)
                    : primitive_scope(
                        producer_execution_position,
                        EventOperationPhase::before);
                auto const commit_scope = cyclic_source_region
                    ? region_scope(
                        *cyclic_source_region, EventOperationPhase::after)
                    : primitive_scope(
                        producer_execution_position,
                        EventOperationPhase::after);
                plan.carry_operations.push_back(EventCarryPlan{
                    .working_representation = *canonical,
                    .persistent_representation = *persistent,
                    .restore_scope = restore_scope,
                    .commit_scope = commit_scope,
                    .retained_history_samples = retained_history,
                    .retained_latency_samples = retained_latency,
                });
            } else if (persistent_ring) {
                plan.persistent_rings.push_back(EventPersistentRingPlan{
                    .storage = *canonical,
                    .prune_scope = cyclic_source_region
                        ? region_scope(
                            *cyclic_source_region,
                            EventOperationPhase::before)
                        : primitive_scope(
                            producer_execution_position,
                            EventOperationPhase::before),
                    .retained_history_samples = retained_history,
                });
            }

            EventMergePlan merge{
                .target_representation = *canonical,
                .scope = primitive_scope(
                    producer_execution_position, EventOperationPhase::after),
                .ordering_history_samples = retained_history,
                .preserve_existing_target =
                    retained_storage || cyclic_source_region.has_value(),
            };
            merge.source_representations.reserve(validated_sources.size());
            if (preserve_source_identity) {
                merge.source_indices.reserve(validated_sources.size());
            }
            for (std::size_t source_index = 0;
                 source_index < validated_sources.size(); ++source_index) {
                auto const& source = validated_sources[source_index];
                auto storage = source_representation_for_group(
                    source, merge.scope);
                if (!storage) {
                    return std::unexpected(std::move(storage.error()));
                }
                plan.producer_group_source_representations[
                    group_index][source.semantic_source_index] = *storage;
                merge.source_representations.push_back(*storage);
                if (preserve_source_identity) {
                    merge.source_indices.push_back(
                        source.semantic_source_index);
                }
            }
            if (cyclic_source_region && !retained_storage) {
                plan.sequence_resets.push_back(EventSequenceResetPlan{
                    .storage = *canonical,
                    .scope = region_scope(
                        *cyclic_source_region, EventOperationPhase::before),
                });
            }
            plan.merges.push_back(std::move(merge));

            for (auto const connection_index : group.connection_indices) {
                auto const& connection =
                    connections.event_connections[connection_index];
                if (!has_realtime_delivery(connection)
                    || has_detached_source(connection)) continue;
                auto const retained_connection =
                    connection.source_history != 0
                    || connection.source_latency != 0
                    || connection.target_history != 0;
                if (connection.external_boundary
                    || !realtime_sources_match(group, connection)
                    || connection.source_type != group.source_type
                    || connection.conversion.source_type
                        != connection.source_type
                    || connection.conversion.target_type
                        != connection.target_type
                    || (retained_connection && !retained_storage)) {
                    return std::unexpected(
                        "GraphJit event fan-in connection requires unsupported retention, feedback, external, or source semantics");
                }

                for (auto const target_id : realtime_targets(connection)) {
                    auto const target_primitive = primitive_index_for_bundle(
                        target_id.bundle);
                    if (!target_primitive
                        || target_id.port
                            >= plan.primitives[*target_primitive].inputs.size()) {
                        return std::unexpected(
                            "GraphJit event fan-in requires internal concrete consumers");
                    }
                    NodeBundlePortId const target_port{
                        target_id.bundle, PortKind::event, target_id.port};
                    auto const target = input.graph.node_bundles
                        .resolve_event_input(target_port).config;
                    if (!is_sequential(target.access)
                        || target.type != connection.target_type) {
                        return std::unexpected(
                            "GraphJit event fan-in consumer disagrees with its declaration");
                    }
                    auto target_representation = *canonical;
                    if (connection.conversion.size() != 0) {
                        auto const preserve_source_indices =
                            target_receives_background_delivery(
                                connection, target_id);
                        auto scope = materialization_scope_for_target(
                            connection.sources,
                            producer_execution_position,
                            target_id);
                        if (!scope) {
                            return std::unexpected(std::move(scope.error()));
                        }
                        auto const* target_region = region_for_bundle(
                            target_id.bundle);
                        auto const select_invocation_window = retained_connection
                            || (target_region && target_region->cyclic);
                        auto materialized_events = materialization_event_bound(
                            group.max_events_per_index,
                            connection.target_history,
                            *scope,
                            select_invocation_window,
                            iv::detail::saturating_add(
                                total_local_capacity,
                                group.storage_requirements
                                    .retained_event_capacity));
                        if (!materialized_events) {
                            return std::unexpected(
                                std::move(materialized_events.error()));
                        }
                        auto existing = std::ranges::find_if(
                            plan.materializations,
                            [&](EventMaterializationPlan const& candidate) {
                                return candidate.source_representation == *canonical
                                    && !candidate.source_index_filter
                                    && candidate.conversion == connection.conversion
                                    && candidate.scope == *scope
                                    && candidate.target_representation
                                        < plan.ports.size()
                                    && plan.ports[
                                           candidate.target_representation].type
                                        == connection.target_type
                                    && plan.ports[
                                           candidate.target_representation]
                                           .has_source_indices
                                        == preserve_source_indices;
                            });
                        if (existing != plan.materializations.end()) {
                            target_representation =
                                existing->target_representation;
                            existing->history_samples = std::max(
                                existing->history_samples,
                                connection.target_history);
                            existing->maximum_output_event_count = std::max(
                                existing->maximum_output_event_count,
                                materialized_events->output_writes);
                            existing->maximum_source_event_reads = std::max(
                                existing->maximum_source_event_reads,
                                materialized_events->source_reads);
                        } else {
                            auto derived = append_representation(
                                group_index,
                                connection.target_type,
                                plan.ports[*canonical].event_capacity,
                                false,
                                false,
                                {},
                                false,
                                preserve_source_indices);
                            if (!derived) {
                                return std::unexpected(
                                    std::move(derived.error()));
                            }
                            target_representation = *derived;
                            plan.materializations.push_back(
                                EventMaterializationPlan{
                                    .source_representation = *canonical,
                                    .target_representation = target_representation,
                                    .conversion = connection.conversion,
                                    .preserve_source_indices =
                                        preserve_source_indices,
                                    .history_samples = connection.target_history,
                                    .maximum_output_event_count =
                                        materialized_events->output_writes,
                                    .maximum_source_event_reads =
                                        materialized_events->source_reads,
                                    .select_invocation_window =
                                        select_invocation_window,
                                    .scope = *scope,
                                });
                        }
                    }
                    auto& target_binding =
                        plan.primitives[*target_primitive].inputs[target_id.port];
                    if (target_binding.storage) {
                        return std::unexpected(
                            "GraphJit event input has more than one realized connection");
                    }
                    target_binding.storage = target_representation;
                    auto mixed = configure_mixed_input(
                        target_binding, connection, target_id, group_index);
                    if (!mixed) {
                        return std::unexpected(std::move(mixed.error()));
                    }
                }
            }
            continue;
        }
        if (group.realtime_sources.size() != 1) {
            return std::unexpected(
                "GraphJit event producer group has no realtime source");
        }

        auto const source_id = group.realtime_sources.front();
        auto const source_primitive = primitive_index_for_bundle(source_id.bundle);
        if (!source_primitive) {
            return std::unexpected(
                "GraphJit event flow requires an internal concrete producer");
        }
        if (!aggregate_sequence
            && analysis.primitives[*source_primitive].bundle.maximum_block_size
                < input.specialization.block_size) {
            return std::unexpected(
                "GraphJit direct event flow requires an unsliced producer");
        }
        NodeBundlePortId const source_port{
            source_id.bundle, PortKind::event, source_id.port};
        auto const source = input.graph.node_bundles
            .resolve_event_output(source_port).config;
        auto const source_position = std::ranges::find(
            group.sources, source_id);
        if (source_position == group.sources.end()) {
            return std::unexpected(
                "GraphJit realtime event source has no semantic position");
        }
        auto const semantic_source_index = static_cast<std::size_t>(
            std::distance(group.sources.begin(), source_position));
        auto const& source_conversion =
            group.source_conversions[semantic_source_index];
        if (!is_tick(source.production)
            || source.type != source_conversion.source_type
            || source_conversion.target_type != group.source_type) {
            return std::unexpected(
                "GraphJit event producer disagrees with its declaration");
        }
        if (source_id.port >= plan.primitives[*source_primitive].outputs.size()) {
            return std::unexpected(
                "GraphJit event producer index is outside primitive metadata");
        }
        auto raw_source_representation = ensure_raw_event_output(source_id);
        if (!raw_source_representation) {
            return std::unexpected(
                std::move(raw_source_representation.error()));
        }
        auto const raw_canonical_representation = plan.primitives[
            *source_primitive].outputs[source_id.port]
                .canonical_storage.value_or(*raw_source_representation);

        if (source.max_events_per_index != group.max_events_per_index) {
            return std::unexpected(
                "GraphJit event producer max_events_per_index disagrees with connection analysis");
        }
        auto const source_region_index = region_index_for_bundle(source_id.bundle);
        auto const source_region_is_cyclic = source_region_index
            && *source_region_index < connections.schedule.regions.size()
            && connections.schedule.regions[*source_region_index].cyclic;
        auto const source_before_scope = source_region_is_cyclic
            ? region_scope(*source_region_index, EventOperationPhase::before)
            : primitive_scope(
                group.live_interval.begin, EventOperationPhase::before);
        auto const source_after_scope = source_region_is_cyclic
            ? region_scope(*source_region_index, EventOperationPhase::after)
            : primitive_scope(
                group.live_interval.begin, EventOperationPhase::after);
        auto const& resolved_source =
            realization.event_output(source_id.bundle, source_id.port);
        auto const source_history = resolved_source.history;
        auto const source_latency = resolved_source.latency;
        auto producer_window_samples = input.specialization.block_size;
        auto aggregate_window_samples = input.specialization.block_size;
        if (source_region_is_cyclic) {
            auto const quantum = connections.schedule.regions[
                *source_region_index].maximum_block_size;
            if (quantum == 0) {
                return std::unexpected(
                    "GraphJit event producer region has no SCC quantum");
            }
            auto const slice_count = input.specialization.block_size / quantum
                + (input.specialization.block_size % quantum != 0);
            producer_window_samples = std::min(
                input.specialization.block_size, quantum);
            if (source_history
                    > std::numeric_limits<std::size_t>::max() - source_latency
                || ((source_history + source_latency) != 0
                    && slice_count
                        > (std::numeric_limits<std::size_t>::max()
                            - aggregate_window_samples)
                            / (source_history + source_latency))) {
                return std::unexpected(
                    "GraphJit cyclic event producer authored window overflows size_t");
            }
            aggregate_window_samples += slice_count
                * (source_history + source_latency);
        }
        if (source_history > std::numeric_limits<std::size_t>::max()
                - producer_window_samples
            || source_latency > std::numeric_limits<std::size_t>::max()
                - producer_window_samples - source_history) {
            return std::unexpected(
                "GraphJit event producer temporal window overflows size_t");
        }
        producer_window_samples += source_history + source_latency;
        if (!source_region_is_cyclic) {
            aggregate_window_samples = producer_window_samples;
        }
        auto const base_max_events = event_count_for_sample_span(
            source.max_events_per_index, aggregate_window_samples);
        auto const base_capacity = event_sequence_capacity_for_sample_span(
            source.max_events_per_index, producer_window_samples);
        if (!base_max_events || !base_capacity) {
            return std::unexpected(
                "GraphJit event producer sizing rate/sample span exceeds representable static capacity");
        }

        std::size_t retained_history = 0;
        std::size_t retained_latency = 0;
        for (auto const connection_index : group.connection_indices) {
            if (connection_index >= connections.event_connections.size()) {
                return std::unexpected(
                    "GraphJit event producer group references an invalid connection");
            }
            auto const& connection = connections.event_connections[connection_index];
            if (!has_realtime_delivery(connection)) continue;
            retained_history = std::max(
                retained_history,
                std::max(connection.source_history, connection.target_history));
            retained_latency = std::max(retained_latency, connection.source_latency);
        }
        group_cost_requirements[group_index] =
            group.storage_requirements;

        auto aggregate_capacity = rounded_event_capacity(*base_max_events);
        if (!aggregate_capacity) {
            return std::unexpected(std::move(aggregate_capacity.error()));
        }
        // A producer inside an SCC is invoked once per slice, but its
        // canonical sequence is the aggregate for the complete root call.
        // Size that sequence for every event the SCC can author during the
        // root call; only producer-local buffers use the single-slice bound.
        std::size_t working_capacity = aggregate_sequence
            ? *aggregate_capacity
            : *base_capacity;
        std::size_t carry_capacity = 0;
        std::size_t ring_capacity = 0;
        if (compact_carry) {
            if (retained_latency > std::numeric_limits<std::size_t>::max()
                    - retained_history) {
                return std::unexpected(
                    "GraphJit compact event carry retained window overflows size_t");
            }
            auto const retained_window_samples = retained_history + retained_latency;
            auto const carry_capacity_bound = event_sequence_capacity_for_sample_span(
                source.max_events_per_index, retained_window_samples);
            if (!carry_capacity_bound) {
                return std::unexpected(
                    "GraphJit compact event carry exceeds representable static capacity");
            }
            carry_capacity = *carry_capacity_bound;
            auto const carry_max_events =
                group.storage_requirements.retained_event_capacity;
            if (*base_max_events > std::numeric_limits<std::size_t>::max()
                    - carry_max_events) {
                return std::unexpected(
                    "GraphJit compact event carry working event bound overflows size_t");
            }
            auto const working_required = *base_max_events + carry_max_events;
            if (working_required == 0) {
                working_capacity = 0;
            } else {
                constexpr auto highest_power_of_two =
                    std::size_t{1}
                    << (std::numeric_limits<std::size_t>::digits - 1);
                if (working_required > highest_power_of_two) {
                    return std::unexpected(
                        "GraphJit compact event carry exceeds representable working capacity");
                }
                working_capacity = next_power_of_2(working_required);
            }
        } else if (persistent_ring) {
            if (retained_history > std::numeric_limits<std::size_t>::max()
                    - input.specialization.block_size
                || retained_latency > std::numeric_limits<std::size_t>::max()
                    - input.specialization.block_size - retained_history) {
                return std::unexpected(
                    "GraphJit persistent event ring temporal span overflows size_t");
            }
            if (*base_max_events
                > std::numeric_limits<std::size_t>::max()
                    - group.storage_requirements.retained_event_capacity) {
                return std::unexpected(
                    "GraphJit persistent event ring exceeds representable static capacity");
            }
            auto ring_capacity_bound = rounded_event_capacity(
                *base_max_events
                + group.storage_requirements.retained_event_capacity);
            if (!ring_capacity_bound) {
                return std::unexpected(std::move(ring_capacity_bound.error()));
            }
            ring_capacity = *ring_capacity_bound;
        }

        auto const raw_has_retained_storage =
            plan.ports[raw_canonical_representation].persistent
            || std::ranges::any_of(
                plan.carry_operations,
                [&](EventCarryPlan const& carry) {
                    return carry.working_representation
                        == raw_canonical_representation;
                });
        auto const uses_raw_source_directly =
            !source_region_is_cyclic
            && source_conversion.size() == 0
            && source_history >= retained_history
            && source_latency >= retained_latency
            && (!retained_storage || raw_has_retained_storage);
        auto source_representation = [&]()
            -> std::expected<std::size_t, std::string> {
            if (uses_raw_source_directly)
                return raw_canonical_representation;
            if (!persistent_ring) {
                return append_representation(
                    group_index,
                    group.source_type,
                    working_capacity,
                    false);
            }
            std::string migration_identity =
                "graphjit.event:" + std::to_string(static_cast<unsigned>(group.source_type))
                + ':' + std::to_string(source_id.bundle) + '.'
                + std::to_string(source_id.port)
                + ":kind=persistent_ring:history=" + std::to_string(retained_history)
                + ":latency=" + std::to_string(retained_latency)
                + ":capacity=" + std::to_string(ring_capacity);
            return append_representation(
                group_index,
                group.source_type,
                ring_capacity,
                false,
                true,
                std::move(migration_identity),
                true);
        }();
        if (!source_representation) {
            return std::unexpected(std::move(source_representation.error()));
        }
        plan.producer_group_representations[group_index] = *source_representation;

        if (compact_carry && !uses_raw_source_directly) {
            std::string migration_identity =
                "graphjit.event:" + std::to_string(static_cast<unsigned>(group.source_type))
                + ':' + std::to_string(source_id.bundle) + '.'
                + std::to_string(source_id.port)
                + ":kind=compact_carry:history=" + std::to_string(retained_history)
                + ":latency=" + std::to_string(retained_latency)
                + ":capacity=" + std::to_string(carry_capacity);
            auto persistent_representation = append_representation(
                group_index,
                group.source_type,
                carry_capacity,
                false,
                true,
                std::move(migration_identity));
            if (!persistent_representation) {
                return std::unexpected(
                    std::move(persistent_representation.error()));
            }
            plan.carry_operations.push_back(EventCarryPlan{
                .working_representation = *source_representation,
                .persistent_representation = *persistent_representation,
                .restore_scope = source_before_scope,
                .commit_scope = source_after_scope,
                .retained_history_samples = retained_history,
                .retained_latency_samples = retained_latency,
            });
        } else if (persistent_ring && !uses_raw_source_directly) {
            plan.persistent_rings.push_back(EventPersistentRingPlan{
                .storage = *source_representation,
                .prune_scope = source_before_scope,
                .retained_history_samples = retained_history,
            });
        }

        if (!uses_raw_source_directly) {
            auto producer_representation = *raw_source_representation;
            auto const merge_scope = primitive_scope(
                group.live_interval.begin, EventOperationPhase::after);
            if (source_conversion.size() != 0) {
                auto converted = append_representation(
                    group_index,
                    group.source_type,
                    plan.ports[*raw_source_representation].event_capacity);
                if (!converted) {
                    return std::unexpected(std::move(converted.error()));
                }
                producer_representation = *converted;
                plan.materializations.push_back(EventMaterializationPlan{
                    .source_representation = *raw_source_representation,
                    .target_representation = *converted,
                    .conversion = source_conversion,
                    .maximum_output_event_count = *base_max_events,
                    .maximum_source_event_reads = *base_max_events,
                    .before_merge = true,
                    .scope = merge_scope,
                });
            }
            plan.merges.push_back(EventMergePlan{
                .source_representations = {producer_representation},
                .target_representation = *source_representation,
                .scope = merge_scope,
                .ordering_history_samples = retained_history,
                .preserve_existing_target = retained_storage
                    || source_region_is_cyclic,
            });
            if (!retained_storage) {
                plan.sequence_resets.push_back(EventSequenceResetPlan{
                    .storage = *source_representation,
                    .scope = source_before_scope,
                });
            }
        }

        for (auto const connection_index : group.connection_indices) {
            auto const& connection = connections.event_connections[connection_index];
            if (!has_realtime_delivery(connection)
                || has_detached_source(connection)) continue;
            auto const retained_connection =
                connection.source_history != 0
                || connection.source_latency != 0
                || connection.target_history != 0;
            if (connection.external_boundary
                || !realtime_sources_match(group, connection)
                || connection.source_type != group.source_type
                || connection.conversion.source_type != connection.source_type
                || connection.conversion.target_type != connection.target_type
                || (retained_connection && !retained_storage)) {
                return std::unexpected(
                    "GraphJit event connection requires unsupported retention, feedback, external, or source-composition semantics");
            }
            for (auto const target_id : realtime_targets(connection)) {
                auto const target_primitive = primitive_index_for_bundle(
                    target_id.bundle);
                if (!target_primitive) {
                    return std::unexpected(
                        "GraphJit event flow requires internal concrete consumers");
                }
                NodeBundlePortId const target_port{
                    target_id.bundle, PortKind::event, target_id.port};
                auto const target = input.graph.node_bundles
                    .resolve_event_input(target_port).config;
                if (!is_sequential(target.access)
                    || target.type != connection.target_type
                    || target_id.port
                        >= plan.primitives[*target_primitive].inputs.size()) {
                    return std::unexpected(
                        "GraphJit event consumer disagrees with its declaration");
                }
                auto target_representation = *source_representation;
                if (connection.conversion.size() != 0) {
                    auto scope = materialization_scope_for_target(
                        connection.sources,
                        group.live_interval.begin,
                        target_id);
                    if (!scope) {
                        return std::unexpected(std::move(scope.error()));
                    }
                    auto const target_region = region_for_bundle(
                        target_id.bundle);
                    auto const select_invocation_window = retained_connection
                        || (target_region && target_region->cyclic);
                    auto materialized_events = materialization_event_bound(
                        group.max_events_per_index,
                        connection.target_history,
                        *scope,
                        select_invocation_window,
                        iv::detail::saturating_add(
                            *base_max_events,
                            group.storage_requirements
                                .retained_event_capacity));
                    if (!materialized_events) {
                        return std::unexpected(
                            std::move(materialized_events.error()));
                    }
                    auto existing = std::ranges::find_if(
                        plan.materializations,
                        [&](EventMaterializationPlan const& candidate) {
                            return candidate.source_representation
                                    == *source_representation
                                && !candidate.source_index_filter
                                && candidate.conversion == connection.conversion
                                && candidate.scope == *scope
                                && candidate.target_representation
                                    < plan.ports.size()
                                && plan.ports[
                                       candidate.target_representation].type
                                    == connection.target_type;
                        });
                    if (existing != plan.materializations.end()) {
                        target_representation = existing->target_representation;
                        existing->history_samples = std::max(
                            existing->history_samples,
                            connection.target_history);
                        existing->maximum_output_event_count = std::max(
                            existing->maximum_output_event_count,
                            materialized_events->output_writes);
                        existing->maximum_source_event_reads = std::max(
                            existing->maximum_source_event_reads,
                            materialized_events->source_reads);
                    } else {
                        auto derived = append_representation(
                            group_index,
                            connection.target_type,
                            plan.ports[*source_representation]
                                .event_capacity);
                        if (!derived) {
                            return std::unexpected(std::move(derived.error()));
                        }
                        target_representation = *derived;
                        plan.materializations.push_back(EventMaterializationPlan{
                            .source_representation = *source_representation,
                            .target_representation = target_representation,
                            .conversion = connection.conversion,
                            .history_samples = connection.target_history,
                            .maximum_output_event_count =
                                materialized_events->output_writes,
                            .maximum_source_event_reads =
                                materialized_events->source_reads,
                            .select_invocation_window =
                                select_invocation_window,
                            .scope = *scope,
                        });
                    }
                }
                auto& target_binding =
                    plan.primitives[*target_primitive].inputs[target_id.port];
                if (target_binding.storage) {
                    return std::unexpected(
                        "GraphJit event input has more than one realized connection");
                }
                target_binding.storage = target_representation;
                auto mixed = configure_mixed_input(
                    target_binding,
                    connection,
                    target_id,
                    group_index,
                    semantic_source_index);
                if (!mixed) {
                    return std::unexpected(std::move(mixed.error()));
                }
            }
        }
    }

    auto plan_source_event_feedback = [&] (
        EventConnectionPlan const& connection,
        EventSourcePlan const& source,
        std::size_t source_index,
        std::size_t source_group_index,
        std::size_t source_representation,
        std::size_t producer_position)
        -> std::expected<std::size_t, std::string> {
        if (!source.detach || !source.detach_region
            || *source.detach_region >= connections.schedule.regions.size()
            || source.detach->loop_extra_latency == 0) {
            return std::unexpected(
                "GraphJit per-source event detach lost its validated SCC metadata");
        }
        auto const& region =
            connections.schedule.regions[*source.detach_region];
        if (!region.cyclic || region.maximum_block_size == 0
            || source_group_index
                >= connections.event_producer_groups.size()
            || source_representation >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit per-source event detach has invalid producer storage");
        }
        auto const& group =
            connections.event_producer_groups[source_group_index];
        auto const& source_storage = plan.ports[source_representation];
        if (source_storage.type != connection.source_type) {
            return std::unexpected(
                "GraphJit per-source event detach source representation changed type");
        }

        auto const latency = source.detach->loop_extra_latency;
        auto const retained_history = std::max(
            source.history, connection.target_history);
        if (source.latency > std::numeric_limits<std::size_t>::max() - latency
            || retained_history
                > std::numeric_limits<std::size_t>::max()
                    - latency - source.latency) {
            return std::unexpected(
                "GraphJit per-source event feedback retained span overflows size_t");
        }
        auto const retained_window_samples =
            retained_history + source.latency + latency;
        auto const slice_count = input.specialization.block_size
            / region.maximum_block_size
            + (input.specialization.block_size % region.maximum_block_size != 0);
        if (source.history
                > std::numeric_limits<std::size_t>::max() - source.latency
            || ((source.history + source.latency) != 0
                && slice_count
                    > (std::numeric_limits<std::size_t>::max()
                        - input.specialization.block_size)
                        / (source.history + source.latency))) {
            return std::unexpected(
                "GraphJit per-source event feedback authored span overflows size_t");
        }
        auto const authored_window_samples = input.specialization.block_size
            + slice_count * (source.history + source.latency);
        auto const current_event_count = event_count_for_sample_span(
            source.max_events_per_index, input.specialization.block_size);
        auto const retained_event_count = event_count_for_sample_span(
            source.max_events_per_index, retained_window_samples);
        auto const authored_event_count = event_count_for_sample_span(
            source.max_events_per_index, authored_window_samples);
        auto const source_event_reads = event_count_for_sample_span(
            source.max_events_per_index, authored_window_samples);
        if (!current_event_count || !retained_event_count
            || !authored_event_count || !source_event_reads) {
            return std::unexpected(
                "GraphJit per-source event feedback rate/span is not representable");
        }
        auto feedback_requirements = EventConnectionStorageRequirements{
            .current_window_samples = input.specialization.block_size,
            .retained_window_samples = retained_window_samples,
            .current_event_capacity = *current_event_count,
            .retained_event_capacity = *retained_event_count,
            .value_size_bytes = sizeof(TimedEvent),
            .operations = RealtimeStorageOperationCounts{
                .invariant_copied_values = *authored_event_count,
                .full_ring_addressed_values = *authored_event_count,
            },
        };
        auto feedback_storage = choose_event_connection_storage_plan(
            feedback_requirements, cost_model);
        auto const append_scope = primitive_scope(
            producer_position, EventOperationPhase::after);
        auto const reset_scope = region_scope(
            *source.detach_region, EventOperationPhase::before);
        auto existing = std::ranges::find_if(
            plan.feedback_operations,
            [&](EventFeedbackPlan const& feedback) {
                return feedback.source_representation == source_representation
                    && !feedback.source_index_filter
                    && feedback.source_sequence_restarts_each_append
                    && feedback.append_scope == append_scope
                    && feedback.reset_scope == reset_scope
                    && feedback.loop_extra_latency == latency
                    && feedback.retained_window_samples
                        == retained_window_samples;
            });
        if (existing != plan.feedback_operations.end())
            return existing->target_representation;

        auto const feedback_index = plan.feedback_operations.size();
        if (decisions) {
            if (feedback_index >= decisions->feedback.size()) {
                return std::unexpected(
                    "GraphJit event costing lost a per-source feedback decision");
            }
            feedback_requirements =
                decisions->feedback[feedback_index].requirements;
            feedback_storage = decisions->feedback[feedback_index].storage;
        }
        auto const identity_base =
            "graphjit.event.feedback:group=" + event_group_identity(group)
            + ":source=" + std::to_string(source_index)
            + ":type="
            + std::to_string(static_cast<unsigned>(connection.source_type))
            + ":latency=" + std::to_string(latency)
            + ":retained=" + std::to_string(retained_window_samples);

        std::size_t target_representation = 0;
        switch (feedback_storage.kind) {
        case RealtimeBufferStorageKind::transient_stack: {
            auto capacity = rounded_event_capacity(*authored_event_count);
            if (!capacity) return std::unexpected(std::move(capacity.error()));
            auto target = append_representation(
                source_group_index, connection.source_type, *capacity);
            if (!target) return std::unexpected(std::move(target.error()));
            target_representation = *target;
            break;
        }
        case RealtimeBufferStorageKind::stack_with_persistent_carry: {
            if (*authored_event_count
                > std::numeric_limits<std::size_t>::max()
                    - *retained_event_count) {
                return std::unexpected(
                    "GraphJit per-source event feedback compact capacity overflows size_t");
            }
            auto working_capacity = rounded_event_capacity(
                *authored_event_count + *retained_event_count);
            auto carry_capacity = event_sequence_capacity_for_sample_span(
                source.max_events_per_index, retained_window_samples);
            if (!working_capacity || !carry_capacity) {
                return std::unexpected(
                    "GraphJit per-source event feedback compact capacity is not representable");
            }
            auto working = append_representation(
                source_group_index, connection.source_type, *working_capacity);
            if (!working) return std::unexpected(std::move(working.error()));
            target_representation = *working;
            auto persistent = append_representation(
                source_group_index,
                connection.source_type,
                *carry_capacity,
                false,
                true,
                identity_base + ":kind=compact_carry:capacity="
                    + std::to_string(*carry_capacity));
            if (!persistent)
                return std::unexpected(std::move(persistent.error()));
            plan.carry_operations.push_back(EventCarryPlan{
                .working_representation = target_representation,
                .persistent_representation = *persistent,
                .restore_scope = reset_scope,
                .commit_scope = region_scope(
                    *source.detach_region, EventOperationPhase::after),
                .retained_history_samples = retained_history,
                .retained_latency_samples = source.latency + latency,
            });
            break;
        }
        case RealtimeBufferStorageKind::full_node_storage: {
            if (*authored_event_count
                > std::numeric_limits<std::size_t>::max()
                    - *retained_event_count) {
                return std::unexpected(
                    "GraphJit per-source event feedback persistent capacity overflows size_t");
            }
            auto ring_capacity = rounded_event_capacity(
                *authored_event_count + *retained_event_count);
            if (!ring_capacity)
                return std::unexpected(std::move(ring_capacity.error()));
            auto target = append_representation(
                source_group_index,
                connection.source_type,
                *ring_capacity,
                false,
                true,
                identity_base + ":kind=full_node_storage:capacity="
                    + std::to_string(*ring_capacity),
                true);
            if (!target) return std::unexpected(std::move(target.error()));
            target_representation = *target;
            break;
        }
        }
        plan.feedback_operations.push_back(EventFeedbackPlan{
            .source_representation = source_representation,
            .target_representation = target_representation,
            .source_sequence_restarts_each_append = true,
            .append_scope = append_scope,
            .reset_scope = reset_scope,
            .storage_requirements = feedback_requirements,
            .storage_plan = feedback_storage,
            .authored_event_count = *authored_event_count,
            .source_event_reads = *source_event_reads,
            .retained_event_count = *retained_event_count,
            .retained_window_samples = retained_window_samples,
            .ordering_history_samples = retained_history,
            .loop_extra_latency = latency,
        });
        return target_representation;
    };

    for (auto const& connection : connections.event_connections) {
        if (!connection.detach) continue;
        if (!connection.detach_region
            || *connection.detach_region >= connections.schedule.regions.size()
            || !connection.detach->loop_extra_latency) {
            return std::unexpected(
                "GraphJit event detach lost its validated SCC metadata");
        }
        auto const& region = connections.schedule.regions[*connection.detach_region];
        if (!region.cyclic || region.maximum_block_size == 0) {
            return std::unexpected(
                "GraphJit event detach has an invalid SCC execution region");
        }
        if (!is_entirely_realtime_delivery(connection)
            || connection.external_boundary) {
            return std::unexpected(
                "GraphJit event feedback requires internal realtime transport");
        }

        auto const source_group_it = std::ranges::find_if(
            connections.event_producer_groups,
            [&](EventProducerGroupPlan const& group) {
                return group.source_type == connection.source_type
                    && group.sources == connection.sources
                    && source_conversions_match(group, connection);
            });
        if (source_group_it == connections.event_producer_groups.end()) {
            return std::unexpected(
                "GraphJit event detach lost its semantic producer group");
        }
        auto const source_group_index = static_cast<std::size_t>(
            std::distance(connections.event_producer_groups.begin(), source_group_it));
        std::size_t producer_position = 0;
        for (auto const source : connection.sources) {
            if (source.bundle
                    >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[source.bundle]) {
                return std::unexpected(
                    "GraphJit event feedback source has no executable schedule position");
            }
            producer_position = std::max(
                producer_position,
                *connections.schedule.bundle_execution_position[source.bundle]);
        }
        if (source_group_index >= plan.producer_group_representations.size()
            || !plan.producer_group_representations[source_group_index]) {
            return std::unexpected(
                "GraphJit event detach source has no realized event storage");
        }
        auto const canonical_source_representation =
            *plan.producer_group_representations[source_group_index];
        if (canonical_source_representation >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit event detach source storage is invalid");
        }
        // Detached feedback must observe the producer group's canonical
        // root-call stream, not a producer-local per-slice buffer. Any local
        // source-history/latency buffer is merged into this stream immediately
        // before feedback_append runs. The feedback cursor then selects only
        // the newly appended suffix, so retained events already present in a
        // carry buffer or persistent ring are not copied into feedback again.
        auto const source_representation = canonical_source_representation;
        auto const& source_storage = plan.ports[source_representation];
        if (source_storage.type != connection.source_type) {
            return std::unexpected(
                "GraphJit event detach source storage disagrees with detach type");
        }

        auto const latency = connection.detach->loop_extra_latency;
        bool has_consumer_position = false;
        for (auto const target_id : connection.targets) {
            if (target_id.bundle
                    >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[target_id.bundle]) {
                return std::unexpected(
                    "GraphJit event feedback consumer has no executable schedule position");
            }
            has_consumer_position = true;
        }
        if (!has_consumer_position) {
            return std::unexpected(
                "GraphJit event feedback has no in-SCC consumer");
        }

        auto const retained_history = std::max(
            connection.source_history, connection.target_history);
        if (connection.source_latency
                > std::numeric_limits<std::size_t>::max() - latency
            || retained_history
                > std::numeric_limits<std::size_t>::max()
                    - latency - connection.source_latency) {
            return std::unexpected(
                "GraphJit event feedback retained span overflows size_t");
        }
        auto const retained_window_samples =
            retained_history + connection.source_latency + latency;
        auto const slice_count = input.specialization.block_size
            / region.maximum_block_size
            + (input.specialization.block_size % region.maximum_block_size != 0);
        if (connection.source_history
                > std::numeric_limits<std::size_t>::max()
                    - connection.source_latency
            || ((connection.source_history + connection.source_latency) != 0
                && slice_count
                    > (std::numeric_limits<std::size_t>::max()
                        - input.specialization.block_size)
                        / (connection.source_history
                            + connection.source_latency))) {
            return std::unexpected(
                "GraphJit event feedback authored span overflows size_t");
        }
        auto const authored_window_samples =
            input.specialization.block_size
            + slice_count
                * (connection.source_history + connection.source_latency);
        auto const current_event_count = event_count_for_sample_span(
            source_group_it->max_events_per_index,
            input.specialization.block_size);
        auto const retained_event_count = event_count_for_sample_span(
            source_group_it->max_events_per_index,
            retained_window_samples);
        auto const authored_event_count = event_count_for_sample_span(
            source_group_it->max_events_per_index,
            authored_window_samples);
        if (!current_event_count || !retained_event_count
            || !authored_event_count) {
            return std::unexpected(
                "GraphJit event feedback rate/span exceeds representable static capacity");
        }
        auto feedback_requirements = EventConnectionStorageRequirements{
                // Storage selection compares the root-call working window with
                // the state retained between calls. History/latency may make a
                // feedback producer author more than one root window while its
                // SCC is sliced; that additional work belongs in the operation
                // counts below, not in the current-footprint term. Storage
                // feedback buffers are still sized from authored_event_count.
                .current_window_samples = input.specialization.block_size,
                .retained_window_samples = retained_window_samples,
                .current_event_capacity = *current_event_count,
                .retained_event_capacity = *retained_event_count,
                .value_size_bytes = sizeof(TimedEvent),
                .operations = RealtimeStorageOperationCounts{
                    // feedback_append copies the newly-authored producer
                    // suffix exactly once. A full persistent target performs
                    // the same writes through ring addressing.
                    .invariant_copied_values = *authored_event_count,
                    .full_ring_addressed_values = *authored_event_count,
                },
            };
        auto feedback_storage = choose_event_connection_storage_plan(
            feedback_requirements, cost_model);

        // Multiple detached branches from one source with the same authored
        // latency are the same delayed event stream. Share one planned delayed
        // storage and one producer-side append operation across all such
        // consumers, regardless of whether that storage is transient,
        // compact carry, or a full persistent ring.
        auto existing_feedback = std::ranges::find_if(
            plan.feedback_operations,
            [&](EventFeedbackPlan const& feedback) {
                return feedback.source_representation == source_representation
                    && feedback.append_scope
                        == primitive_scope(
                            producer_position, EventOperationPhase::after)
                    && feedback.loop_extra_latency == latency
                    && feedback.retained_window_samples == retained_window_samples;
            });

        std::size_t target_representation = 0;
        if (existing_feedback != plan.feedback_operations.end()) {
            target_representation = existing_feedback->target_representation;
            if (existing_feedback->reset_scope
                != region_scope(
                    *connection.detach_region, EventOperationPhase::before)) {
                return std::unexpected(
                    "GraphJit shared event feedback has an invalid reset scope");
            }
        } else {
            auto const feedback_index = plan.feedback_operations.size();
            if (decisions) {
                if (feedback_index >= decisions->feedback.size()) {
                    return std::unexpected(
                        "GraphJit event costing lost a feedback decision");
                }
                feedback_requirements =
                    decisions->feedback[feedback_index].requirements;
                feedback_storage = decisions->feedback[feedback_index].storage;
            }
            auto identity_base =
                "graphjit.event.feedback:group="
                + event_group_identity(*source_group_it)
                + ":type="
                + std::to_string(static_cast<unsigned>(connection.source_type))
                + ":latency=" + std::to_string(latency)
                + ":retained=" + std::to_string(retained_window_samples);

            switch (feedback_storage.kind) {
            case RealtimeBufferStorageKind::transient_stack: {
                auto capacity = rounded_event_capacity(*authored_event_count);
                if (!capacity) {
                    return std::unexpected(std::move(capacity.error()));
                }
                auto appended = append_representation(
                    source_group_index,
                    connection.source_type,
                    *capacity);
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
                target_representation = *appended;
                break;
            }
            case RealtimeBufferStorageKind::stack_with_persistent_carry: {
                if (*authored_event_count
                    > std::numeric_limits<std::size_t>::max()
                        - *retained_event_count) {
                    return std::unexpected(
                        "GraphJit event feedback compact working capacity overflows size_t");
                }
                auto working_capacity = rounded_event_capacity(
                    *authored_event_count + *retained_event_count);
                auto carry_capacity = event_sequence_capacity_for_sample_span(
                    source_group_it->max_events_per_index,
                    retained_window_samples);
                if (!working_capacity || !carry_capacity) {
                    return std::unexpected(
                        "GraphJit event feedback compact capacity is not representable");
                }
                auto working = append_representation(
                    source_group_index,
                    connection.source_type,
                    *working_capacity);
                if (!working) {
                    return std::unexpected(std::move(working.error()));
                }
                target_representation = *working;
                auto persistent = append_representation(
                    source_group_index,
                    connection.source_type,
                    *carry_capacity,
                    false,
                    true,
                    identity_base + ":kind=compact_carry:capacity="
                        + std::to_string(*carry_capacity));
                if (!persistent) {
                    return std::unexpected(std::move(persistent.error()));
                }
                plan.carry_operations.push_back(EventCarryPlan{
                    .working_representation = target_representation,
                    .persistent_representation = *persistent,
                    .restore_scope = region_scope(
                        *connection.detach_region, EventOperationPhase::before),
                    .commit_scope = region_scope(
                        *connection.detach_region, EventOperationPhase::after),
                    .retained_history_samples = retained_history,
                    .retained_latency_samples =
                        connection.source_latency + latency,
                });
                break;
            }
            case RealtimeBufferStorageKind::full_node_storage: {
                if (*authored_event_count
                    > std::numeric_limits<std::size_t>::max()
                        - *retained_event_count) {
                    return std::unexpected(
                        "GraphJit event feedback persistent capacity overflows size_t");
                }
                auto ring_capacity = rounded_event_capacity(
                    *authored_event_count + *retained_event_count);
                if (!ring_capacity) {
                    return std::unexpected(std::move(ring_capacity.error()));
                }
                auto appended = append_representation(
                    source_group_index,
                    connection.source_type,
                    *ring_capacity,
                    false,
                    true,
                    identity_base + ":kind=full_node_storage:capacity="
                        + std::to_string(*ring_capacity),
                    true);
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
                target_representation = *appended;
                break;
            }
            }

            plan.feedback_operations.push_back(EventFeedbackPlan{
                .source_representation = source_representation,
                .target_representation = target_representation,
                .append_scope = primitive_scope(
                    producer_position, EventOperationPhase::after),
                .reset_scope = region_scope(
                    *connection.detach_region, EventOperationPhase::before),
                .storage_requirements = feedback_requirements,
                .storage_plan = feedback_storage,
                .authored_event_count = *authored_event_count,
                .source_event_reads = *authored_event_count,
                .retained_event_count = *retained_event_count,
                .retained_window_samples = retained_window_samples,
                .ordering_history_samples = retained_history,
                .loop_extra_latency = latency,
            });
        }

        for (auto const target_id : connection.targets) {
            auto const target_primitive = primitive_index_for_bundle(target_id.bundle);
            if (!target_primitive
                || target_id.port >= plan.primitives[*target_primitive].inputs.size()) {
                return std::unexpected(
                    "GraphJit event feedback requires concrete in-SCC consumers");
            }
            NodeBundlePortId const target_port{
                target_id.bundle, PortKind::event, target_id.port};
            auto const target = input.graph.node_bundles
                .resolve_event_input(target_port).config;
            if (!is_sequential(target.access)
                || target.type != connection.target_type) {
                return std::unexpected(
                    "GraphJit event feedback consumer disagrees with its declaration");
            }
            auto consumer_representation = target_representation;
            if (connection.conversion.size() != 0) {
                if (target_id.bundle
                        >= connections.schedule.bundle_execution_position.size()
                    || !connections.schedule.bundle_execution_position[
                        target_id.bundle]) {
                    return std::unexpected(
                        "GraphJit event feedback consumer has no schedule position");
                }
                auto const scope = primitive_scope(
                    *connections.schedule.bundle_execution_position[
                        target_id.bundle],
                    EventOperationPhase::before);
                auto materialized_events = materialization_event_bound(
                    source_group_it->max_events_per_index,
                    connection.target_history,
                    scope,
                    true,
                    iv::detail::saturating_add(
                        *authored_event_count, *retained_event_count));
                if (!materialized_events) {
                    return std::unexpected(
                        std::move(materialized_events.error()));
                }
                auto existing = std::ranges::find_if(
                    plan.materializations,
                    [&](EventMaterializationPlan const& candidate) {
                        return candidate.source_representation
                                == target_representation
                            && !candidate.source_index_filter
                            && candidate.conversion == connection.conversion
                            && candidate.scope == scope
                            && candidate.target_representation
                                < plan.ports.size()
                            && plan.ports[
                                   candidate.target_representation].type
                                == connection.target_type;
                    });
                if (existing != plan.materializations.end()) {
                    consumer_representation = existing->target_representation;
                    existing->history_samples = std::max(
                        existing->history_samples, connection.target_history);
                    existing->maximum_output_event_count = std::max(
                        existing->maximum_output_event_count,
                        materialized_events->output_writes);
                    existing->maximum_source_event_reads = std::max(
                        existing->maximum_source_event_reads,
                        materialized_events->source_reads);
                } else {
                    auto derived = append_representation(
                        source_group_index,
                        connection.target_type,
                        plan.ports[target_representation]
                            .event_capacity);
                    if (!derived) {
                        return std::unexpected(std::move(derived.error()));
                    }
                    consumer_representation = *derived;
                    plan.materializations.push_back(EventMaterializationPlan{
                        .source_representation = target_representation,
                        .target_representation = consumer_representation,
                        .conversion = connection.conversion,
                        .history_samples = connection.target_history,
                        .maximum_output_event_count =
                            materialized_events->output_writes,
                        .maximum_source_event_reads =
                            materialized_events->source_reads,
                        .select_invocation_window = true,
                        .scope = scope,
                    });
                }
            }
            auto& target_binding =
                plan.primitives[*target_primitive].inputs[target_id.port];
            if (target_binding.storage) {
                return std::unexpected(
                    "GraphJit event feedback consumer input is connected more than once");
            }
            target_binding.storage = consumer_representation;
        }
    }

    for (auto const& connection : connections.event_connections) {
        if (!connection.requires_source_composition) continue;
        if (!is_entirely_realtime_delivery(connection)
            || connection.external_boundary) {
            return std::unexpected(
                "GraphJit composed event detach requires internal realtime transport");
        }
        auto const group_it = std::ranges::find_if(
            connections.event_producer_groups,
            [&](EventProducerGroupPlan const& group) {
                return group.source_type == connection.source_type
                    && group.sources == connection.sources
                    && source_conversions_match(group, connection);
            });
        if (group_it == connections.event_producer_groups.end()) {
            return std::unexpected(
                "GraphJit composed event detach lost its producer group");
        }
        auto const group_index = static_cast<std::size_t>(std::distance(
            connections.event_producer_groups.begin(), group_it));
        if (group_index >= plan.producer_group_representations.size()
            || !plan.producer_group_representations[group_index]) {
            return std::unexpected(
                "GraphJit composed event detach has no producer representation");
        }
        auto const canonical =
            *plan.producer_group_representations[group_index];
        if (canonical >= plan.ports.size()
            || !plan.ports[canonical].has_source_indices) {
            return std::unexpected(
                "GraphJit composed event detach requires ordered fan-in storage");
        }
        if (group_index
                >= plan.producer_group_source_representations.size()
            || plan.producer_group_source_representations[group_index].size()
                != connection.sources.size()) {
            return std::unexpected(
                "GraphJit composed event detach has no per-source producer representations");
        }
        for (auto const source_id : connection.sources) {
            if (source_id.bundle
                    >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[
                    source_id.bundle]) {
                return std::unexpected(
                    "GraphJit composed event source has no schedule position");
            }
        }

        std::vector<std::optional<std::size_t>> delayed_sources(
            connection.source_plans.size());
        for (std::size_t source_index = 0;
             source_index < connection.source_plans.size();
             ++source_index) {
            auto const& source = connection.source_plans[source_index];
            if (!source.detach) continue;
            auto const source_representation =
                plan.producer_group_source_representations[
                    group_index][source_index];
            if (!source_representation) {
                return std::unexpected(
                    "GraphJit composed event detach lost its source representation");
            }
            auto const source_id = connection.sources[source_index];
            auto delayed = plan_source_event_feedback(
                connection,
                source,
                source_index,
                group_index,
                *source_representation,
                *connections.schedule.bundle_execution_position[
                    source_id.bundle]);
            if (!delayed)
                return std::unexpected(std::move(delayed.error()));
            delayed_sources[source_index] = *delayed;
        }

        auto const identity = EventConversionRegistry::plan(
            connection.source_type, connection.source_type);
        for (auto const target_id : connection.targets) {
            auto const target_primitive = primitive_index_for_bundle(
                target_id.bundle);
            if (!target_primitive
                || target_id.port
                    >= plan.primitives[*target_primitive].inputs.size()
                || target_id.bundle
                    >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[
                    target_id.bundle]) {
                return std::unexpected(
                    "GraphJit composed event detach requires a scheduled concrete consumer");
            }
            NodeBundlePortId const target_port{
                target_id.bundle, PortKind::event, target_id.port};
            auto const target = input.graph.node_bundles
                .resolve_event_input(target_port).config;
            if (!is_sequential(target.access)
                || target.type != connection.target_type) {
                return std::unexpected(
                    "GraphJit composed event consumer disagrees with its declaration");
            }
            auto const scope = primitive_scope(
                *connections.schedule.bundle_execution_position[
                    target_id.bundle],
                EventOperationPhase::before);

            std::vector<std::size_t> branch_representations;
            branch_representations.reserve(connection.source_plans.size());
            std::size_t merged_capacity = 0;
            for (std::size_t source_index = 0;
                 source_index < connection.source_plans.size();
                 ++source_index) {
                auto const& source = connection.source_plans[source_index];
                auto const source_representation = delayed_sources[source_index]
                    .value_or(canonical);
                auto const source_filter = delayed_sources[source_index]
                    ? std::optional<std::size_t>{}
                    : std::optional<std::size_t>{source_index};
                if (source_representation >= plan.ports.size()) {
                    return std::unexpected(
                        "GraphJit composed event branch references invalid storage");
                }
                auto const branch_capacity =
                    plan.ports[source_representation].event_capacity;
                if (merged_capacity
                    > std::numeric_limits<std::size_t>::max()
                        - branch_capacity) {
                    return std::unexpected(
                        "GraphJit composed event capacity overflows size_t");
                }
                merged_capacity += branch_capacity;

                auto existing = std::ranges::find_if(
                    plan.materializations,
                    [&](EventMaterializationPlan const& candidate) {
                        return candidate.source_representation
                                == source_representation
                            && candidate.source_index_filter == source_filter
                            && candidate.conversion == identity
                            && candidate.scope == scope
                            && candidate.before_merge
                            && candidate.target_representation
                                < plan.ports.size()
                            && plan.ports[candidate.target_representation].type
                                == connection.source_type;
                    });
                if (existing != plan.materializations.end()) {
                    branch_representations.push_back(
                        existing->target_representation);
                    continue;
                }
                auto materialized_events = materialization_event_bound(
                    source.max_events_per_index,
                    connection.target_history,
                    scope,
                    true,
                    branch_capacity);
                if (!materialized_events) {
                    return std::unexpected(
                        std::move(materialized_events.error()));
                }
                auto branch = append_representation(
                    group_index,
                    connection.source_type,
                    branch_capacity);
                if (!branch)
                    return std::unexpected(std::move(branch.error()));
                branch_representations.push_back(*branch);
                plan.materializations.push_back(EventMaterializationPlan{
                    .source_representation = source_representation,
                    .target_representation = *branch,
                    .conversion = identity,
                    .source_index_filter = source_filter,
                    .history_samples = connection.target_history,
                    .maximum_output_event_count =
                        materialized_events->output_writes,
                    .maximum_source_event_reads =
                        materialized_events->source_reads,
                    .select_invocation_window = true,
                    .before_merge = true,
                    .scope = scope,
                });
            }

            auto rounded_capacity = rounded_event_capacity(merged_capacity);
            if (!rounded_capacity)
                return std::unexpected(std::move(rounded_capacity.error()));
            auto merged = append_representation(
                group_index,
                connection.source_type,
                *rounded_capacity,
                false,
                false,
                {},
                false,
                true);
            if (!merged)
                return std::unexpected(std::move(merged.error()));
            EventMergePlan merge{
                .source_representations =
                    std::move(branch_representations),
                .target_representation = *merged,
                .scope = scope,
                .ordering_history_samples = connection.target_history,
            };
            merge.source_indices.resize(connection.source_plans.size());
            std::iota(
                merge.source_indices.begin(), merge.source_indices.end(), 0);
            plan.merges.push_back(std::move(merge));

            auto consumer_representation = *merged;
            if (connection.conversion.size() != 0) {
                auto converted = append_representation(
                    group_index,
                    connection.target_type,
                    *rounded_capacity);
                if (!converted)
                    return std::unexpected(std::move(converted.error()));
                auto materialized_events = materialization_event_bound(
                    connection.max_events_per_index,
                    connection.target_history,
                    scope,
                    true,
                    *rounded_capacity);
                if (!materialized_events) {
                    return std::unexpected(
                        std::move(materialized_events.error()));
                }
                consumer_representation = *converted;
                plan.materializations.push_back(EventMaterializationPlan{
                    .source_representation = *merged,
                    .target_representation = *converted,
                    .conversion = connection.conversion,
                    .history_samples = connection.target_history,
                    .maximum_output_event_count =
                        materialized_events->output_writes,
                    .maximum_source_event_reads =
                        materialized_events->source_reads,
                    .select_invocation_window = true,
                    .scope = scope,
                });
            }
            auto& target_binding =
                plan.primitives[*target_primitive].inputs[target_id.port];
            if (target_binding.storage) {
                return std::unexpected(
                    "GraphJit composed event consumer input is connected more than once");
            }
            target_binding.storage = consumer_representation;
        }
    }

    if (!decisions) {
        // The concrete operation graph is now complete. Cost every shared
        // conversion once, charge ring reads only to the full-NodeStorage
        // candidate whose source actually lives in a ring, and charge each
        // shared delayed stream once regardless of consumer fanout.
        std::vector<RealtimeStorageOperationCounts> group_operations(
            connections.event_producer_groups.size());
        std::vector<EventConnectionStorageRequirements> feedback_requirements;
        feedback_requirements.reserve(plan.feedback_operations.size());
        for (auto const& feedback : plan.feedback_operations) {
            feedback_requirements.push_back(feedback.storage_requirements);
            if (feedback.source_representation >= plan.ports.size()) {
                return std::unexpected(
                    "GraphJit event costing found an invalid feedback source");
            }
            auto source_group = plan.ports[
                feedback.source_representation].producer_group_index;
            if (source_group >= group_operations.size()
                && feedback.target_representation < plan.ports.size()) {
                source_group = plan.ports[
                    feedback.target_representation].producer_group_index;
            }
            if (source_group >= group_operations.size()) {
                return std::unexpected(
                    "GraphJit event costing lost a feedback producer group");
            }
            group_operations[source_group].full_ring_addressed_values =
                iv::detail::saturating_add(
                    group_operations[source_group].full_ring_addressed_values,
                    feedback.source_event_reads);
        }

        for (auto const& materialization : plan.materializations) {
            auto const feedback = std::ranges::find_if(
                plan.feedback_operations,
                [&](EventFeedbackPlan const& candidate) {
                    return candidate.target_representation
                        == materialization.source_representation;
                });
            if (feedback != plan.feedback_operations.end()) {
                auto const feedback_index = static_cast<std::size_t>(
                    std::distance(plan.feedback_operations.begin(), feedback));
                auto& operations =
                    feedback_requirements[feedback_index].operations;
                operations.invariant_copied_values =
                    iv::detail::saturating_add(
                        operations.invariant_copied_values,
                        materialization.maximum_output_event_count);
                operations.full_ring_addressed_values =
                    iv::detail::saturating_add(
                        operations.full_ring_addressed_values,
                        materialization.maximum_source_event_reads);
                continue;
            }

            if (materialization.source_representation
                >= plan.ports.size()) {
                return std::unexpected(
                    "GraphJit event costing found an invalid materialization source");
            }
            auto source_group = plan.ports[
                materialization.source_representation].producer_group_index;
            if (source_group >= group_operations.size()
                && materialization.target_representation
                    < plan.ports.size()) {
                source_group = plan.ports[
                    materialization.target_representation]
                    .producer_group_index;
            }
            if (source_group >= group_operations.size()) {
                return std::unexpected(
                    "GraphJit event costing lost a materialization producer group");
            }
            auto& operations = group_operations[source_group];
            operations.invariant_copied_values = iv::detail::saturating_add(
                operations.invariant_copied_values,
                materialization.maximum_output_event_count);
            operations.full_ring_addressed_values =
                iv::detail::saturating_add(
                    operations.full_ring_addressed_values,
                    materialization.maximum_source_event_reads);
        }

        auto add_operations = [](EventConnectionStorageRequirements requirements,
                                 RealtimeStorageOperationCounts const& extra) {
            auto& operations = requirements.operations;
            operations.invariant_copied_values = iv::detail::saturating_add(
                operations.invariant_copied_values,
                extra.invariant_copied_values);
            operations.transient_extra_copied_values =
                iv::detail::saturating_add(
                    operations.transient_extra_copied_values,
                    extra.transient_extra_copied_values);
            operations.carry_extra_copied_values = iv::detail::saturating_add(
                operations.carry_extra_copied_values,
                extra.carry_extra_copied_values);
            operations.full_extra_copied_values = iv::detail::saturating_add(
                operations.full_extra_copied_values,
                extra.full_extra_copied_values);
            operations.full_ring_addressed_values = iv::detail::saturating_add(
                operations.full_ring_addressed_values,
                extra.full_ring_addressed_values);
            operations.transient_extra_stack_values =
                iv::detail::saturating_add(
                    operations.transient_extra_stack_values,
                    extra.transient_extra_stack_values);
            operations.carry_extra_stack_values = iv::detail::saturating_add(
                operations.carry_extra_stack_values,
                extra.carry_extra_stack_values);
            operations.full_extra_stack_values = iv::detail::saturating_add(
                operations.full_extra_stack_values,
                extra.full_extra_stack_values);
            return requirements;
        };
        auto selected_cost = [](EventConnectionStoragePlan const& candidate)
            -> std::optional<std::size_t> {
            auto const& cost = candidate.candidate_costs.for_kind(
                candidate.kind);
            if (!cost.legal) return std::nullopt;
            return cost.weighted_cost;
        };

        EventPlanningDecisions selected;
        selected.producer_groups.reserve(group_cost_requirements.size());
        for (std::size_t group_index = 0;
             group_index < group_cost_requirements.size(); ++group_index) {
            if (!connections.event_producer_groups[group_index]
                     .has_realtime_connections) {
                selected.producer_groups.emplace_back();
                continue;
            }
            auto group_requirements = add_operations(
                group_cost_requirements[group_index],
                group_operations[group_index]);
            auto const group_storage = choose_event_connection_storage_plan(
                group_requirements, cost_model);
            auto const group_cost = selected_cost(group_storage);
            if (!group_cost) {
                return std::unexpected(
                    "GraphJit event operations have no storage realization within the compile-time stack budget");
            }
            selected.producer_groups.push_back(EventGroupPlanningDecision{
                .storage = group_storage,
            });
        }

        selected.feedback.reserve(feedback_requirements.size());
        for (auto const& requirements : feedback_requirements) {
            auto const storage = choose_event_connection_storage_plan(
                requirements, cost_model);
            if (!selected_cost(storage)) {
                return std::unexpected(
                    "GraphJit event feedback has no storage realization within the compile-time stack budget");
            }
            selected.feedback.push_back(EventFeedbackPlanningDecision{
                .requirements = requirements,
                .storage = storage,
            });
        }
        return plan_event_ports_once(
            input, analysis, connections, realization, cost_model, &selected);
    }
    if (plan.feedback_operations.size() != decisions->feedback.size()) {
        return std::unexpected(
            "GraphJit event costing decisions disagree with feedback operations");
    }

    // Complete disconnected realtime ports explicitly. Inputs supplied by
    // background evaluation bind their TickInvocationCall slot; disconnected
    // inputs bind the immutable empty sequence of their type. Outputs use the
    // same transient/carry/ring policy as a connected stream: disconnection
    // removes delivery, not an authored Tick output's own history/latency
    // state.
    for (std::size_t primitive_index = 0;
        primitive_index < plan.primitives.size(); ++primitive_index) {
        if (analysis.primitives[primitive_index].implementation->tick_block
            == nullptr) {
            continue;
        }
        auto const bundle = analysis.primitives[primitive_index].bundle.node_bundle;

        for (std::size_t port = 0;
             port < plan.primitives[primitive_index].inputs.size(); ++port) {
            auto& binding = plan.primitives[primitive_index].inputs[port];
            if (binding.storage) continue;
            auto invocation_supplied = external_tick_sequential_slot(
                connections, bundle, PortKind::event, port);
            if (!invocation_supplied) {
                return std::unexpected(
                    std::move(invocation_supplied.error()));
            }
            if (*invocation_supplied) {
                binding.invocation_supplied = true;
                auto const& runtime_binding = connections.background
                    .tick_runtime.sequential_event_inputs[
                        **invocation_supplied];
                if (runtime_binding.port >= connections.background.ports.size()) {
                    return std::unexpected(
                        "GraphJit invocation-supplied event input lost its logical port");
                }
                auto const& logical =
                    connections.background.ports[runtime_binding.port];
                if (logical.sequential_history
                    > std::numeric_limits<std::size_t>::max()
                        - input.specialization.block_size) {
                    return std::unexpected(
                        "GraphJit invocation-supplied event window overflows size_t");
                }
                double maximum_rate = 0.0;
                for (auto const storage : runtime_binding.storage) {
                    if (storage >= connections.background.storage.ports.size()) {
                        return std::unexpected(
                            "GraphJit invocation-supplied event input references missing storage");
                    }
                    maximum_rate = std::max(
                        maximum_rate,
                        connections.background.storage.ports[storage]
                            .max_events_per_index);
                }
                // Without history the callback reads the slot's current
                // window directly.
                if (logical.sequential_history == 0) continue;
                auto const capacity = event_sequence_capacity_for_sample_span(
                    maximum_rate,
                    input.specialization.block_size
                        + logical.sequential_history);
                if (!capacity) {
                    return std::unexpected(
                        "GraphJit invocation-supplied event capacity is not representable");
                }
                auto timeline = append_input_timeline(
                    binding,
                    EventInputPortId{.bundle = bundle, .port = port},
                    logical.event_type,
                    logical.sequential_history,
                    *capacity);
                if (!timeline) {
                    return std::unexpected(std::move(timeline.error()));
                }
                continue;
            }
            NodeBundlePortId const port_id{bundle, PortKind::event, port};
            auto const config = input.graph.node_bundles
                .resolve_event_input(port_id).config;
            // See the Random Access case in plan_sample_ports().
            if (!is_sequential(config)) {
                binding.random_access = true;
                continue;
            }
            auto empty = immutable_empty_representation(config.type);
            if (!empty) return std::unexpected(std::move(empty.error()));
            binding.storage = *empty;
        }

        for (std::size_t port = 0;
             port < plan.primitives[primitive_index].outputs.size(); ++port) {
            auto& binding = plan.primitives[primitive_index].outputs[port];
            if (!binding.realtime) continue;
            if (binding.storage) continue;
            auto authored = ensure_raw_event_output(EventOutputPortId{
                .bundle = bundle,
                .port = port,
            });
            if (!authored) {
                return std::unexpected(std::move(authored.error()));
            }
        }
    }

    // Derive stack-buffer lifetimes from the operations that actually access
    // each event sequence. Producer-group intervals are intentionally not used
    // here: a conversion result, merge input, or feedback working buffer often
    // exists for only a small suffix of the producer group's full interval.
    std::vector<std::optional<ConnectionLiveIntervalPlan>> stack_buffer_lifetimes(
        plan.ports.size());

    std::vector<std::size_t> primitive_execution_positions(
        analysis.primitives.size());
    for (std::size_t primitive_index = 0;
         primitive_index < analysis.primitives.size(); ++primitive_index) {
        if (analysis.primitives[primitive_index].implementation->tick_block
            == nullptr) {
            continue;
        }
        auto const bundle = analysis.primitives[primitive_index].bundle.node_bundle;
        if (bundle >= connections.schedule.bundle_execution_position.size()
            || !connections.schedule.bundle_execution_position[bundle]) {
            return std::unexpected(
                "GraphJit event stack lifetime planning lost a primitive schedule position");
        }
        primitive_execution_positions[primitive_index] =
            *connections.schedule.bundle_execution_position[bundle];
    }

    struct EventRegionBounds {
        bool cyclic = false;
        std::size_t begin = 0;
        std::size_t end = 0;
    };
    std::vector<EventRegionBounds> region_bounds(
        connections.schedule.regions.size());
    std::vector<std::optional<std::size_t>> execution_position_region(
        analysis.primitives.size());
    for (std::size_t region_index = 0;
         region_index < connections.schedule.regions.size(); ++region_index) {
        auto const& region = connections.schedule.regions[region_index];
        if (region.execution_order.empty()) continue;
        auto begin = std::numeric_limits<std::size_t>::max();
        std::size_t end = 0;
        for (auto const bundle : region.execution_order) {
            if (bundle >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[bundle]) {
                return std::unexpected(
                    "GraphJit event stack lifetime planning lost a region schedule position");
            }
            auto const position =
                *connections.schedule.bundle_execution_position[bundle];
            begin = std::min(begin, position);
            end = std::max(end, position);
            if (position >= execution_position_region.size()) {
                return std::unexpected(
                    "GraphJit event stack lifetime planning found an invalid execution position");
            }
            execution_position_region[position] = region_index;
        }
        region_bounds[region_index] = EventRegionBounds{
            .cyclic = region.cyclic,
            .begin = begin,
            .end = end,
        };
    }

    auto touch_buffer = [&](std::size_t representation_index,
                                    std::size_t begin,
                                    std::size_t end)
        -> std::expected<void, std::string> {
        if (representation_index >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit event stack lifetime references an invalid buffer");
        }
        if (begin > end) {
            return std::unexpected(
                "GraphJit event stack lifetime has an inverted schedule interval");
        }
        if (plan.ports[representation_index].persistent) return {};
        auto& live = stack_buffer_lifetimes[representation_index];
        if (!live) {
            live = ConnectionLiveIntervalPlan{
                .begin = begin,
                .end = end,
                .crosses_kernel_invocations = false,
            };
        } else {
            live->begin = std::min(live->begin, begin);
            live->end = std::max(live->end, end);
        }
        return {};
    };
    auto touch_at = [&](std::size_t representation_index,
                        std::size_t position)
        -> std::expected<void, std::string> {
        return touch_buffer(
            representation_index, position, position);
    };
    auto scope_position = [&](EventOperationScope scope)
        -> std::expected<std::size_t, std::string> {
        if (scope.kind == EventOperationScopeKind::primitive) {
            if (scope.index >= analysis.primitives.size()) {
                return std::unexpected(
                    "GraphJit event operation has an invalid primitive scope");
            }
            return scope.index;
        }
        if (scope.index >= region_bounds.size()
            || connections.schedule.regions[scope.index].execution_order.empty()) {
            return std::unexpected(
                "GraphJit event operation has an invalid region scope");
        }
        auto const& bounds = region_bounds[scope.index];
        return scope.phase == EventOperationPhase::before
            ? bounds.begin
            : bounds.end;
    };

    // Primitive callbacks are the direct writers/readers of event buffers.
    for (std::size_t primitive_index = 0;
         primitive_index < plan.primitives.size(); ++primitive_index) {
        auto const position = primitive_execution_positions[primitive_index];
        for (auto const& output : plan.primitives[primitive_index].outputs) {
            if (!output.storage) continue;
            auto begin = position;
            auto end = position;
            // An append-existing output in an SCC accumulates one sequence
            // across every slice of the root invocation. Keep that buffer live
            // across the whole SCC; ordinary per-slice materialization buffers
            // remain eligible for tighter reuse.
            if (output.append_existing
                && position < execution_position_region.size()
                && execution_position_region[position]) {
                auto const& bounds =
                    region_bounds[*execution_position_region[position]];
                if (bounds.cyclic) {
                    begin = bounds.begin;
                    end = bounds.end;
                }
            }
            if (auto touched = touch_buffer(
                    *output.storage, begin, end);
                !touched) {
                return std::unexpected(std::move(touched.error()));
            }
        }
        for (auto const& input_binding : plan.primitives[primitive_index].inputs) {
            if (input_binding.storage) {
                if (auto touched = touch_at(*input_binding.storage, position);
                    !touched) {
                    return std::unexpected(std::move(touched.error()));
                }
            }
            for (auto const operand :
                 {input_binding.realtime_operand_storage,
                  input_binding.combined_storage}) {
                if (!operand) continue;
                if (auto touched = touch_at(*operand, position); !touched) {
                    return std::unexpected(std::move(touched.error()));
                }
            }
        }
    }

    // Restore happens before the producer step (or before the complete cyclic
    // region); commit reads the same working buffer after the producer step (or
    // after the complete cyclic region).
    for (auto const& carry : plan.carry_operations) {
        auto const begin = scope_position(carry.restore_scope);
        auto const end = scope_position(carry.commit_scope);
        if (!begin) return std::unexpected(std::move(begin.error()));
        if (!end) return std::unexpected(std::move(end.error()));
        if (auto touched = touch_buffer(
                carry.working_representation,
                std::min(*begin, *end),
                std::max(*begin, *end));
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
    }

    // A merge reads every source and writes/extends the target after the last
    // producer. Earlier producer-local buffers therefore end exactly at the
    // merge step instead of at the producer group's last consumer.
    for (auto const& merge : plan.merges) {
        auto const operation_position = scope_position(merge.scope);
        if (!operation_position) {
            return std::unexpected(std::move(operation_position.error()));
        }
        for (auto const source_representation : merge.source_representations) {
            if (auto touched = touch_at(
                    source_representation, *operation_position);
                !touched) {
                return std::unexpected(std::move(touched.error()));
            }
        }
        auto target_begin = *operation_position;
        auto target_end = *operation_position;
        if (merge.preserve_existing_target
            && merge.scope.kind == EventOperationScopeKind::primitive
            && *operation_position < execution_position_region.size()
            && execution_position_region[*operation_position]) {
            auto const& bounds = region_bounds[
                *execution_position_region[*operation_position]];
            if (bounds.cyclic) {
                // The merge extends this sequence once per SCC slice. A
                // linear interval beginning at the merge position does not
                // cover the earlier primitive positions on the next loop
                // iteration, so another transient allocation could overwrite
                // the accumulated prefix before the following merge.
                target_begin = bounds.begin;
                target_end = bounds.end;
            }
        }
        if (auto touched = touch_buffer(
                merge.target_representation, target_begin, target_end);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
    }

    // Scope is part of the storage operation. Lifetime planning therefore
    // consumes the selected slice/root-call boundary directly and never has to
    // reverse-engineer it from storage consumers.
    for (auto const& materialization : plan.materializations) {
        auto const operation_position = scope_position(materialization.scope);
        if (!operation_position) {
            return std::unexpected(std::move(operation_position.error()));
        }
        if (auto touched = touch_at(
                materialization.source_representation, *operation_position);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
        if (auto touched = touch_at(
                materialization.target_representation, *operation_position);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
    }

    // Feedback appends read the source and write the delayed buffer after the
    // producer. The target is then consumed on a later SCC slice, so a stack
    // buffer written inside an SCC crosses the loop back-edge. A linear
    // first/last schedule interval cannot express that wrapped lifetime; keep
    // the target allocated for the complete SCC invocation. Retained feedback
    // buffers are already widened by their restore/commit operations above.
    for (auto const& feedback : plan.feedback_operations) {
        auto const operation_position = scope_position(feedback.append_scope);
        if (!operation_position) {
            return std::unexpected(std::move(operation_position.error()));
        }
        if (auto touched = touch_at(
                feedback.source_representation,
                *operation_position);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }

        auto target_begin = *operation_position;
        auto target_end = *operation_position;
        if (feedback.append_scope.kind == EventOperationScopeKind::region) {
            if (feedback.append_scope.index >= region_bounds.size()) {
                return std::unexpected(
                    "GraphJit event feedback has an invalid region scope");
            }
            auto const& bounds = region_bounds[feedback.append_scope.index];
            if (bounds.cyclic) {
                target_begin = bounds.begin;
                target_end = bounds.end;
            }
        } else if (*operation_position < execution_position_region.size()
                   && execution_position_region[*operation_position]) {
            auto const& bounds = region_bounds[
                *execution_position_region[*operation_position]];
            if (bounds.cyclic) {
                target_begin = bounds.begin;
                target_end = bounds.end;
            }
        }
        if (auto touched = touch_buffer(
                feedback.target_representation,
                target_begin,
                target_end);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
    }

    std::vector<TransientArenaAllocationRequest> transient_requests;
    std::vector<std::size_t> transient_representations;
    transient_requests.reserve(plan.ports.size());
    transient_representations.reserve(plan.ports.size());
    for (std::size_t representation_index = 0;
         representation_index < plan.ports.size();
         ++representation_index) {
        auto const& storage = plan.ports[representation_index];
        if (storage.persistent || storage.immutable_empty) continue;
        if (!stack_buffer_lifetimes[representation_index]) {
            return std::unexpected(
                "GraphJit transient event buffer has no scheduled reader or writer");
        }
        transient_requests.push_back(TransientArenaAllocationRequest{
            .size_bytes = storage.size_bytes,
            .alignment = storage.alignment,
            .live_interval = *stack_buffer_lifetimes[representation_index],
        });
        transient_representations.push_back(representation_index);
    }
    auto arena = plan_transient_arena(transient_requests);
    if (!arena) return std::unexpected(std::move(arena.error()));
    plan.transient_arena_size = arena->size_bytes;
    plan.transient_arena_alignment = arena->alignment;
    plan.transient_allocations.reserve(arena->allocations.size());
    for (std::size_t request_index = 0;
         request_index < arena->allocations.size();
         ++request_index) {
        auto const representation_index = transient_representations[request_index];
        auto const& allocation = arena->allocations[request_index];
        auto const allocation_index = plan.transient_allocations.size();
        plan.transient_allocations.push_back(EventTransientAllocationPlan{
            .representation_index = representation_index,
            .size_bytes = allocation.size_bytes,
            .alignment = allocation.alignment,
            .region_relative_offset = allocation.offset,
            .live_interval = transient_requests[request_index].live_interval,
        });
        plan.ports[representation_index].transient_allocation =
            allocation_index;
    }

    return plan;
}

std::expected<EventPortBindingPlan, std::string> plan_event_ports(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    ResolvedPortRealization const& realization,
    RealtimeStorageCostModel const& cost_model)
{
    return plan_event_ports_once(
        input, analysis, connections, realization, cost_model, nullptr);
}

std::expected<ExecutionPlan, std::string> plan_execution(
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    DeclarationPlan const& declarations,
    PackageImportPlan const& imports,
    SamplePortBindingPlan const& sample_ports,
    EventPortBindingPlan const& event_ports)
{
    if (analysis.empty) {
        return ExecutionPlan{};
    }
    if (declarations.primitive_storage.size() != analysis.primitives.size()) {
        return std::unexpected(
            "GraphJit lowering lost a canonical primitive storage plan");
    }
    if (imports.primitive_callbacks.size() != analysis.primitives.size()) {
        return std::unexpected(
            "GraphJit lowering lost a primitive callback import plan");
    }

    ExecutionPlan plan{};
    plan.primitive_steps.reserve(analysis.primitives.size());
    plan.regions.reserve(connections.schedule.regions.size());
    std::vector<std::optional<std::size_t>> schedule_region_execution_region(
        connections.schedule.regions.size());

    // Keep one flattened primitive-step namespace because all storage plans
    // refer to producer execution positions in that namespace. Tock-only
    // primitives have no position in it. Execution regions add the information
    // LLVM realization needs to switch cyclic SCCs from the ordinary
    // primitive-major traversal to slice-major traversal. Detach is connection
    // metadata, so there are no synthetic detach primitives.
    std::vector<bool> scheduled(analysis.primitives.size(), false);
    for (auto const region_index : connections.schedule.region_order) {
        if (region_index >= connections.schedule.regions.size()) {
            return std::unexpected(
                "GraphJit connection schedule contains an invalid region index");
        }
        auto const& source_region = connections.schedule.regions[region_index];
        auto const execution_region_index = plan.regions.size();
        ExecutionRegionPlan region{
            .cyclic = source_region.cyclic,
            .maximum_block_size = source_region.maximum_block_size,
            .scc_feedback_latency = source_region.scc_feedback_latency,
        };
        for (auto const bundle : source_region.execution_order) {
            auto const primitive = std::ranges::find_if(
                analysis.primitives,
                [&](PrimitiveAnalysis const& candidate) {
                    return candidate.bundle.node_bundle == bundle;
                });
            if (primitive == analysis.primitives.end()) {
                return std::unexpected(
                    "GraphJit connection schedule contains a non-primitive bundle");
            }
            auto const i = static_cast<std::size_t>(
                std::distance(analysis.primitives.begin(), primitive));
            if (scheduled[i]) {
                return std::unexpected(
                    "GraphJit connection schedule contains a primitive more than once");
            }
            if (imports.primitive_callbacks[i].tick_block.empty()) {
                return std::unexpected(
                    "GraphJit connection schedule contains a Tock-only primitive");
            }
            scheduled[i] = true;
            auto const& callbacks = imports.primitive_callbacks[i];
            auto const step_index = plan.primitive_steps.size();
            plan.primitive_steps.push_back(PrimitiveExecutionStep{
                .configuration_index = i,
                .storage_index = i,
                .maximum_block_size = primitive->bundle.maximum_block_size,
                .tick_callback_symbol = callbacks.tick_block,
                .tick_batch_callback_symbol = callbacks.tick_block_batch,
                .skip_callback_symbol = callbacks.skip_block,
                .skip_batch_callback_symbol = callbacks.skip_block_batch,
            });
            region.primitive_steps.push_back(step_index);
        }
        if (region.cyclic && region.primitive_steps.empty()) {
            return std::unexpected(
                "GraphJit cyclic execution region contains no executable primitive");
        }
        if (!region.primitive_steps.empty()) {
            schedule_region_execution_region[region_index] = execution_region_index;
            plan.regions.push_back(std::move(region));
        }
    }
    plan.ready_groups.reserve(connections.schedule.ready_groups.size());
    for (auto const& scheduled_group : connections.schedule.ready_groups) {
        if (scheduled_group.empty()) {
            return std::unexpected(
                "GraphJit connection schedule contains an empty ready group");
        }
        ExecutionReadyGroupPlan group;
        for (auto const schedule_region : scheduled_group) {
            if (schedule_region >= schedule_region_execution_region.size()
                || !schedule_region_execution_region[schedule_region]) {
                return std::unexpected(
                    "GraphJit ready group references a missing execution region");
            }
            auto const execution_region =
                *schedule_region_execution_region[schedule_region];
            auto const& region = plan.regions[execution_region];
            if (region.cyclic) {
                if (scheduled_group.size() != 1 || group.cyclic_region
                    || !group.primitive_steps.empty()) {
                    return std::unexpected(
                        "GraphJit cyclic SCC was placed in a realtime batch group");
                }
                group.cyclic_region = execution_region;
                continue;
            }
            if (region.primitive_steps.size() != 1 || group.cyclic_region) {
                return std::unexpected(
                    "GraphJit acyclic ready group has an invalid execution region");
            }
            group.primitive_steps.push_back(region.primitive_steps.front());
        }
        if (group.primitive_steps.size() > 1) {
            auto const& first = plan.primitive_steps[group.primitive_steps.front()];
            for (auto const step_index : group.primitive_steps) {
                auto const& step = plan.primitive_steps[step_index];
                if (step.tick_batch_callback_symbol.empty()
                    || step.tick_batch_callback_symbol
                        != first.tick_batch_callback_symbol
                    || step.maximum_block_size != first.maximum_block_size) {
                    return std::unexpected(
                        "GraphJit ready group contains incompatible batch callbacks");
                }
            }
        }
        plan.ready_groups.push_back(std::move(group));
    }
    if (plan.ready_groups.empty() && !plan.regions.empty()) {
        return std::unexpected(
            "GraphJit connection schedule lost its ready-frontier groups");
    }
    for (std::size_t i = 0; i < scheduled.size(); ++i) {
        auto const tick = !imports.primitive_callbacks[i].tick_block.empty();
        if (scheduled[i] != tick) {
            return std::unexpected(tick
                ? "GraphJit connection schedule omitted a Tick primitive"
                : "GraphJit connection schedule included a Tock-only primitive");
        }
    }

    // Map flattened execution positions back to their region before assigning
    // storage event operations. Operations whose semantic lifetime is one
    // complete cyclic root invocation belong to the region boundary rather
    // than to an individual primitive slice.
    std::vector<std::optional<std::size_t>> step_regions(
        plan.primitive_steps.size());
    std::vector<std::optional<std::size_t>> configuration_steps(
        analysis.primitives.size());
    for (std::size_t region_index = 0; region_index < plan.regions.size();
         ++region_index) {
        for (auto const step_index : plan.regions[region_index].primitive_steps) {
            if (step_index >= plan.primitive_steps.size()) {
                return std::unexpected(
                    "GraphJit execution region references an invalid primitive step");
            }
            step_regions[step_index] = region_index;
            auto const configuration_index =
                plan.primitive_steps[step_index].configuration_index;
            if (configuration_index >= configuration_steps.size()) {
                return std::unexpected(
                    "GraphJit execution step references an invalid primitive configuration");
            }
            configuration_steps[configuration_index] = step_index;
        }
    }

    for (std::size_t carry_index = 0;
         carry_index < sample_ports.storage.carry_operations.size();
         ++carry_index) {
        auto const& carry = sample_ports.storage.carry_operations[carry_index];
        if (carry.restore_execution_position >= plan.primitive_steps.size()
            || carry.commit_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit sample carry operation references an invalid execution position");
        }
        plan.primitive_steps[carry.restore_execution_position]
            .sample_carry_restores_before.push_back(carry_index);
        plan.primitive_steps[carry.commit_execution_position]
            .sample_carry_commits_after.push_back(carry_index);
    }

    for (std::size_t timeline_index = 0;
         timeline_index < sample_ports.storage.feedback_timelines.size();
         ++timeline_index) {
        auto const& timeline = sample_ports.storage.feedback_timelines[timeline_index];
        if (timeline.writer.kind
            == SampleFeedbackTimelineWriterKind::producer_home) {
            continue;
        }
        if (timeline.writer.after_execution_position
            >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit sample feedback timeline references an invalid execution position");
        }
        plan.primitive_steps[timeline.writer.after_execution_position]
            .sample_feedback_writes_after.push_back(timeline_index);
    }

    for (std::size_t materialization_index = 0;
         materialization_index < sample_ports.storage.materializations.size();
         ++materialization_index) {
        auto const& materialization =
            sample_ports.storage.materializations[materialization_index];
        if (materialization.before_execution_position) {
            if (*materialization.before_execution_position
                >= plan.primitive_steps.size()) {
                return std::unexpected(
                    "GraphJit sample materialization references an invalid before-execution position");
            }
            plan.primitive_steps[*materialization.before_execution_position]
                .sample_materializations_before.push_back(materialization_index);
            continue;
        }
        if (materialization.after_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit sample materialization references an invalid execution position");
        }
        plan.primitive_steps[materialization.after_execution_position]
            .sample_materializations_after.push_back(materialization_index);
    }

    for (std::size_t composition_index = 0;
         composition_index < sample_ports.storage.compositions.size();
         ++composition_index) {
        auto const& composition =
            sample_ports.storage.compositions[composition_index];
        if (composition.before_execution_position) {
            if (*composition.before_execution_position
                >= plan.primitive_steps.size()) {
                return std::unexpected(
                    "GraphJit sample composition references an invalid before-execution position");
            }
            plan.primitive_steps[*composition.before_execution_position]
                .sample_compositions_before.push_back(composition_index);
            continue;
        }
        if (composition.after_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit sample composition references an invalid execution position");
        }
        plan.primitive_steps[composition.after_execution_position]
            .sample_compositions_after.push_back(composition_index);
    }

    auto append_event_operation = [&] (
        EventOperationScope scope,
        EventOperationRef operation) -> std::expected<void, std::string> {
        auto const before = scope.phase == EventOperationPhase::before;
        if (scope.kind == EventOperationScopeKind::primitive) {
            if (scope.index >= plan.primitive_steps.size()) {
                return std::unexpected(
                    "GraphJit event operation references an invalid primitive scope");
            }
            auto& operations = before
                ? plan.primitive_steps[scope.index].event_operations_before
                : plan.primitive_steps[scope.index].event_operations_after;
            operations.push_back(operation);
            return {};
        }
        if (scope.index >= schedule_region_execution_region.size()
            || !schedule_region_execution_region[scope.index]) {
            return std::unexpected(
                "GraphJit event operation references an invalid schedule region scope");
        }
        auto const execution_region =
            *schedule_region_execution_region[scope.index];
        auto& operations = before
            ? plan.regions[execution_region].event_operations_before
            : plan.regions[execution_region].event_operations_after;
        operations.push_back(operation);
        return {};
    };

    for (std::size_t ring_index = 0;
         ring_index < event_ports.persistent_rings.size(); ++ring_index) {
        auto scheduled = append_event_operation(
            event_ports.persistent_rings[ring_index].prune_scope,
            {EventOperationKind::persistent_ring_prune, ring_index});
        if (!scheduled) return std::unexpected(std::move(scheduled.error()));
    }
    for (auto const& reset : event_ports.sequence_resets) {
        auto scheduled = append_event_operation(
            reset.scope,
            {EventOperationKind::sequence_reset, reset.storage});
        if (!scheduled) return std::unexpected(std::move(scheduled.error()));
    }
    for (std::size_t carry_index = 0;
         carry_index < event_ports.carry_operations.size(); ++carry_index) {
        auto const& carry = event_ports.carry_operations[carry_index];
        auto restored = append_event_operation(
            carry.restore_scope, {EventOperationKind::carry_restore, carry_index});
        if (!restored) return std::unexpected(std::move(restored.error()));
        auto committed = append_event_operation(
            carry.commit_scope, {EventOperationKind::carry_commit, carry_index});
        if (!committed) return std::unexpected(std::move(committed.error()));
    }
    for (std::size_t merge_index = 0;
         merge_index < event_ports.merges.size(); ++merge_index) {
        auto scheduled = append_event_operation(
            event_ports.merges[merge_index].scope,
            {EventOperationKind::merge, merge_index});
        if (!scheduled) return std::unexpected(std::move(scheduled.error()));
    }
    for (std::size_t materialization_index = 0;
         materialization_index < event_ports.materializations.size();
         ++materialization_index) {
        auto scheduled = append_event_operation(
            event_ports.materializations[materialization_index].scope,
            {EventOperationKind::materialize, materialization_index});
        if (!scheduled) return std::unexpected(std::move(scheduled.error()));
    }
    for (std::size_t feedback_index = 0;
         feedback_index < event_ports.feedback_operations.size();
         ++feedback_index) {
        auto const& feedback = event_ports.feedback_operations[feedback_index];
        if (feedback.storage_plan.kind
            == RealtimeBufferStorageKind::transient_stack) {
            auto reset = append_event_operation(
                feedback.reset_scope,
                {EventOperationKind::sequence_reset,
                 feedback.target_representation});
            if (!reset) return std::unexpected(std::move(reset.error()));
        }
        auto appended = append_event_operation(
            feedback.append_scope,
            {EventOperationKind::feedback_append, feedback_index});
        if (!appended) return std::unexpected(std::move(appended.error()));
    }

    // An append-existing transient output owns an invocation aggregate. Reset
    // it at the scope that owns that aggregate; carry/ring-backed sequences are
    // initialized by their own explicit operations.
    for (std::size_t primitive_index = 0;
         primitive_index < event_ports.primitives.size(); ++primitive_index) {
        if (analysis.primitives[primitive_index].implementation->tick_block
            == nullptr) {
            continue;
        }
        if (primitive_index >= configuration_steps.size()
            || !configuration_steps[primitive_index]) {
            return std::unexpected(
                "GraphJit event output is absent from the execution schedule");
        }
        auto const step_index = *configuration_steps[primitive_index];
        for (auto const& output : event_ports.primitives[primitive_index].outputs) {
            if (!output.append_existing || !output.storage) continue;
            auto const storage = *output.storage;
            if (storage >= event_ports.ports.size()) {
                return std::unexpected(
                    "GraphJit event output reset references an invalid storage");
            }
            auto const initialized_elsewhere = std::ranges::any_of(
                event_ports.carry_operations,
                [&](EventCarryPlan const& carry) {
                    return carry.working_representation == storage;
                }) || event_ports.ports[storage].persistent_ring;
            if (initialized_elsewhere) continue;
            EventOperationScope reset_scope{
                .kind = EventOperationScopeKind::primitive,
                .phase = EventOperationPhase::before,
                .index = step_index,
            };
            if (step_regions[step_index]
                && plan.regions[*step_regions[step_index]].cyclic) {
                reset_scope = EventOperationScope{
                    .kind = EventOperationScopeKind::region,
                    .phase = EventOperationPhase::before,
                    .index = *step_regions[step_index],
                };
            }
            auto const& operations = reset_scope.kind
                    == EventOperationScopeKind::region
                ? plan.regions[reset_scope.index].event_operations_before
                : plan.primitive_steps[reset_scope.index].event_operations_before;
            auto const already_scheduled = std::ranges::any_of(
                operations,
                [&](EventOperationRef operation) {
                    return operation.kind == EventOperationKind::sequence_reset
                        && operation.index == storage;
                });
            if (!already_scheduled) {
                // reset_scope is constructed from step_regions above, so a
                // region index here is already an ExecutionPlan::regions index
                // rather than a ConnectionAnalysisPlan schedule-region index.
                if (reset_scope.kind == EventOperationScopeKind::region) {
                    plan.regions[reset_scope.index].event_operations_before.push_back(
                        {EventOperationKind::sequence_reset, storage});
                } else {
                    plan.primitive_steps[reset_scope.index]
                        .event_operations_before.push_back(
                            {EventOperationKind::sequence_reset, storage});
                }
            }
        }
    }

    auto operation_rank = [&](EventOperationRef operation) {
        switch (operation.kind) {
        case EventOperationKind::sequence_reset: return 0;
        case EventOperationKind::persistent_ring_prune: return 1;
        case EventOperationKind::carry_restore: return 2;
        case EventOperationKind::materialize:
            return operation.index < event_ports.materializations.size()
                    && event_ports.materializations[operation.index]
                        .before_merge
                ? 4
                : 7;
        case EventOperationKind::merge:
            return operation.index < event_ports.merges.size()
                    && event_ports.merges[operation.index]
                        .before_materialization
                ? 3
                : 5;
        case EventOperationKind::feedback_append: return 6;
        case EventOperationKind::carry_commit: return 8;
        }
        return 9;
    };
    auto order_operations = [&](std::vector<EventOperationRef>& operations) {
        std::ranges::stable_sort(
            operations,
            [&](EventOperationRef lhs, EventOperationRef rhs) {
                return operation_rank(lhs) < operation_rank(rhs);
            });
    };
    for (auto& step : plan.primitive_steps) {
        order_operations(step.event_operations_before);
        order_operations(step.event_operations_after);
    }
    for (auto& region : plan.regions) {
        order_operations(region.event_operations_before);
        order_operations(region.event_operations_after);
        if (!region.cyclic
            && (!region.event_operations_before.empty()
                || !region.event_operations_after.empty())) {
            return std::unexpected(
                "GraphJit acyclic execution region unexpectedly owns region-scoped event operations");
        }
    }

    return plan;
}

std::expected<RootStackBufferPlan, std::string> plan_root_stack_buffers(
    SamplePortBindingPlan const& sample_ports,
    EventPortBindingPlan const& event_ports)
{
    auto align_up = [](std::size_t value, std::size_t alignment)
        -> std::optional<std::size_t> {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return std::nullopt;
        }
        auto const mask = alignment - 1;
        if (value > std::numeric_limits<std::size_t>::max() - mask) {
            return std::nullopt;
        }
        return (value + mask) & ~mask;
    };
    auto checked_total = [](std::size_t offset, std::size_t size)
        -> std::optional<std::size_t> {
        if (size > std::numeric_limits<std::size_t>::max() - offset) {
            return std::nullopt;
        }
        return offset + size;
    };

    auto const sample_size = sample_ports.storage.transient_arena_size;
    auto const sample_alignment = std::max(
        std::size_t{1}, sample_ports.storage.transient_arena_alignment);
    auto const event_size = event_ports.transient_arena_size;
    auto const event_alignment = std::max(
        std::size_t{1}, event_ports.transient_arena_alignment);

    auto sample_first_event_offset = align_up(sample_size, event_alignment);
    auto event_first_sample_offset = align_up(event_size, sample_alignment);
    if (!sample_first_event_offset || !event_first_sample_offset) {
        return std::unexpected(
            "GraphJit root stack buffer alignment overflows size_t");
    }
    auto sample_first_size = checked_total(
        *sample_first_event_offset, event_size);
    auto event_first_size = checked_total(
        *event_first_sample_offset, sample_size);
    if (!sample_first_size || !event_first_size) {
        return std::unexpected(
            "GraphJit root stack buffer size overflows size_t");
    }

    RootStackBufferPlan result{
        .size_bytes = *sample_first_size,
        .alignment = std::max(sample_alignment, event_alignment),
        .sample_offset = 0,
        .event_offset = *sample_first_event_offset,
    };
    if (*event_first_size < *sample_first_size) {
        result.size_bytes = *event_first_size;
        result.sample_offset = *event_first_sample_offset;
        result.event_offset = 0;
    }
    return result;
}
// Decides whether lowering emits one multi-step ready group as a native
// batch. The decision belongs to planning rather than LLVM emission so that
// the planner can restore the scalar schedule for a group that would only be
// reordered without being batched.
std::expected<bool, std::string> realtime_ready_group_batches(
    ConnectionAnalysisPlan const& connections,
    DeclarationPlan const& declarations,
    PackageImportPlan const& imports,
    SamplePortBindingPlan const& sample_ports,
    EventPortBindingPlan const& event_ports,
    RootStackBufferPlan const& root_stack,
    ExecutionPlan const& execution,
    ExecutionReadyGroupPlan const& group)
{
    if (group.cyclic_region || group.primitive_steps.size() < 2) return false;

    std::string_view tick_symbol;
    std::string_view skip_symbol;
    std::vector<PrimitiveExecutionStep const*> lanes;
    lanes.reserve(group.primitive_steps.size());
    for (auto const step_index : group.primitive_steps) {
        if (step_index >= execution.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit realtime batch references a missing primitive step");
        }
        auto const& step = execution.primitive_steps[step_index];
        if (step.configuration_index >= imports.primitive_callbacks.size()
            || step.configuration_index >= sample_ports.primitives.size()
            || step.configuration_index >= event_ports.primitives.size()
            || step.configuration_index
                >= declarations.primitive_activity.size()) {
            return std::unexpected(
                "GraphJit realtime batch references an incomplete primitive plan");
        }
        // TTL activity is decided per lane at runtime, so a lane with an
        // activity plan can also be invoked through the skip batch.
        auto const has_activity = declarations
            .primitive_activity[step.configuration_index].has_value();
        if (step.tick_batch_callback_symbol.empty()
            || (has_activity && step.skip_batch_callback_symbol.empty())) {
            return false;
        }
        if (tick_symbol.empty()) {
            tick_symbol = step.tick_batch_callback_symbol;
            skip_symbol = step.skip_batch_callback_symbol;
        }
        if (step.tick_batch_callback_symbol != tick_symbol
            || step.skip_batch_callback_symbol != skip_symbol
            || step.maximum_block_size < declarations.node_layout.max_block_size) {
            return false;
        }

        // These operations delimit a producer's authored callback. Moving all
        // pre-operations before one batch and all post-operations after it is
        // legal for an antichain, but it can extend transient live ranges and
        // increase stack pressure. The initial cost model keeps such groups
        // scalar; Front 16 can cost that transformation with layout jointly.
        if (!step.sample_carry_restores_before.empty()
            || !step.sample_materializations_before.empty()
            || !step.sample_compositions_before.empty()
            || !step.sample_feedback_writes_after.empty()
            || !step.sample_materializations_after.empty()
            || !step.sample_compositions_after.empty()
            || !step.sample_transition_materializations_after.empty()
            || !step.sample_carry_commits_after.empty()
            || !step.event_operations_before.empty()
            || !step.event_operations_after.empty()) {
            return false;
        }

        auto const bundle = imports
            .primitive_callbacks[step.configuration_index].bundle;
        auto const& background_plan = connections.background;
        if (bundle < background_plan.bundle_to_background_node.size()
            && background_plan.bundle_to_background_node[bundle]) {
            auto const node =
                *background_plan.bundle_to_background_node[bundle];
            if (node >= background_plan.tick_runtime.nodes.size()) {
                return std::unexpected(
                    "GraphJit realtime batch references a missing Tick invocation");
            }
            auto const& tick_invocation =
                background_plan.tick_runtime.nodes[node];
            if (tick_invocation.sample_capture_count != 0
                || tick_invocation.event_capture_count != 0) {
                return false;
            }
        }
        lanes.push_back(std::addressof(step));
    }

    // Transient placement is planned against the flattened scalar execution
    // positions. Two representations whose scalar lifetimes do not overlap may
    // therefore share bytes even when their primitives are later selected for
    // one native batch. A batch callback can retain and access every lane
    // facade until it returns, so a writable lane range must not overlap any
    // other lane's callback-facing range. Shared read-only inputs remain legal.
    // Front 16 can widen/co-plan these lifetimes when the additional storage is
    // cheaper than scalar fallback.
    enum class CallbackStorageBase : std::uint8_t {
        root_transient,
        node_storage,
    };
    struct CallbackStorageAccess {
        CallbackStorageBase base = CallbackStorageBase::root_transient;
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t lane = 0;
        bool writable = false;
    };
    std::vector<CallbackStorageAccess> callback_storage;
    auto append_range = [&](CallbackStorageBase base,
                            std::size_t begin,
                            std::size_t size,
                            std::size_t lane,
                            bool writable)
        -> std::expected<void, std::string> {
        if (size == 0
            || begin > std::numeric_limits<std::size_t>::max() - size) {
            return std::unexpected(
                "GraphJit realtime batch callback storage range is invalid");
        }
        callback_storage.push_back(CallbackStorageAccess{
            .base = base,
            .begin = begin,
            .end = begin + size,
            .lane = lane,
            .writable = writable,
        });
        return {};
    };
    auto append_sample_representation = [&](std::size_t representation,
                                            std::size_t lane,
                                            bool writable)
        -> std::expected<void, std::string> {
        if (representation >= sample_ports.storage.representations.size()) {
            return std::unexpected(
                "GraphJit realtime batch references missing sample storage");
        }
        auto const& storage =
            sample_ports.storage.representations[representation];
        if (storage.constant_value) {
            if (writable) {
                return std::unexpected(
                    "GraphJit realtime batch output references immutable sample storage");
            }
            return {};
        }
        if (storage.transient_allocation
            != detail::no_sample_transient_allocation) {
            if (storage.transient_allocation
                >= sample_ports.storage.transient_allocations.size()) {
                return std::unexpected(
                    "GraphJit realtime batch references missing transient sample storage");
            }
            auto const& allocation = sample_ports.storage
                .transient_allocations[storage.transient_allocation];
            if (root_stack.sample_offset
                > std::numeric_limits<std::size_t>::max()
                    - allocation.region_relative_offset) {
                return std::unexpected(
                    "GraphJit realtime batch sample storage offset overflows size_t");
            }
            return append_range(
                CallbackStorageBase::root_transient,
                root_stack.sample_offset
                    + allocation.region_relative_offset,
                allocation.size_bytes,
                lane,
                writable);
        }
        if (storage.persistent_allocation
            != detail::no_sample_persistent_allocation) {
            if (storage.persistent_allocation
                >= sample_ports.storage.persistent_allocations.size()) {
                return std::unexpected(
                    "GraphJit realtime batch references missing persistent sample storage");
            }
            auto const& allocation = sample_ports.storage
                .persistent_allocations[storage.persistent_allocation];
            return append_range(
                CallbackStorageBase::node_storage,
                allocation.storage_offset,
                allocation.size_bytes,
                lane,
                writable);
        }
        return std::unexpected(
            "GraphJit realtime batch sample storage has no physical allocation");
    };
    auto append_event_representation = [&](std::size_t representation,
                                           std::size_t lane,
                                           bool writable)
        -> std::expected<void, std::string> {
        if (representation >= event_ports.ports.size()) {
            return std::unexpected(
                "GraphJit realtime batch references missing event storage");
        }
        auto const& storage = event_ports.ports[representation];
        if (storage.immutable_empty) {
            if (writable) {
                return std::unexpected(
                    "GraphJit realtime batch output references immutable event storage");
            }
            return {};
        }
        if (storage.persistent) {
            return append_range(
                CallbackStorageBase::node_storage,
                storage.storage_offset,
                storage.size_bytes,
                lane,
                writable);
        }
        if (storage.transient_allocation
            == detail::no_event_transient_allocation
            || storage.transient_allocation
                >= event_ports.transient_allocations.size()) {
            return std::unexpected(
                "GraphJit realtime batch references missing transient event storage");
        }
        auto const& allocation = event_ports.transient_allocations[
            storage.transient_allocation];
        if (root_stack.event_offset
            > std::numeric_limits<std::size_t>::max()
                - allocation.region_relative_offset) {
            return std::unexpected(
                "GraphJit realtime batch event storage offset overflows size_t");
        }
        return append_range(
            CallbackStorageBase::root_transient,
            root_stack.event_offset + allocation.region_relative_offset,
            allocation.size_bytes,
            lane,
            writable);
    };

    for (std::size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
        auto const primitive = lanes[lane_index]->configuration_index;
        auto const& lane_sample_ports = sample_ports.primitives[primitive];
        for (auto const& input : lane_sample_ports.inputs) {
            if (input.has_contiguous_scratch) return false;
            // The root writes a slot-fed input's timeline for this lane.
            for (auto const& channel : input.channels) {
                auto appended = append_sample_representation(
                    channel.storage, lane_index, input.invocation_supplied);
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
            }
            for (auto const& channel : input.realtime_operand_channels) {
                auto appended = append_sample_representation(
                    channel.storage, lane_index, false);
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
            }
        }
        for (auto const& output : lane_sample_ports.outputs) {
            if (!output.realtime || !output.storage) continue;
            auto appended = append_sample_representation(
                *output.storage, lane_index, true);
            if (!appended) {
                return std::unexpected(std::move(appended.error()));
            }
        }

        auto const& lane_event_ports = event_ports.primitives[primitive];
        for (auto const& input : lane_event_ports.inputs) {
            if (input.storage) {
                // The root writes a slot-fed input's timeline for this lane.
                auto appended = append_event_representation(
                    *input.storage, lane_index, input.invocation_supplied);
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
            }
            if (input.realtime_operand_storage) {
                auto appended = append_event_representation(
                    *input.realtime_operand_storage, lane_index, false);
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
            }
            if (input.combined_storage) {
                auto appended = append_event_representation(
                    *input.combined_storage, lane_index, true);
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
            }
        }
        for (auto const& output : lane_event_ports.outputs) {
            if (!output.realtime || !output.storage) continue;
            auto appended = append_event_representation(
                *output.storage, lane_index, true);
            if (!appended) {
                return std::unexpected(std::move(appended.error()));
            }
        }
    }
    for (std::size_t left = 0; left < callback_storage.size(); ++left) {
        for (std::size_t right = left + 1;
             right < callback_storage.size(); ++right) {
            auto const& a = callback_storage[left];
            auto const& b = callback_storage[right];
            if (a.lane == b.lane || a.base != b.base
                || (!a.writable && !b.writable)) {
                continue;
            }
            if (a.begin < b.end && b.begin < a.end) return false;
        }
    }
    return true;
}

} // namespace

std::expected<LoweringPlan, std::string> build_lowering_plan(
    LoweringInput const& input)
{
    auto analysis = analyze_graph(input);
    if (!analysis) return std::unexpected(std::move(analysis.error()));

    auto const boundary_bundle = input.graph.public_ports.boundary_handle();
    if (boundary_bundle < input.graph.node_bundles.size()) {
        auto const& boundary = input.graph.node_bundles.bundle(boundary_bundle);
        if (boundary.sample_input_count() != 0
            || boundary.sample_output_count() != 0
            || boundary.event_input_count() != 0
            || boundary.event_output_count() != 0) {
            return std::unexpected(
                "GraphJit root graph must not declare boundary ports; system I/O and inter-module communication belong in concrete node types");
        }
    }

    auto port_realization = resolve_port_realization(input.graph);
    if (!port_realization) {
        return std::unexpected(std::move(port_realization.error()));
    }

    // The identifier has no semantic meaning outside this compilation. Equal
    // values mean that the package callback and every callback-facing solved
    // port fact are equal. Configured values, state, and storage addresses are
    // intentionally absent.
    auto same_callback_batch_class = [&](PrimitiveAnalysis const& left,
                                         PrimitiveAnalysis const& right) {
        if ((left.implementation->tick_block != nullptr)
                != (right.implementation->tick_block != nullptr)
            || left.implementation->code_key
                != right.implementation->code_key
            || left.implementation->revision.get()
                != right.implementation->revision.get()
            || left.implementation->provider_version
                != right.implementation->provider_version
            || (left.implementation->tick_block != nullptr
                && left.bundle.maximum_block_size
                    != right.bundle.maximum_block_size)) {
            return false;
        }
        auto const left_bundle = left.bundle.node_bundle;
        auto const right_bundle = right.bundle.node_bundle;
        auto const& left_configured =
            input.graph.node_bundles.bundle(left_bundle);
        auto const& right_configured =
            input.graph.node_bundles.bundle(right_bundle);
        if (left_configured.sample_input_count()
                != right_configured.sample_input_count()
            || left_configured.sample_output_count()
                != right_configured.sample_output_count()
            || left_configured.event_input_count()
                != right_configured.event_input_count()
            || left_configured.event_output_count()
                != right_configured.event_output_count()
            || port_realization->execution_rate(left_bundle)
                != port_realization->execution_rate(right_bundle)) {
            return false;
        }
        for (std::size_t port = 0;
             port < left_configured.sample_input_count(); ++port) {
            if (port_realization->input(left_bundle, port)
                != port_realization->input(right_bundle, port)) return false;
        }
        for (std::size_t port = 0;
             port < left_configured.sample_output_count(); ++port) {
            if (port_realization->output(left_bundle, port)
                != port_realization->output(right_bundle, port)) return false;
        }
        for (std::size_t port = 0;
             port < left_configured.event_input_count(); ++port) {
            if (port_realization->event_input(left_bundle, port)
                != port_realization->event_input(right_bundle, port)) return false;
        }
        for (std::size_t port = 0;
             port < left_configured.event_output_count(); ++port) {
            if (port_realization->event_output(left_bundle, port)
                != port_realization->event_output(right_bundle, port)) return false;
        }
        return true;
    };
    std::vector<std::size_t> callback_batch_classes(
        input.graph.node_bundles.size(),
        std::numeric_limits<std::size_t>::max());
    std::vector<std::size_t> class_representatives;
    for (std::size_t primitive_index = 0;
         primitive_index < analysis->primitives.size(); ++primitive_index) {
        auto const& primitive = analysis->primitives[primitive_index];
        auto selected = class_representatives.size();
        for (std::size_t candidate = 0;
             candidate < class_representatives.size(); ++candidate) {
            if (same_callback_batch_class(
                    primitive,
                    analysis->primitives[class_representatives[candidate]])) {
                selected = candidate;
                break;
            }
        }
        if (selected == class_representatives.size()) {
            class_representatives.push_back(primitive_index);
        }
        callback_batch_classes[primitive.bundle.node_bundle] = selected;
    }

    for (auto const& primitive : analysis->primitives) {
        auto const& implementation = *primitive.implementation;
        if (!implementation.pointwise_tick) continue;
        auto const bundle = primitive.bundle.node_bundle;
        auto const& configured = input.graph.node_bundles.bundle(bundle);
        std::optional<std::int32_t> pointwise_pace;
        auto accept_pace = [&](PortPace pace) {
            if (!pointwise_pace) {
                pointwise_pace = pace.exponent();
                return true;
            }
            return *pointwise_pace == pace.exponent();
        };
        for (std::size_t port = 0;
             port < configured.sample_input_count(); ++port) {
            auto const config = input.graph.node_bundles.resolve_sample_input(
                {bundle, PortKind::sample, port}).config;
            if (is_sequential(config)
                && !accept_pace(port_realization->input(bundle, port).absolute_pace)) {
                return std::unexpected(
                    "pointwise tick() node resolves incompatible continuous-port paces");
            }
        }
        for (std::size_t port = 0;
             port < configured.sample_output_count(); ++port) {
            auto const config = input.graph.node_bundles.resolve_sample_output(
                {bundle, PortKind::sample, port}).config;
            if (is_tick(config)
                && !accept_pace(port_realization->output(bundle, port).absolute_pace)) {
                return std::unexpected(
                    "pointwise tick() node resolves incompatible continuous-port paces");
            }
        }
        if (pointwise_pace && *pointwise_pace != 0
            && (configured.event_input_count() != 0
                || configured.event_output_count() != 0)) {
            return std::unexpected(
                "pointwise tick() with event ports requires project-rate continuous pace because event time is integer-valued");
        }
    }

    std::vector<bool> intrinsically_replayable(
        input.graph.node_bundles.size(), false);
    std::size_t reflected_bundle = 0;
    input.graph.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            intrinsically_replayable.at(reflected_bundle++) =
                view.intrinsically_replayable;
        });

    // Persisted Tick capture still publishes the canonical project-rate
    // scalar-Sample store owned by the recording front. Keep that unfinished
    // consumer boundary explicit without disabling the type-owned realtime
    // storage path for ephemeral FractionalGlobalIndex and FFTBlock ports.
    for (NodeBundleHandle bundle = 0;
         bundle < input.graph.node_bundles.size(); ++bundle) {
        auto const& configured = input.graph.node_bundles.bundle(bundle);
        for (std::size_t port = 0;
             port < configured.sample_output_count(); ++port) {
            auto const selected = port_realization->output(bundle, port).value_type;
            auto const output = input.graph.node_bundles.resolve_sample_output(
                {bundle, PortKind::sample, port}).config;
            if (is_tick(output) && is_persisted(output.retention)
                && selected != continuous_value_type_id_v<Sample>) {
                return std::unexpected(
                    "GraphJit persisted Tick capture does not yet support continuous type '"
                    + std::string(continuous_value_type_info(selected).name)
                    + "'");
            }
            if (is_tick(output) && is_persisted(output.retention)
                && port_realization->output(bundle, port).absolute_pace
                    != PortPace{}) {
                return std::unexpected(
                    "GraphJit persisted Tick capture does not yet support a non-project-rate continuous output");
            }
        }
        if (intrinsically_replayable[bundle]) {
            for (std::size_t port = 0;
                 port < configured.sample_input_count(); ++port) {
                if (port_realization->input(bundle, port).history != 0)
                    return std::unexpected(
                        "intrinsically replayable node resolves nonzero Sequential input history");
            }
            for (std::size_t port = 0;
                 port < configured.sample_output_count(); ++port) {
                auto const& output = port_realization->output(bundle, port);
                if (output.history != 0 || output.latency != 0)
                    return std::unexpected(
                        "intrinsically replayable node resolves nonzero Tick output history or latency");
            }
            for (std::size_t port = 0;
                 port < configured.event_input_count(); ++port) {
                if (port_realization->event_input(bundle, port).history != 0)
                    return std::unexpected(
                        "intrinsically replayable node resolves nonzero Sequential event-input history");
            }
            for (std::size_t port = 0;
                 port < configured.event_output_count(); ++port) {
                auto const& output = port_realization->event_output(bundle, port);
                if (output.history != 0 || output.latency != 0)
                    return std::unexpected(
                        "intrinsically replayable node resolves nonzero Tick event-output history or latency");
            }
        }
    }

    struct RealtimePortPlans {
        ConnectionAnalysisPlan connections{};
        SamplePortBindingPlan sample_ports{};
        EventPortBindingPlan event_ports{};
        RootStackBufferPlan root_stack{};
    };

    std::optional<BackgroundEvaluationPlan> retained_background_plan;

    auto configure_background_replay = [&](ConnectionAnalysisPlan& connections)
        -> std::expected<void, std::string> {
        auto& background = connections.background;
        for (auto& node : background.nodes) {
            auto const primitive = std::ranges::find_if(
                analysis->primitives,
                [&](PrimitiveAnalysis const& candidate) {
                    return candidate.bundle.node_bundle == node.bundle;
                });
            if (primitive == analysis->primitives.end()
                || primitive->implementation == nullptr) {
                return std::unexpected(
                    "GraphJit background node has no primitive implementation");
            }
            node.implementation_identity =
                primitive->implementation->identity;
            node.provider_version =
                primitive->implementation->provider_version;
            node.configuration_identity =
                primitive->implementation->configuration_identity;
        }
        for (auto& replay : background.runtime.replay_invocations) {
            if (replay.node >= background.nodes.size()) {
                return std::unexpected(
                    "GraphJit background replay invocation references an invalid node");
            }
            auto const bundle = background.nodes[replay.node].bundle;
            auto const primitive = std::ranges::find_if(
                analysis->primitives,
                [&](PrimitiveAnalysis const& candidate) {
                    return candidate.bundle.node_bundle == bundle;
                });
            if (primitive == analysis->primitives.end()
                || primitive->implementation == nullptr) {
                return std::unexpected(
                    "GraphJit background replay invocation has no primitive implementation");
            }

            // Replay invokes the imported Tick wrapper, never skip_block().
            // A non-pointwise implementation therefore has authored block
            // input access and needs one bounded contiguous workspace.
            replay.block_input_access =
                !primitive->implementation->pointwise_tick;
        }
        return {};
    };

    // The first analysis derives background topology from every callback
    // batch class. Later analyses reuse that retained background plan, so
    // narrowing these realtime classes changes only the realtime schedule.
    std::vector<std::size_t> realtime_batch_classes = callback_batch_classes;
    auto const no_batch_class = std::numeric_limits<std::size_t>::max();

    auto plan_realtime_ports = [&](RealtimeStorageCostModel const& cost_model)
        -> std::expected<RealtimePortPlans, std::string> {
        auto connections = build_connection_analysis_plan(
            input.graph,
            *port_realization,
            input.specialization.block_size,
            cost_model,
            retained_background_plan ? &*retained_background_plan : nullptr,
            realtime_batch_classes);
        if (!connections) {
            return std::unexpected(std::move(connections.error()));
        }
        if (auto replay = configure_background_replay(*connections); !replay) {
            return std::unexpected(std::move(replay.error()));
        }
        if (!retained_background_plan) {
            retained_background_plan = connections->background;
        }
        auto sample_ports = plan_sample_ports(
            input, *analysis, *connections, *port_realization, cost_model);
        if (!sample_ports) {
            return std::unexpected(std::move(sample_ports.error()));
        }
        auto event_ports = plan_event_ports(
            input, *analysis, *connections, *port_realization, cost_model);
        if (!event_ports) {
            return std::unexpected(std::move(event_ports.error()));
        }
        auto root_stack = plan_root_stack_buffers(*sample_ports, *event_ports);
        if (!root_stack) {
            return std::unexpected(std::move(root_stack.error()));
        }
        return RealtimePortPlans{
            .connections = std::move(*connections),
            .sample_ports = std::move(*sample_ports),
            .event_ports = std::move(*event_ports),
            .root_stack = *root_stack,
        };
    };

    auto const& configured_cost_model = input.realtime_storage_cost_model;
    auto const stack_budget = configured_cost_model.stack_budget_bytes;
    for (;;) {
        auto realtime_ports = plan_realtime_ports(configured_cost_model);
        if (!realtime_ports) {
            return std::unexpected(std::move(realtime_ports.error()));
        }

        auto const has_realtime_batch_class = std::ranges::any_of(
            realtime_batch_classes,
            [&](std::size_t value) { return value != no_batch_class; });
        if (realtime_ports->root_stack.size_bytes > stack_budget
            && has_realtime_batch_class) {
            // Ready-group ordering can extend producer lifetimes. Batching must
            // not be the reason buffers move to NodeStorage, so an over-budget
            // schedule first falls back to the scalar order before stack pressure
            // is applied. Costing batching against such moves is Front 16 work.
            std::ranges::fill(realtime_batch_classes, no_batch_class);
            realtime_ports = plan_realtime_ports(configured_cost_model);
            if (!realtime_ports) {
                return std::unexpected(std::move(realtime_ports.error()));
            }
        }

        if (realtime_ports->root_stack.size_bytes > stack_budget) {
            // Local storage choices are costed before stack buffers are packed. If
            // their packed high-water mark exceeds the project limit, increase the
            // cost of stack bytes and re-plan. This moves ordinary producer,
            // history/latency, feedback, and disconnected-output buffers to
            // NodeStorage through their existing full_node_storage alternative;
            // conversion/merge buffers that have no persistent implementation stay
            // on the stack.
            auto pressure_model = configured_cost_model;
            auto pressure = pressure_model.stack_footprint_byte_weight;
            auto last_insufficient_pressure = pressure;
            std::optional<std::size_t> first_fitting_pressure;
            std::optional<RealtimePortPlans> first_fitting_plan;
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                if (pressure == 0) {
                    pressure = 1;
                } else if (pressure
                           <= std::numeric_limits<std::size_t>::max() / 4) {
                    pressure *= 4;
                } else {
                    break;
                }
                pressure_model.stack_footprint_byte_weight = pressure;
                auto candidate = plan_realtime_ports(pressure_model);
                if (!candidate) {
                    return std::unexpected(std::move(candidate.error()));
                }
                if (candidate->root_stack.size_bytes <= stack_budget) {
                    first_fitting_pressure = pressure;
                    first_fitting_plan = std::move(*candidate);
                    break;
                }
                last_insufficient_pressure = pressure;
                realtime_ports = std::move(candidate);
            }

            // Find the smallest integer stack-byte weight that satisfies the hard
            // limit. This avoids keeping buffers in NodeStorage merely because the
            // exponential search overshot a storage-choice crossover.
            if (first_fitting_pressure && first_fitting_plan) {
                auto low = last_insufficient_pressure + 1;
                auto high = *first_fitting_pressure;
                auto best = std::move(*first_fitting_plan);
                while (low < high) {
                    auto const midpoint = low + (high - low) / 2;
                    pressure_model.stack_footprint_byte_weight = midpoint;
                    auto candidate = plan_realtime_ports(pressure_model);
                    if (!candidate) {
                        return std::unexpected(std::move(candidate.error()));
                    }
                    if (candidate->root_stack.size_bytes <= stack_budget) {
                        high = midpoint;
                        best = std::move(*candidate);
                    } else {
                        low = midpoint + 1;
                    }
                }
                realtime_ports = std::move(best);
            }

            if (realtime_ports->root_stack.size_bytes > stack_budget) {
                // The hard limit outranks the configured heuristic weights. This
                // final pass asks every chooser for its minimum-stack legal
                // realization. Any bytes left after this pass are mandatory
                // invocation-local conversion/merge/output buffers.
                auto minimum_stack_model = configured_cost_model;
                minimum_stack_model.copied_byte_weight = 0;
                minimum_stack_model.ring_addressed_byte_weight = 0;
                minimum_stack_model.stack_footprint_byte_weight = 1;
                minimum_stack_model.persistent_footprint_byte_weight = 0;
                auto minimum_stack = plan_realtime_ports(minimum_stack_model);
                if (!minimum_stack) {
                    return std::unexpected(std::move(minimum_stack.error()));
                }
                realtime_ports = std::move(minimum_stack);
            }

            if (realtime_ports->root_stack.size_bytes > stack_budget) {
                return std::unexpected(
                    "GraphJit packed realtime stack requires "
                    + std::to_string(realtime_ports->root_stack.size_bytes)
                    + " bytes, exceeding the configured "
                    + std::to_string(stack_budget)
                    + "-byte stack budget after all eligible buffers were moved to NodeStorage");
            }
        }

        auto declarations = plan_declarations(
            input,
            *analysis,
            realtime_ports->connections,
            *port_realization,
            realtime_ports->sample_ports,
            realtime_ports->event_ports);
        if (!declarations) return std::unexpected(std::move(declarations.error()));

        for (auto& node : realtime_ports->connections.background.nodes) {
            auto const primitive = std::ranges::find_if(
                analysis->primitives,
                [&](PrimitiveAnalysis const& candidate) {
                    return candidate.bundle.node_bundle == node.bundle;
                });
            if (primitive == analysis->primitives.end()) {
                return std::unexpected(
                    "background node has no matching TockState layout record");
            }
            if (!node.authored_tock_execution) {
                node.tock_state_node.reset();
                continue;
            }
            auto const primitive_index = static_cast<std::size_t>(
                std::distance(analysis->primitives.begin(), primitive));
            if (primitive_index >= declarations->primitive_layout_nodes.size()) {
                return std::unexpected(
                    "background Tock node lost its top-level layout mapping");
            }
            node.tock_state_node =
                declarations->primitive_layout_nodes[primitive_index];
        }

        auto realtime_port_state_realizations =
            plan_sample_realtime_port_state_realizations(
                *analysis,
                realtime_ports->connections,
                *declarations,
                realtime_ports->sample_ports);
        if (!realtime_port_state_realizations) {
            return std::unexpected(
                std::move(realtime_port_state_realizations.error()));
        }
        auto event_realtime_port_state_realizations =
            plan_event_realtime_port_state_realizations(
                *analysis,
                realtime_ports->connections,
                *declarations,
                realtime_ports->event_ports);
        if (!event_realtime_port_state_realizations) {
            return std::unexpected(
                std::move(event_realtime_port_state_realizations.error()));
        }
        realtime_port_state_realizations->event_states =
            std::move(*event_realtime_port_state_realizations);

        auto imports = plan_package_imports(
            input, *analysis, realtime_ports->connections.background);
        if (!imports) return std::unexpected(std::move(imports.error()));
        for (std::size_t primitive = 0;
             primitive < realtime_ports->sample_ports.primitives.size();
             ++primitive) {
            auto const& sample =
                realtime_ports->sample_ports.primitives[primitive];
            auto const mixes_background = std::ranges::any_of(
                sample.inputs,
                &PrimitiveSampleInputBindingPlan::combines_invocation_supplied);
            if (mixes_background
                && (primitive >= imports->primitive_callbacks.size()
                    || imports->primitive_callbacks[primitive]
                           .combine_sequential_input_values.empty())) {
                return std::unexpected(
                    "GraphJit mixed Tick/background sample input has no Sequential combination callback");
            }
        }

        auto configurations = plan_node_configurations(
            input, *analysis, realtime_ports->connections, *imports);
        if (!configurations) {
            return std::unexpected(std::move(configurations.error()));
        }

        auto execution = plan_execution(
            *analysis,
            realtime_ports->connections,
            *declarations,
            *imports,
            realtime_ports->sample_ports,
            realtime_ports->event_ports);
        if (!execution) return std::unexpected(std::move(execution.error()));

        // A ready group that lowering cannot batch would only reorder the scalar
        // schedule and lengthen transient lifetimes. Remove its members from the
        // realtime batch classes and plan again; each pass removes at least two
        // bundles, so this terminates.
        std::vector<std::size_t> rejected_bundles;
        for (auto const& group : execution->ready_groups) {
            if (group.cyclic_region || group.primitive_steps.size() < 2) continue;
            auto batches = realtime_ready_group_batches(
                realtime_ports->connections,
                *declarations,
                *imports,
                realtime_ports->sample_ports,
                realtime_ports->event_ports,
                realtime_ports->root_stack,
                *execution,
                group);
            if (!batches) return std::unexpected(std::move(batches.error()));
            if (*batches) continue;
            for (auto const step_index : group.primitive_steps) {
                auto const configuration =
                    execution->primitive_steps[step_index].configuration_index;
                rejected_bundles.push_back(
                    imports->primitive_callbacks[configuration].bundle);
            }
        }
        if (!rejected_bundles.empty()) {
            for (auto const bundle : rejected_bundles) {
                if (bundle >= realtime_batch_classes.size()
                    || realtime_batch_classes[bundle] == no_batch_class) {
                    return std::unexpected(
                        "GraphJit rejected a realtime batch outside the batch classes");
                }
                realtime_batch_classes[bundle] = no_batch_class;
            }
            continue;
        }

        return LoweringPlan{
            .port_realization = std::move(*port_realization),
            .connections = std::move(realtime_ports->connections),
            .declarations = std::move(*declarations),
            .imports = std::move(*imports),
            .configurations = std::move(*configurations),
            .sample_ports = std::move(realtime_ports->sample_ports),
            .event_ports = std::move(realtime_ports->event_ports),
            .realtime_port_state_realizations =
                std::move(*realtime_port_state_realizations),
            .root_stack = realtime_ports->root_stack,
            .execution = std::move(*execution),
        };
    }
}
std::expected<void, std::string> split_unbatched_ready_groups(LoweringPlan& plan)
{
    std::vector<ExecutionReadyGroupPlan> groups;
    groups.reserve(plan.execution.ready_groups.size());
    for (auto& group : plan.execution.ready_groups) {
        if (group.cyclic_region || group.primitive_steps.size() < 2) {
            groups.push_back(std::move(group));
            continue;
        }
        auto batches = realtime_ready_group_batches(
            plan.connections,
            plan.declarations,
            plan.imports,
            plan.sample_ports,
            plan.event_ports,
            plan.root_stack,
            plan.execution,
            group);
        if (!batches) return std::unexpected(std::move(batches.error()));
        if (*batches) {
            groups.push_back(std::move(group));
            continue;
        }
        for (auto const step : group.primitive_steps) {
            groups.push_back(ExecutionReadyGroupPlan{.primitive_steps = {step}});
        }
    }
    plan.execution.ready_groups = std::move(groups);
    return {};
}
} // namespace iv::graph_jit::detail
