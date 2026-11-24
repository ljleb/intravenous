#include <intravenous/graph_jit/lowering_plan.h>
#include <intravenous/graph_jit/activity_runtime.h>
#include <intravenous/graph_jit/background_storage_plan.h>
#include <intravenous/graph_jit/lowering/graph_analysis.h>
#include <intravenous/graph_jit/realtime_port_state_transition.h>
#include <intravenous/graph_jit/sample_storage_plan.h>
#include <intravenous/packages/package_pipeline_types.h>

#include <llvm/IR/Function.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
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

void initialize_copy_raw_region(
    std::span<std::byte> storage,
    std::span<std::byte const> data)
{
    IV_ASSERT(storage.size() == data.size(),
        "raw region initializer data has the wrong size");
    std::ranges::copy(data, storage.begin());
}

std::string node_config_global_symbol(std::size_t primitive_index)
{
    return "iv.graph.node_config." + std::to_string(primitive_index);
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
        auto const has_tick = !implementation->tick_block.empty();
        auto const has_tock = !implementation->tock_coverage.empty();
        auto const intrinsic = implementation->resolved_module_input.has_value()
            || implementation->observation_slot.has_value() || implementation->native_background_node;
        if ((!intrinsic && !implementation->package) || has_tick == has_tock
            || !implementation->declare_node || (!intrinsic && !implementation->node_data)) {
            return std::unexpected(
                "resolved primitive implementation is incomplete at the GraphJit lowering boundary");
        }
        if (implementation->replay_mode != TickReplayMode::disabled && !has_tick) {
            return std::unexpected(
                "only Tick primitives may declare intrinsic replayability");
        }
        if (has_tick && (implementation->tick_block_batch.empty()
                || implementation->skip_block.empty()
                || implementation->skip_block_batch.empty())) {
            return std::unexpected(
                "Tick primitive has an incomplete normalized scalar/batch callback family");
        }
        if (has_tock && (implementation->tock_coverage_batch.empty()
                || implementation->propagate_forward_coverage.empty()
                || implementation->propagate_forward_coverage_batch.empty())) {
            return std::unexpected(
                "Tock primitive has an incomplete normalized scalar/batch callback family");
        }
        if ((!has_tick && (!implementation->tick_block_batch.empty()
                || !implementation->skip_block.empty()
                || !implementation->skip_block_batch.empty()))
            || (!has_tock && !implementation->tock_coverage_batch.empty())
            || (!has_tock
                && (!implementation->propagate_forward_coverage.empty()
                    || !implementation->propagate_forward_coverage_batch.empty()
                    || !implementation->propagate_reverse_coverage.empty()
                    || !implementation->propagate_reverse_coverage_batch.empty()))
            || (implementation->propagate_forward_coverage.empty()
                != implementation->propagate_forward_coverage_batch.empty())
            || (implementation->propagate_reverse_coverage.empty()
                != implementation->propagate_reverse_coverage_batch.empty())) {
            return std::unexpected(
                "resolved primitive has an inconsistent package callback family");
        }
        if (primitive.node_size == 0 && !intrinsic) {
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
    auto const& implementation = *primitive.implementation;
    std::ostringstream out;
    out << "graphjit.activity:"
        << migration_identity_fields(*planned.stable_identity)
        << ":type=" << implementation.identity.node_type_id.size() << ':'
        << implementation.identity.node_type_id
        << ":provider=" << implementation.provider_version
        << ":code=" << implementation.code_key.low << '.'
        << implementation.code_key.high
        << ":config=";
    auto const& configuration = implementation.configuration_identity;
    if (auto const* sample = std::get_if<CompilerSampleConfiguration>(&configuration)) {
        out << "sample:" << sample->bits;
    } else {
        out << reinterpret_cast<std::uintptr_t>(
            std::get<std::shared_ptr<void const>>(configuration).get());
    }
    out << ":ttl=" << activity.ttl_samples
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

std::string manual_control_migration_identity(
    ConfiguredGraph const& graph,
    PlannedGraphNode const& planned,
    ConfiguredManualControl const& control)
{
    if (!planned.stable_identity) return {};
    // The owner and authored control identify the binding across selection reordering.
    auto const input_name = graph.node_bundles.resolve_sample_input(control.input).config.name;
    std::ostringstream out;
    out << "graphjit.manual_control:"
        << migration_identity_fields(*planned.stable_identity)
        << ":input=" << control.id.port_name.size() << ':'
        << control.id.port_name
        << migration_port_identity_fields(input_name,
            port_migration_index(graph, control.input, PortDirection::input, input_name))
        << ":source=" << control.id.node.instance_id.size() << ':' << control.id.node.instance_id;
    for (auto const& selector : control.id.node.path) std::visit([&](auto const& entry) {
        using T = std::remove_cvref_t<decltype(entry)>;
        if constexpr (std::same_as<T, ProjectSymbolicNodeSelector>) {
            out << ":symbolic=" << entry.source_identity.size() << ':' << entry.source_identity;
            if (entry.type_identity) out << ":type=" << entry.type_identity->size() << ':' << *entry.type_identity;
        } else if constexpr (std::same_as<T, ProjectSymbolicMemberSelector>) out << ":member=" << entry.index;
        else out << ":child=" << entry.name.size() << ':' << entry.name;
    }, selector);
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
        auto const first_declared_node = layout_builder.node_count();
        auto const node_index = implementation.declare_node(
            implementation.node_data,
            implementation.state_structures,
            layout_builder,
            reflected_ports);
        declared_node_indices.push_back(node_index);

        // Authored identity can span configurations. A configured bundle
        // coordinate, including its owned declarations, is snapshot-local.
        auto const planned = std::ranges::find(
            connections.nodes, bundle, &PlannedGraphNode::bundle);
        if (planned == connections.nodes.end()) {
            return std::unexpected(
                "GraphJit declaration lost its planned concrete node");
        }
        if (planned->stable_identity) {
            auto const identity = "graphjit.node:" + migration_identity_fields(*planned->stable_identity);
            layout_builder.set_node_migration_identity(
                node_index, identity);
            if (input.configured_bundles.snapshot) {
                for (auto child = first_declared_node; child < layout_builder.node_count(); ++child) {
                    if (child == node_index) continue;
                    layout_builder.set_node_migration_identity(child, identity
                        + ":snapshot=" + std::to_string(input.configured_bundles.snapshot)
                        + ":declaration=" + std::to_string(child - first_declared_node));
                }
            }
        }
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

    std::vector<LiveTickCoverageHistoryPlan> live_histories;
    auto const& background = connections.background;
    auto& sample_storage = sample_ports.storage;
    std::vector<std::size_t> retained_coverage(background.accumulators.output_change_count);
    auto retain = [&](SequentialCoverageSource const& source, std::size_t history)
        -> std::expected<void, std::string> {
        if (!source.output_change) return {};
        if (source.read_latency > std::numeric_limits<std::size_t>::max() - history)
            return std::unexpected("GraphJit live coverage retention overflows");
        auto& extent = retained_coverage[*source.output_change];
        extent = std::max(extent, source.read_latency + history);
        return {};
    };
    for (auto const& primitive : sample_ports.primitives) {
        for (auto const& port : primitive.inputs) {
            for (auto const& projection : port.coverage_projections) {
                for (auto const& source : projection.sources) {
                    if (auto retained = retain(source, port.history); !retained)
                        return std::unexpected(std::move(retained.error()));
                }
            }
        }
    }
    auto retain_composition = [&](auto const& contributions, std::size_t history)
        -> std::expected<void, std::string> {
        for (auto const& contribution : contributions) {
            for (auto const& source : contribution.sources) {
                for (auto const& coverage : source.coverage_sources) {
                    if (auto retained = retain(coverage, history); !retained) return retained;
                }
            }
            for (auto const& coverage : contribution.coverage_sources) {
                if (auto retained = retain(coverage, history); !retained) return retained;
            }
        }
        return {};
    };
    for (auto const& composition : sample_storage.compositions) {
        if (auto retained = retain_composition(composition.contributions, composition.target_history); !retained)
            return std::unexpected(std::move(retained.error()));
    }
    for (auto const& timeline : sample_storage.feedback_timelines) {
        if (auto retained = retain_composition(
                timeline.writer.composition_contributions, timeline.writer.revision_frames); !retained)
            return std::unexpected(std::move(retained.error()));
    }
    for (auto const& connection : background.connections) {
        for (auto const& delivery : connection.event_deliveries) {
            if (!delivery.source_port || !uses_realtime_storage(delivery.mechanism)) continue;
            auto const& source = background.ports[*delivery.source_port];
            if (auto retained = retain({source.accumulators.output_change, delivery.live_read_latency}, 0);
                !retained) return std::unexpected(std::move(retained.error()));
        }
    }
    auto ensure_pace = [&](std::int32_t pace) {
        if (!std::ranges::contains(sample_storage.sequential_pace_cursors,
                pace, &SequentialPaceCursorPlan::absolute_pace_exponent))
            sample_storage.sequential_pace_cursors.push_back({.absolute_pace_exponent = pace});
    };
    for (auto const& port : background.ports) {
        if (port.direction != PortDirection::output || !port.accumulators.output_change
            || !background.nodes[port.node].tracks_live_tick_coverage) continue;
        auto const primitive = std::ranges::find_if(analysis.primitives, [&](auto const& candidate) {
            return candidate.bundle.node_bundle == port.configured_port.node_bundle_handle;
        });
        if (primitive == analysis.primitives.end())
            return std::unexpected("GraphJit live coverage output has no primitive");
        auto const primitive_index = static_cast<std::size_t>(primitive - analysis.primitives.begin());
        auto const output = port.configured_port.port_index;
        auto const change = *port.accumulators.output_change;
        auto const pace = port.kind == PortKind::sample
            ? sample_ports.primitives[primitive_index].outputs[output].absolute_pace_exponent : 0;
        auto const maximum_window = FractionalGlobalIndexRange::maximum_projection_count(
            input.specialization.block_size, pace);
        auto const retained = retained_coverage[change];
        constexpr auto max_capacity = std::bit_floor(std::numeric_limits<std::size_t>::max());
        if (!maximum_window || *maximum_window > max_capacity
            || retained > max_capacity - *maximum_window)
            return std::unexpected("GraphJit live coverage history capacity overflows");
        auto const capacity = std::bit_ceil(std::max(std::size_t{1}, retained + *maximum_window));
        std::string value_identity;
        if (port.kind == PortKind::sample) {
            auto const storage = sample_ports.primitives[primitive_index].outputs[output].storage;
            if (storage) {
                auto const& representation = sample_storage.representations[*storage];
                auto const carry = std::ranges::find(sample_storage.carry_operations,
                    *storage, &SampleCarryOperationPlan::representation_index);
                auto const allocation = carry != sample_storage.carry_operations.end()
                    ? carry->persistent_allocation : representation.persistent_allocation;
                if (allocation != no_sample_persistent_allocation)
                    value_identity = sample_storage.persistent_allocations[allocation].migration_identity;
            }
        } else {
            auto const storage = event_ports.primitives[primitive_index].outputs[output].storage;
            if (storage) {
                auto const carry = std::ranges::find(event_ports.carry_operations,
                    *storage, &EventCarryPlan::working_representation);
                auto const retained_storage = carry != event_ports.carry_operations.end()
                    ? carry->persistent_representation : *storage;
                value_identity = event_ports.ports[retained_storage].migration_identity;
            }
        }
        auto const identity = value_identity.empty() ? std::string{} : "graphjit.live_coverage:" + value_identity;
        live_histories.push_back({
            .output_change = change, .primitive = primitive_index,
            .pace = pace, .capacity = capacity,
            .region = layout_builder.declare_raw_region(capacity, 1, identity, initialize_zero_raw_region),
        });
        ensure_pace(pace);
        for (auto const& binding : sample_ports.primitives[primitive_index].inputs)
            if (!binding.coverage_projections.empty()) ensure_pace(binding.absolute_pace_exponent);
        if (!event_ports.primitives[primitive_index].inputs.empty()) ensure_pace(0);
    }
    std::ranges::sort(sample_storage.sequential_pace_cursors, {},
        &SequentialPaceCursorPlan::absolute_pace_exponent);

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
                    != no_event_transient_allocation) {
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

    for (auto& control : sample_ports.manual_controls) {
        auto const planned = std::ranges::find(
            connections.nodes,
            analysis.primitives[control.primitive_index].bundle.node_bundle,
            &PlannedGraphNode::bundle);
        if (planned == connections.nodes.end()) {
            return std::unexpected(
                "GraphJit manual control lost its planned concrete node");
        }
        auto const bits = std::bit_cast<std::uint32_t>(
            control.configured.settled_value.value);
        ManualControlRuntimeState initial{
            .target_bits = bits,
            .effective_bits = bits,
        };
        auto const channels = control.channel_coefficients.size();
        if (control.delay != 0) {
            auto const pace = sample_ports.primitives[control.primitive_index].inputs[control.input_index].absolute_pace_exponent;
            auto values = input.specialization.block_size;
            if (pace >= 0) {
                if (static_cast<unsigned>(pace) >= sizeof(std::size_t) * 8 || values > (std::numeric_limits<std::size_t>::max() >> pace))
                    return std::unexpected("GraphJit manual source pace overflows its delay capacity");
                values <<= pace;
            } else {
                auto const shift = static_cast<unsigned>(-pace);
                values = shift >= sizeof(std::size_t) * 8 ? 1 : (values >> shift) + 1;
            }
            if (control.delay > std::numeric_limits<std::size_t>::max() - values
                || control.delay + values > (std::size_t{1} << (sizeof(std::size_t) * 8 - 1)))
                return std::unexpected("GraphJit manual source delay capacity overflows");
            control.delay_capacity = std::bit_ceil(control.delay + values);
        }
        if (channels != 0 && control.delay_capacity > (std::numeric_limits<std::size_t>::max() - sizeof(initial)) / sizeof(Sample) / channels)
            return std::unexpected("GraphJit manual source delay storage overflows");
        auto const size = sizeof(initial) + control.delay_capacity * channels * sizeof(Sample);
        std::vector<std::byte> initial_bytes(size);
        std::memcpy(initial_bytes.data(), &initial, sizeof(initial));
        for (std::size_t channel = 0; channel < channels; ++channel)
            for (std::size_t index = 0; index < control.delay_capacity; ++index)
                std::memcpy(initial_bytes.data() + sizeof(initial) + (channel * control.delay_capacity + index) * sizeof(Sample),
                    &control.delay_initial_values[channel], sizeof(Sample));
        auto identity = manual_control_migration_identity(input.graph, *planned, control.configured);
        if (control.boundary_source && !identity.empty()) {
            identity += ":boundary_delay=" + std::to_string(control.delay);
            for (auto coefficient : control.channel_coefficients) identity += ':' + std::to_string(std::bit_cast<std::uint32_t>(coefficient));
        }
        control.state_region = layout_builder.declare_raw_region(
            size,
            alignof(ManualControlRuntimeState),
            std::move(identity),
            initialize_copy_raw_region,
            std::move(initial_bytes));
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
        // A callback-elided producer never ticks, so it has no dormancy to
        // track. Its readers see an immutable input instead.
        if (sample_plan.tick_callback_elided) continue;
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
            // Constant and dynamic manual-control specializations must
            // declare one canonical activity state. Treat the selected input
            // as runtime-observed in both variants even when constant code
            // binds an immutable value.
            auto static_constant = !externally_bound
                && !input_plan.manual_control.has_value();
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
                || owns(sample_ports.storage.sequential_pace_cursors)
                || owns(live_histories);
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
        auto const manual_control_owns_region = std::ranges::any_of(
            sample_ports.manual_controls,
            [&](ManualControlPlan const& control) {
                return control.state_region.valid()
                    && control.state_region.index == region_index;
            });
        if (region.kind == NodeLayout::Region::Kind::raw
            && (sample_storage_owns_region(region_index)
                || event_storage_owns_region
                || activity_owns_region
                || manual_control_owns_region)) {
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

    for (auto& control : sample_ports.manual_controls) {
        if (!control.state_region.valid()
            || control.state_region.index >= node_layout.regions.size()) {
            return std::unexpected(
                "GraphJit manual control lost its persistent raw region");
        }
        auto const& region = node_layout.regions[control.state_region.index];
        if (region.kind != NodeLayout::Region::Kind::raw
            || region.size != sizeof(ManualControlRuntimeState) + control.delay_capacity * control.channel_coefficients.size() * sizeof(Sample)
            || region.alignment != alignof(ManualControlRuntimeState)
            || region.raw_initialize_fn != initialize_copy_raw_region
            || region.raw_initialize_data.size() != region.size) {
            return std::unexpected(
                "GraphJit manual control state disagrees with finalized NodeLayout");
        }
        control.state_storage_offset = region.storage_offset;
        control.delay_storage_offset = region.storage_offset + sizeof(ManualControlRuntimeState);
    }


    DeclarationPlan plan{
        .node_layout = std::move(node_layout),
        .tock_state_layout = std::move(tock_state_layout),
        .primitive_layout_nodes = std::move(declared_node_indices),
        .primitive_activity = std::move(primitive_activity),
        .activity_frontiers = std::move(activity_frontiers),
        .live_coverage_histories = std::move(live_histories),
    };
    for (auto& history : plan.live_coverage_histories)
        history.storage_offset = plan.node_layout.regions[history.region.index].storage_offset;
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
                "declared primitive State layout disagrees with finalized compiler metadata");
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
    ConfiguredGraph const& graph,
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
        // A producer's canonical representation holds that Tick output's own
        // timeline; name the output so a cutover can tell whether two views
        // hold the same producer's values.
        if (representation.canonical_producer_representation
            && representation.producer_group_index
                < connections.sample_producer_groups.size()) {
            auto const& source_port = connections.sample_producer_groups[
                representation.producer_group_index].source_port;
            auto const planned = source_port
                ? std::ranges::find(
                    connections.nodes,
                    source_port->node_bundle_handle,
                    &PlannedGraphNode::bundle)
                : connections.nodes.end();
            if (planned != connections.nodes.end() && planned->resolved_module_input) {
                auto const& origin = *planned->resolved_module_input;
                view.producer = StableOutputPortId{origin.node, origin.kind, origin.port_name, origin.port_index};
                view.producer_channel = representation_channel;
                view.producer_latency = 0;
            } else if (planned != connections.nodes.end() && planned->stable_identity) {
                auto const& bundle =
                    graph.node_bundles.bundle(source_port->node_bundle_handle);
                if (bundle.is_concrete()) {
                    auto const outputs = bundle.concrete_ports().sample_outputs();
                    if (source_port->port_index < outputs.size()) {
                        view.producer = StableOutputPortId{
                            .node = *planned->stable_identity,
                            .kind = PortKind::sample,
                            .port_name = outputs[source_port->port_index].name,
                            .port_index = port_migration_index(graph, *source_port, PortDirection::output,
                                outputs[source_port->port_index].name),
                        };
                        view.producer_channel = representation_channel;
                        for (auto const& connection : connections.sample_connections) {
                            auto const timing = std::ranges::find_if(
                                connection.source_channel_timings,
                                [&](SampleSourceChannelTimingPlan const& candidate) {
                                    return candidate.source.bundle
                                            == source_port->node_bundle_handle
                                        && candidate.source.port
                                            == source_port->port_index;
                                });
                            if (timing != connection.source_channel_timings.end()) {
                                view.producer_latency = timing->source_latency;
                                break;
                            }
                        }
                    }
                }
            }
        }
        // A feedback timeline written by a copy or a composition is read only
        // by its own connection's input. A producer_home timeline is the
        // producer's canonical representation instead.
        view.feedback_timeline = std::ranges::any_of(
            sample_ports.storage.feedback_timelines,
            [&](SampleFeedbackTimelinePlan const& timeline) {
                return timeline.timeline_representation == representation_index
                    && timeline.writer.kind
                        != SampleFeedbackTimelineWriterKind::producer_home;
            });

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
            view.initial_value = allocation.initialize_value;
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
        // A Sequential input's record may own no history: its window is
        // [-history, in_flight), possibly empty.
        if (!requirement.channel
            || (requirement.extent_samples == 0
                && requirement.role
                    != RealtimePortStateRole::sequential_input_history)) {
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
            // A sample output owns one window: its last history + latency
            // values, filed at the indices they were authored for.
            if (requirement.role != RealtimePortStateRole::tick_output_history
                || binding.latency
                    > std::numeric_limits<std::size_t>::max() - binding.history
                || binding.history + binding.latency == 0
                || binding.history + binding.latency
                    != requirement.extent_samples) {
                return std::unexpected(
                    "GraphJit sample output state disagrees with its semantic extent");
            }
            representation_index = *binding.storage;
            representation_channel = *requirement.channel;
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
        if (requirement.direction == PortDirection::input) {
            auto neutral = initialized_sequential_input_value(
                analysis.primitives[*primitive].implementation,
                port,
                primitive_ports.inputs[port].geometry,
                iv::details::SequentialInputInitialization::neutral);
            if (!neutral) return std::unexpected(std::move(neutral.error()));
            realization.neutral_value = std::move(*neutral);
        }

        if (realization.authored_storage.storage
            == SampleRealtimePortStateStorage::immutable_constant) {
            realization.retained_access =
                SampleRealtimePortStateAccess::immutable_constant;
            result.sample_states.push_back(std::move(realization));
            continue;
        }

        auto retained = storage_view(
            representation_index,
            representation_channel,
            timeline_offset,
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
                    bool all_sources_retained = true;
                    for (auto const& contribution : (**composition).contributions) {
                        for (std::size_t converted_channel = 0;
                             converted_channel
                                < contribution.target_channels.size();
                             ++converted_channel) {
                            if (contribution.target_channels[converted_channel]
                                != representation_channel) {
                                continue;
                            }
                            // An explicit transform names its own source
                            // count and coefficients, as in the composition
                            // kernel; otherwise the registry conversion
                            // between the two layouts does.
                            auto const& transform = contribution.transform;
                            auto const source_count = transform.empty()
                                ? channel_count(contribution.source_layout)
                                : transform.source_channel_count;
                            if (contribution.sources.size() != source_count
                                || (!transform.empty()
                                    && (converted_channel
                                            >= transform.target_channel_count
                                        || transform.coefficients.size()
                                            != source_count
                                                * transform.target_channel_count))) {
                                return std::unexpected(
                                    "GraphJit composed port state has an invalid target projection");
                            }
                            selected_target = true;
                            for (std::size_t source_channel = 0;
                                 source_channel
                                    < contribution.sources.size();
                                 ++source_channel) {
                                auto const coefficient = transform.empty()
                                    ? channel_conversion_coefficient(
                                        contribution.source_layout.channel_type,
                                        contribution.converted_layout.channel_type,
                                        source_channel,
                                        converted_channel)
                                    : transform.coefficients[
                                        converted_channel * source_count
                                        + source_channel];
                                if (coefficient == 0.0f) continue;
                                auto const& source =
                                    contribution.sources[source_channel];
                                bool const owns_samples = requirement.extent_samples != 0
                                    || source.read_latency > source.source_latency;
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
                                if (source_view && !*source_view && !owns_samples) {
                                    source_view = storage_view(source.source_representation,
                                        source.source_channel,
                                        -static_cast<std::int64_t>(source.read_latency), false);
                                }
                                if (!source_view) {
                                    return std::unexpected(
                                        std::move(source_view.error()));
                                }
                                if (!*source_view) {
                                    all_sources_retained = false;
                                    break;
                                }
                                (**source_view).producer_latency = source.source_latency;
                                realization.composition_sources.push_back({
                                    .storage = std::move(**source_view),
                                    .coefficient = coefficient,
                                    .owns_samples = owns_samples,
                                });
                            }
                            if (!all_sources_retained) break;
                        }
                        if (!all_sources_retained) break;
                    }
                    if (!all_sources_retained) realization.composition_sources.clear();
                    if (selected_target && all_sources_retained
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
    ConfiguredGraph const& graph,
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

    // Every realtime delivery to one Sequential event input, with its source.
    auto for_each_delivery = [&](NodeBundlePortId input, auto&& visit) {
        for (auto const& connection : connections.event_connections) {
            for (auto const& delivery : connection.deliveries) {
                if (!uses_realtime_storage(delivery.mechanism)
                    || delivery.target_index >= connection.target_plans.size()
                    || delivery.source_index >= connection.source_plans.size()) {
                    continue;
                }
                auto const target =
                    connection.target_plans[delivery.target_index].target;
                if (target.bundle != input.node_bundle_handle
                    || target.port != input.port_index) {
                    continue;
                }
                auto const& source =
                    connection.source_plans[delivery.source_index];
                auto const& detach = source.detach ? source.detach : connection.detach;
                visit(source, delivery, detach);
            }
        }
    };
    auto input_source_timings = [&](NodeBundlePortId input) {
        std::vector<EventRealtimePortStateSourceTiming> timings;
        for_each_delivery(input, [&](
            EventSourcePlan const& source,
            EventDeliveryPlan const& delivery,
            std::optional<ConfiguredEventConnectionDetach> const& detach) {
            EventRealtimePortStateSourceTiming timing{
                .read_delay = iv::detail::saturating_add(
                    delivery.read_latency,
                    detach ? detach->loop_extra_latency : 0),
                .producer_latency = source.latency,
                .feedback_timeline = detach.has_value(),
                .source_index = delivery.source_index,
            };
            auto const planned = std::ranges::find(
                connections.nodes, source.source.bundle, &PlannedGraphNode::bundle);
            if (planned != connections.nodes.end() && planned->resolved_module_input) {
                auto const& origin = *planned->resolved_module_input;
                timing.producer = StableOutputPortId{origin.node, origin.kind, origin.port_name, origin.port_index};
            } else if (planned != connections.nodes.end() && planned->stable_identity) {
                auto const& bundle = graph.node_bundles.bundle(source.source.bundle);
                if (bundle.is_concrete()) {
                    auto const outputs = bundle.concrete_ports().event_outputs();
                    if (source.source.port < outputs.size()) {
                        timing.producer = StableOutputPortId{
                            .node = *planned->stable_identity,
                            .kind = PortKind::event,
                            .port_name = outputs[source.source.port].name,
                            .port_index = port_migration_index(graph,
                                {source.source.bundle, PortKind::event, source.source.port},
                                PortDirection::output, outputs[source.source.port].name),
                        };
                    }
                }
            }
            timings.push_back(std::move(timing));
        });
        return timings;
    };
    for (std::size_t requirement_index = 0;
         requirement_index
            < connections.realtime_port_states.states.size();
         ++requirement_index) {
        auto const& requirement =
            connections.realtime_port_states.states[requirement_index];
        if (requirement.configured_port.port_kind != PortKind::event) continue;
        // A Sequential input's record may own no history and no in-flight
        // events, as a sample input's may (RealtimePortStateRequirement).
        if (requirement.channel
            || (requirement.direction == PortDirection::output
                && requirement.extent_samples == 0)) {
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
            auto const expected_extent = requirement.role
                    == RealtimePortStateRole::tick_output_history
                ? iv::detail::saturating_add(binding.history, binding.latency)
                : std::size_t{0};
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
            realization.source_timings = input_source_timings(
                requirement.configured_port);
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

                // A group stream and a detach() delayed stream both hold
                // each event at the position the input reads it.
                realization.retained_storage->timeline_offset = 0;

                // A producer group's own stream also holds its sources'
                // latency positions, which belong to those outputs.
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
                    for (std::size_t source_index = 0;
                         source_index < group.sources.size();
                         ++source_index) {
                        auto const source = group.sources[source_index];
                        auto const output_requirement = std::ranges::find_if(
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
                                            tick_output_history;
                            });
                        auto const source_primitive =
                            primitive_index_for_bundle(source.bundle);
                        if (output_requirement
                                == connections.realtime_port_states.states.end()
                            || !source_primitive
                            || *source_primitive >= event_ports.primitives.size()
                            || source.port >= event_ports.primitives[
                                *source_primitive].outputs.size()) {
                            continue;
                        }
                        auto const latency = event_ports.primitives[
                            *source_primitive].outputs[source.port].latency;
                        if (latency == 0) continue;
                        realization.retained_output_sources.push_back(
                            EventRealtimePortStateOutputSource{
                                .requirement_index =
                                    static_cast<std::size_t>(std::distance(
                                        connections.realtime_port_states.states
                                            .begin(),
                                        output_requirement)),
                                .conversion =
                                    group.source_conversions[source_index],
                                .latency = latency,
                                .stream_delay = iv::detail::saturating_add(
                                    latency,
                                    source_index
                                            < group.source_compensations.size()
                                        ? group.source_compensations[
                                            source_index]
                                        : 0),
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

        // The output's retained stream holds each event where it becomes
        // final, its latency after its position.
        auto const& binding = primitive_ports.outputs[port];
        auto const output_offset =
            static_cast<std::int64_t>(binding.latency);
        if (binding.canonical_storage) {
            realization.authored_storage.timeline_offset = output_offset;
        }
        auto retained = retained_representation(authored_representation);
        if (!retained) return std::unexpected(std::move(retained.error()));
        if (*retained) {
            auto view = storage_view(**retained);
            if (!view) return std::unexpected(std::move(view.error()));
            realization.retained_storage = std::move(*view);
            realization.retained_storage->timeline_offset = output_offset;
            realization.retained_access = EventRealtimePortStateAccess::direct;
        }
        result.push_back(std::move(realization));
    }
    return result;
}

// Every Sequential input of a graph that starts without a predecessor reads its
// fresh value at each position of its window. Persistent sample regions on
// whose fresh value every state agrees start at that value
// (plan_fresh_sample_storage_values()). Each view of such a region then
// declares it as its initial_value, as a feedback timeline's view does.
std::expected<void, std::string> declare_fresh_sample_storage_values(
    NodeLayout& layout,
    SampleStoragePlan& storage,
    RealtimePortStateRequirements const& requirements,
    RealtimePortStateRealizations& realizations)
{
    auto fresh = plan_fresh_sample_storage_values(requirements, realizations);
    if (!fresh) return std::unexpected(std::move(fresh.error()));
    for (auto const& region : *fresh) {
        auto declared = declare_sample_fresh_storage_value(
            layout, storage, region.node_storage_offset, region.value);
        if (!declared) return declared;
    }
    auto declare_view = [&](SampleRealtimePortStateStorageView& view) {
        if (!view.node_storage_offset || view.initial_value) return;
        auto const region = std::ranges::find(
            *fresh,
            *view.node_storage_offset,
            &FreshSampleStorageValue::node_storage_offset);
        if (region != fresh->end()) view.initial_value = region->value;
    };
    for (auto& realization : realizations.sample_states) {
        declare_view(realization.authored_storage);
        for (auto& view : realization.retained_sources) declare_view(view);
        for (auto& source : realization.composition_sources) {
            declare_view(source.storage);
        }
    }
    return {};
}

std::expected<PackageImportPlan, std::string> plan_package_imports(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    BackgroundEvaluationPlan const& background)
{
    PackageImportPlan plan;
    if (analysis.empty && input.config_relocations.empty()) return plan;

    auto find_package_index = [&](PackageCode const* package)
        -> std::expected<std::size_t, std::string> {
        if (!package) {
            return std::unexpected("GraphJit lowering import source has no package code");
        }
        for (std::size_t i = 0; i < input.packages.size(); ++i) {
            if (&input.packages[i] == package) return i;
        }
        return std::unexpected("resolved package code is not a GraphJit lowering input");
    };

    auto find_revision_package_index = [&](PackageRevision const* revision)
        -> std::expected<std::size_t, std::string> {
        std::optional<std::size_t> selected;
        for (std::size_t i = 0; i < input.packages.size(); ++i) {
            if (input.packages[i].revision.get() != revision) continue;
            if (selected) {
                return std::unexpected(
                    "GraphJit lowering received duplicate package code for one package revision");
            }
            selected = i;
        }
        if (!selected) {
            return std::unexpected(
                "configuration relocation package revision has no package code input");
        }
        return *selected;
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
                            std::string const& symbol,
                            std::string role,
                            CallbackImportAbi abi) -> std::string {
        for (auto const& callback : package.callbacks) {
            if (callback.symbol == symbol) return callback.symbol;
        }
        package.callbacks.push_back(CallbackImportPlan{
            .symbol = symbol,
            .role = std::move(role),
            .abi = abi,
        });
        return symbol;
    };

    auto add_retained_global = [&](
                                   PackageImportGroup& package,
                                   PackageRetainedGlobal const& global) {
        for (auto const& imported : package.retained_globals) {
            if (imported.symbol == global.symbol) return;
        }
        package.retained_globals.push_back(RetainedGlobalImportPlan{
            .symbol = global.symbol,
            .size = global.size,
            .alignment = global.alignment,
        });
    };

    plan.primitive_callbacks.reserve(analysis.primitives.size());
    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const& primitive = analysis.primitives[i];
        auto const& implementation = *primitive.implementation;
        if (implementation.resolved_module_input || implementation.observation_slot || implementation.native_background_node) {
            auto add = [&](std::string const& symbol, CallbackImportAbi abi) {
                if (symbol.empty()) return;
                if (!std::ranges::any_of(plan.compiler_callbacks,
                        [&](auto const& callback) { return callback.symbol == symbol; }))
                    plan.compiler_callbacks.push_back({symbol, "module input resolution", abi});
            };
            add(implementation.tick_block, CallbackImportAbi::block);
            add(implementation.tick_block_batch, CallbackImportAbi::batch);
            add(implementation.skip_block, CallbackImportAbi::block);
            add(implementation.skip_block_batch, CallbackImportAbi::batch);
            add(implementation.tock_coverage, CallbackImportAbi::background);
            add(implementation.tock_coverage_batch, CallbackImportAbi::batch);
            add(implementation.propagate_forward_coverage, CallbackImportAbi::background);
            add(implementation.propagate_forward_coverage_batch, CallbackImportAbi::batch);
            add(implementation.propagate_reverse_coverage, CallbackImportAbi::background);
            add(implementation.propagate_reverse_coverage_batch, CallbackImportAbi::batch);
            add(implementation.initialize_sequential_input, CallbackImportAbi::sequential_initialize);
            add(implementation.combine_sequential_input_values, CallbackImportAbi::sequential_combine);
            plan.primitive_callbacks.push_back({
                .bundle = primitive.bundle.node_bundle,
                .resolves_module_input = implementation.resolved_module_input.has_value(),
                .tick_block = implementation.tick_block,
                .tick_block_batch = implementation.tick_block_batch,
                .skip_block = implementation.skip_block,
                .skip_block_batch = implementation.skip_block_batch,
                .tock_coverage = implementation.tock_coverage,
                .tock_coverage_batch = implementation.tock_coverage_batch,
                .propagate_forward_coverage = implementation.propagate_forward_coverage,
                .propagate_forward_coverage_batch = implementation.propagate_forward_coverage_batch,
                .propagate_reverse_coverage = implementation.propagate_reverse_coverage,
                .propagate_reverse_coverage_batch = implementation.propagate_reverse_coverage_batch,
                .initialize_sequential_input = implementation.initialize_sequential_input,
                .combine_sequential_input_values = implementation.combine_sequential_input_values,
            });
            continue;
        }
        auto package_index = find_package_index(implementation.package);
        if (!package_index) return std::unexpected(std::move(package_index.error()));
        auto& package = package_group(*package_index);

        PrimitiveCallbackPlan callbacks{
            .bundle = primitive.bundle.node_bundle,
        };
        if (!implementation.tick_block.empty()) {
            callbacks.tick_block = add_callback(
                package, implementation.tick_block, "tick_block", CallbackImportAbi::block);
            callbacks.tick_block_batch = add_callback(
                package,
                implementation.tick_block_batch,
                "tick_block_batch",
                CallbackImportAbi::batch);
            if (primitive.bundle.block_skippable) {
                callbacks.skip_block = add_callback(
                    package, implementation.skip_block, "skip_block", CallbackImportAbi::block);
                callbacks.skip_block_batch = add_callback(
                    package,
                    implementation.skip_block_batch,
                    "skip_block_batch",
                    CallbackImportAbi::batch);
            }
        }
        if (!implementation.initialize_sequential_input.empty()) {
            callbacks.initialize_sequential_input = add_callback(
                package,
                implementation.initialize_sequential_input,
                "initialize_sequential_input",
                CallbackImportAbi::sequential_initialize);
        }
        if (!implementation.combine_sequential_input_values.empty()) {
            callbacks.combine_sequential_input_values = add_callback(
                package,
                implementation.combine_sequential_input_values,
                "combine_sequential_input_values",
                CallbackImportAbi::sequential_combine);
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
                if (implementation.tock_coverage.empty()
                    || implementation.propagate_forward_coverage.empty()) {
                    return std::unexpected(
                        "GraphJit authored Tock node has incomplete background callbacks");
                }
                callbacks.tock_coverage = add_callback(
                    package,
                    implementation.tock_coverage,
                    "tock_coverage",
                    CallbackImportAbi::background);
                callbacks.tock_coverage_batch = add_callback(
                    package,
                    implementation.tock_coverage_batch,
                    "tock_coverage_batch",
                    CallbackImportAbi::batch);
            }
            if (node.authored_forward_coverage) {
                callbacks.propagate_forward_coverage = add_callback(
                    package,
                    implementation.propagate_forward_coverage,
                    "propagate_forward_coverage",
                    CallbackImportAbi::background);
                callbacks.propagate_forward_coverage_batch = add_callback(
                    package,
                    implementation.propagate_forward_coverage_batch,
                    "propagate_forward_coverage_batch",
                    CallbackImportAbi::batch);
            }
            if (node.authored_reverse_coverage) {
                if (node.accumulators.input_requirement_count != 0) {
                    if (implementation.propagate_reverse_coverage.empty()) {
                        return std::unexpected(
                            "GraphJit authored Tock node with background inputs has no reverse callback");
                    }
                    callbacks.propagate_reverse_coverage = add_callback(
                        package,
                        implementation.propagate_reverse_coverage,
                        "propagate_reverse_coverage",
                        CallbackImportAbi::background);
                    callbacks.propagate_reverse_coverage_batch = add_callback(
                        package,
                        implementation.propagate_reverse_coverage_batch,
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
        if (relocation.relocation->immutable_data) {
            auto const& data = *relocation.relocation->immutable_data;
            if (relocation.relocation->retained_global_index || relocation.revision
                || relocation.retained_global || !data.storage || data.bytes.empty()
                || !std::has_single_bit(data.alignment)
                || relocation.relocation->addend >= data.bytes.size()) {
                return std::unexpected("invalid immutable configuration data relocation");
            }
            continue;
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
                "non-null configuration relocation has no retained package global");
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
        auto package_index = find_revision_package_index(relocation.revision.get());
        if (!package_index) return std::unexpected(std::move(package_index.error()));
        auto const& package_input = input.packages[*package_index];
        if (index >= package_input.retained_globals.size()
            || &package_input.retained_globals[index] != relocation.retained_global) {
            return std::unexpected(
                "configuration relocation retained global is absent from its package code");
        }
        if (relocation.retained_global->symbol.empty()) {
            return std::unexpected(
                "configuration relocation target is not an immutable defined LLVM global");
        }
        if (relocation.retained_global->size != accepted.size) {
            return std::unexpected(
                "configuration relocation retained global disagrees with the accepted revision");
        }
        add_retained_global(package_group(*package_index), *relocation.retained_global);
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
                "non-null configuration relocation lost its retained package global");
        }
        for (auto const& package : imports.packages) {
            for (auto const& global : package.retained_globals) {
                if (global.symbol == relocation.retained_global->symbol) {
                    if (relocation.relocation->addend >= global.size) {
                        return std::unexpected(
                            "configuration relocation addend lies outside retained package global");
                    }
                    return global.symbol;
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
            .module_input_config = primitive.implementation->module_input_config
                ? std::optional(*primitive.implementation->module_input_config) : std::nullopt,
            .bytes = primitive.bundle.node_size
                ? std::vector<std::byte>(first, first + primitive.bundle.node_size)
                : std::vector<std::byte>{},
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
            if (imported->empty() && !relocation.relocation->immutable_data
                && relocation.relocation->addend != 0) {
                return std::unexpected(
                    "explicit-null configuration relocation has a non-zero addend");
            }

            std::fill_n(node.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                sizeof(void*), std::byte{0});
            node.relocations.push_back(NodeConfigurationRelocationPlan{
                .byte_offset = offset,
                .addend = relocation.relocation->addend,
                .retained_global_symbol = std::move(*imported),
                .immutable_data = relocation.relocation->immutable_data,
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
    // the realtime root needs to switch cyclic SCCs from the ordinary
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
        if (source_region.execution_order.empty()) continue;
        schedule_region_execution_region[region_index] = execution_region_index;
        plan.regions.push_back(std::move(region));
    }

    auto append_step = [&](NodeBundleHandle bundle, std::size_t execution_region)
        -> std::expected<std::size_t, std::string> {
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
            .tick_batch_callback_symbol = callbacks.tick_block_batch,
            .skip_batch_callback_symbol = callbacks.skip_block_batch,
        });
        plan.regions[execution_region].primitive_steps.push_back(step_index);
        return step_index;
    };

    plan.ready_groups.reserve(connections.schedule.ready_groups.size());
    for (auto const& scheduled_group : connections.schedule.ready_groups) {
        if (scheduled_group.empty()) {
            return std::unexpected(
                "GraphJit connection schedule contains an empty ready group");
        }
        ExecutionReadyGroupPlan group;
        auto const group_is_cyclic = connections.schedule
            .regions[scheduled_group.front()].cyclic;
        std::vector<std::size_t> execution_regions;
        execution_regions.reserve(scheduled_group.size());
        for (auto const schedule_region : scheduled_group) {
            if (schedule_region >= schedule_region_execution_region.size()
                || !schedule_region_execution_region[schedule_region]) {
                return std::unexpected(
                    "GraphJit ready group references a missing execution region");
            }
            auto const execution_region =
                *schedule_region_execution_region[schedule_region];
            auto const& source_region = connections.schedule.regions[schedule_region];
            if (source_region.cyclic != group_is_cyclic) {
                return std::unexpected(
                    "GraphJit ready group mixes cyclic and acyclic regions");
            }
            execution_regions.push_back(execution_region);
        }
        if (group_is_cyclic) {
            auto const phase_count = connections.schedule
                .regions[scheduled_group.front()].execution_order.size();
            for (auto const schedule_region : scheduled_group) {
                if (connections.schedule.regions[schedule_region]
                        .execution_order.size() != phase_count) {
                    return std::unexpected(
                        "GraphJit cyclic ready group has unequal execution phase counts");
                }
            }
            group.cyclic_regions = execution_regions;
            for (std::size_t phase = 0; phase < phase_count; ++phase) {
                for (std::size_t lane = 0; lane < scheduled_group.size(); ++lane) {
                    auto step = append_step(
                        connections.schedule.regions[scheduled_group[lane]]
                            .execution_order[phase],
                        execution_regions[lane]);
                    if (!step) return std::unexpected(std::move(step.error()));
                    group.primitive_steps.push_back(*step);
                }
            }
        } else {
            for (std::size_t lane = 0; lane < scheduled_group.size(); ++lane) {
                auto const& source_region = connections.schedule
                    .regions[scheduled_group[lane]];
                if (source_region.execution_order.size() != 1) {
                    return std::unexpected(
                        "GraphJit acyclic ready group has an invalid execution region");
                }
                auto step = append_step(
                    source_region.execution_order.front(), execution_regions[lane]);
                if (!step) return std::unexpected(std::move(step.error()));
                group.primitive_steps.push_back(*step);
            }
        }
        if (group.cyclic_regions.empty() && group.primitive_steps.size() > 1) {
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
    for (auto const& region : plan.regions) {
        if (region.cyclic && region.primitive_steps.empty()) {
            return std::unexpected(
                "GraphJit cyclic execution region contains no executable primitive");
        }
    }
    if (plan.ready_groups.empty() && !plan.regions.empty()) {
        return std::unexpected(
            "GraphJit connection schedule lost its ready-frontier groups");
    }
    for (std::size_t i = 0; i < scheduled.size(); ++i) {
        auto const tick = !imports.primitive_callbacks[i].tick_block.empty();
        auto const executes = tick
            && !sample_ports.primitives[i].tick_callback_elided;
        if (scheduled[i] != executes) {
            return std::unexpected(executes
                ? "GraphJit connection schedule omitted an executable Tick primitive"
                : "GraphJit connection schedule included a non-executable primitive");
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
        if (carry.cyclic_region_scope) {
            if (*carry.cyclic_region_scope
                    >= schedule_region_execution_region.size()
                || !schedule_region_execution_region[
                    *carry.cyclic_region_scope]) {
                return std::unexpected(
                    "GraphJit sample carry operation references a missing cyclic region");
            }
            auto const execution_region = *schedule_region_execution_region[
                *carry.cyclic_region_scope];
            if (!plan.regions[execution_region].cyclic
                || step_regions[carry.restore_execution_position]
                    != execution_region
                || step_regions[carry.commit_execution_position]
                    != execution_region) {
                return std::unexpected(
                    "GraphJit sample carry operation has an inconsistent cyclic scope");
            }
            plan.regions[execution_region]
                .sample_carry_restores_before.push_back(carry_index);
            plan.regions[execution_region]
                .sample_carry_commits_after.push_back(carry_index);
            continue;
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
        if (analysis.primitives[primitive_index].implementation->tick_block.empty()) {
            continue;
        }
        // A callback-elided producer has no execution step. Its eligibility
        // already requires that it have no event output.
        if (sample_ports.primitives[primitive_index].tick_callback_elided) {
            if (!event_ports.primitives[primitive_index].outputs.empty()) {
                return std::unexpected(
                    "GraphJit callback-elided primitive has an event output");
            }
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
            && (!region.sample_carry_restores_before.empty()
                || !region.sample_carry_commits_after.empty()
                || !region.event_operations_before.empty()
                || !region.event_operations_after.empty())) {
            return std::unexpected(
                "GraphJit acyclic execution region unexpectedly owns region-scoped operations");
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
    ExecutionReadyGroupPlan const& group,
    bool allow_step_operations = false,
    std::size_t required_block_size = 0)
{
    if (!group.cyclic_regions.empty()) {
        if (group.cyclic_regions.size() < 2) {
            return false;
        }
        auto const first_region_index = group.cyclic_regions.front();
        if (first_region_index >= execution.regions.size()) {
            return std::unexpected(
                "GraphJit realtime cyclic batch references a missing region");
        }
        auto const& first_region = execution.regions[first_region_index];
        if (group.primitive_steps.size()
            != group.cyclic_regions.size()
                * first_region.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit realtime cyclic batch has an invalid phase layout");
        }
        for (auto const region_index : group.cyclic_regions) {
            if (region_index >= execution.regions.size()) {
                return std::unexpected(
                    "GraphJit realtime cyclic batch references a missing region");
            }
            auto const& region = execution.regions[region_index];
            if (!region.cyclic
                || region.maximum_block_size != first_region.maximum_block_size
                || region.scc_feedback_latency != first_region.scc_feedback_latency
                || region.primitive_steps.size()
                    != first_region.primitive_steps.size()) {
                return false;
            }
        }
        for (std::size_t phase = 0;
             phase < first_region.primitive_steps.size();
             ++phase) {
            ExecutionReadyGroupPlan phase_group;
            phase_group.primitive_steps.reserve(group.cyclic_regions.size());
            for (std::size_t lane = 0; lane < group.cyclic_regions.size(); ++lane) {
                auto const step = execution.regions[group.cyclic_regions[lane]]
                    .primitive_steps[phase];
                if (group.primitive_steps[
                        phase * group.cyclic_regions.size() + lane]
                    != step) {
                    return std::unexpected(
                        "GraphJit realtime cyclic batch phase does not match its regions");
                }
                phase_group.primitive_steps.push_back(step);
            }
            auto batches = realtime_ready_group_batches(
                connections,
                declarations,
                imports,
                sample_ports,
                event_ports,
                root_stack,
                execution,
                phase_group,
                true,
                first_region.maximum_block_size);
            if (!batches || !*batches) return batches;
        }
        return true;
    }
    if (group.primitive_steps.size() < 2) return false;
    if (required_block_size == 0) {
        required_block_size = declarations.node_layout.max_block_size;
    }

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
            || step.maximum_block_size < required_block_size) {
            return false;
        }

        // These operations delimit a producer's authored callback. Moving all
        // pre-operations before one batch and all post-operations after it is
        // legal for an antichain, but it can extend transient live ranges and
        // increase stack pressure. The initial cost model keeps such groups
        // scalar; Front 16 can cost that transformation with layout jointly.
        if (!allow_step_operations && step.has_port_operations()) {
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
            if (!allow_step_operations
                && (tick_invocation.sample_capture_count != 0
                    || tick_invocation.event_capture_count != 0)) {
                return false;
            }
        }
        lanes.push_back(std::addressof(step));
    }
    // Transient placement uses the batch-aware execution positions. Keep this
    // physical check as an independent invariant: persistent storage, a
    // transition specialization, or a future storage policy must not make two
    // callback lanes alias writable bytes. Shared read-only inputs remain
    // legal.
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
            if (input.has_contiguous_scratch) {
                auto const channels = channel_count(input.channel_layout);
                auto const value_size = input.geometry.value_size_bytes;
                if (channels == 0 || value_size == 0
                    || input.contiguous_storage_capacity == 0
                    || input.contiguous_storage_capacity
                        > std::numeric_limits<std::size_t>::max() / value_size
                    || channels
                        > std::numeric_limits<std::size_t>::max()
                            / (input.contiguous_storage_capacity * value_size)
                    || root_stack.sample_offset
                        > std::numeric_limits<std::size_t>::max()
                            - input.contiguous_scratch_offset) {
                    return std::unexpected(
                        "GraphJit realtime batch continuous input scratch is invalid");
                }
                auto appended = append_range(
                    CallbackStorageBase::root_transient,
                    root_stack.sample_offset + input.contiguous_scratch_offset,
                    channels * input.contiguous_storage_capacity * value_size,
                    lane_index,
                    true);
                if (!appended) {
                    return std::unexpected(std::move(appended.error()));
                }
            }
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

std::expected<void, std::string> configure_background_replay(
    BackgroundEvaluationPlan& background, GraphAnalysis const& analysis)
{
    for (auto& node : background.nodes) {
        auto const primitive = std::ranges::find_if(
            analysis.primitives,
            [&](PrimitiveAnalysis const& candidate) {
                return candidate.bundle.node_bundle == node.bundle;
            });
        if (primitive == analysis.primitives.end()
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
            analysis.primitives,
            [&](PrimitiveAnalysis const& candidate) {
                return candidate.bundle.node_bundle == bundle;
            });
        if (primitive == analysis.primitives.end()
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
}

std::expected<void, std::string> bind_event_capture_capacities(
    BackgroundEvaluationPlan& background,
    GraphAnalysis const& analysis,
    EventPortBindingPlan const& event_ports)
{
    for (auto& capture : background.tick_runtime.event_captures) {
        if (capture.port >= background.ports.size()) {
            return std::unexpected("GraphJit Event capture references a missing port");
        }
        auto const port = background.ports[capture.port].configured_port;
        std::size_t primitive = 0;
        while (primitive < analysis.primitives.size()
            && analysis.primitives[primitive].bundle.node_bundle != port.node_bundle_handle) {
            ++primitive;
        }
        if (primitive >= event_ports.primitives.size()
            || port.port_index >= event_ports.primitives[primitive].outputs.size()) {
            return std::unexpected("GraphJit recorded Event output has no primitive binding");
        }
        auto const storage = event_ports.primitives[primitive].outputs[port.port_index].storage;
        if (!storage || *storage >= event_ports.ports.size()) {
            return std::unexpected("GraphJit recorded Event output has no callback storage");
        }
        capture.event_acceptance_capacity = event_ports.ports[*storage].event_capacity;
    }
    return {};
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

    std::vector<std::optional<Sample>> static_sample_values(
        input.graph.node_bundles.size());
    std::size_t static_value_bundle = 0;
    input.graph.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            if (view.static_sample_value) {
                static_sample_values.at(static_value_bundle) =
                    *view.static_sample_value;
            }
            ++static_value_bundle;
        });
    std::vector<NodeBundleHandle> callback_elision_candidates;
    for (auto const& primitive : analysis->primitives) {
        auto const bundle = primitive.bundle.node_bundle;
        if (bundle >= static_sample_values.size()
            || !static_sample_values[bundle]) {
            continue;
        }
        auto const& configured = input.graph.node_bundles.bundle(bundle);
        auto const& implementation = *primitive.implementation;
        if (!configured.is_concrete()
            || configured.sample_input_count() != 0
            || configured.sample_output_count() != 1
            || configured.event_input_count() != 0
            || configured.event_output_count() != 0
            || implementation.state_size != 0
            || implementation.background_state_size != 0) {
            return std::unexpected(
                "GraphJit static_sample_value requires one stateless concrete Tick sample output and no inputs");
        }
        NodeBundlePortId const output_port{bundle, PortKind::sample, 0};
        auto const output = input.graph.node_bundles.resolve_sample_output(
            output_port).config;
        auto const& resolved = port_realization->output(bundle, 0);
        if (!is_sequential_output(output) || is_buffered(output)
            || resolved.value_type != ContinuousValueTypeId::sample
            || resolved.size.has_value()) {
            return std::unexpected(
                "GraphJit static_sample_value requires one ephemeral scalar Sample Tick output");
        }
        callback_elision_candidates.push_back(bundle);
    }

    // The identifier has no semantic meaning outside this compilation. Equal
    // values mean that the package callback and every callback-facing solved
    // port fact are equal. Configured values, state, and storage addresses are
    // intentionally absent.
    auto same_callback_batch_class = [&](PrimitiveAnalysis const& left,
                                         PrimitiveAnalysis const& right) {
        if ((!left.implementation->tick_block.empty())
                != (!right.implementation->tick_block.empty())
            || left.implementation->code_key
                != right.implementation->code_key
            || left.implementation->revision.get()
                != right.implementation->revision.get()
            || left.implementation->provider_version
                != right.implementation->provider_version
            || (!left.implementation->tick_block.empty()
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
            if (is_sequential_output(config)
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
                view.replay_mode != TickReplayMode::disabled;
        });

    for (NodeBundleHandle bundle = 0;
         bundle < input.graph.node_bundles.size(); ++bundle) {
        auto const& configured = input.graph.node_bundles.bundle(bundle);
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
        SamplePortBindingPlan sample_ports{};
        EventPortBindingPlan event_ports{};
        RootStackBufferPlan root_stack{};
    };

    // Background topology keeps all callback batch classes. Only a change
    // to the realtime batch classes requires a new connection analysis.
    auto const& configured_cost_model = input.realtime_storage_cost_model;
    auto topology = ConnectionTopology::build(
        input.graph, *port_realization, input.specialization.block_size,
        callback_batch_classes, input.configured_bundles, input.bundle_migration_identities);
    if (!topology) return std::unexpected(std::move(topology.error()));
    auto imports = plan_package_imports(
        input, *analysis, topology->background_topology());
    if (!imports) return std::unexpected(std::move(imports.error()));
    std::vector<std::size_t> realtime_batch_classes = callback_batch_classes;
    auto const no_batch_class = std::numeric_limits<std::size_t>::max();

    auto const stack_budget = configured_cost_model.stack_budget_bytes;
    for (;;) {
        auto connections = ConnectionTopology(*topology).build_analysis_plan(
            realtime_batch_classes, callback_elision_candidates);
        if (!connections) return std::unexpected(std::move(connections.error()));

        auto prepared_sample_connections = prepare_sample_storage_connections(
            *analysis, *connections);
        if (!prepared_sample_connections) {
            return std::unexpected(std::move(prepared_sample_connections.error()));
        }
        SampleStorageConnections const storage_connections{
            .analysis = *connections,
            .sample_connections = *prepared_sample_connections,
        };

        auto plan_realtime_ports = [&](RealtimeStorageCostModel const& cost_model)
            -> std::expected<RealtimePortPlans, std::string> {
            auto sample_ports = plan_sample_ports(
                input.graph,
                input.specialization.block_size,
                *analysis,
                storage_connections,
                *port_realization,
                cost_model,
                static_sample_values);
            if (!sample_ports) {
                return std::unexpected(std::move(sample_ports.error()));
            }
            auto event_ports = plan_event_ports(
                input.graph, input.specialization.block_size, *analysis,
                *connections, *port_realization, cost_model);
            if (!event_ports) {
                return std::unexpected(std::move(event_ports.error()));
            }
            auto root_stack = plan_root_stack_buffers(*sample_ports, *event_ports);
            if (!root_stack) {
                return std::unexpected(std::move(root_stack.error()));
            }
            return RealtimePortPlans{
                .sample_ports = std::move(*sample_ports),
                .event_ports = std::move(*event_ports),
                .root_stack = *root_stack,
            };
        };

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
            continue;
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
            *connections,
            *port_realization,
            realtime_ports->sample_ports,
            realtime_ports->event_ports);
        if (!declarations) return std::unexpected(std::move(declarations.error()));

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

        auto execution = plan_execution(
            *analysis,
            *connections,
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
            if ((!group.cyclic_regions.empty()
                    && group.cyclic_regions.size() < 2)
                || (group.cyclic_regions.empty()
                    && group.primitive_steps.size() < 2)) {
                continue;
            }
            auto batches = realtime_ready_group_batches(
                *connections,
                *declarations,
                *imports,
                realtime_ports->sample_ports,
                realtime_ports->event_ports,
                realtime_ports->root_stack,
                *execution,
                group);
            if (!batches) return std::unexpected(std::move(batches.error()));
            if (*batches) continue;
            if (!group.cyclic_regions.empty()) {
                for (auto const region_index : group.cyclic_regions) {
                    for (auto const step_index :
                         execution->regions[region_index].primitive_steps) {
                        auto const configuration = execution
                            ->primitive_steps[step_index].configuration_index;
                        rejected_bundles.push_back(
                            imports->primitive_callbacks[configuration].bundle);
                    }
                }
            } else {
                for (auto const step_index : group.primitive_steps) {
                    auto const configuration = execution
                        ->primitive_steps[step_index].configuration_index;
                    rejected_bundles.push_back(
                        imports->primitive_callbacks[configuration].bundle);
                }
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

        auto replay = configure_background_replay(connections->background, *analysis);
        if (!replay) return std::unexpected(std::move(replay.error()));
        auto captures = bind_event_capture_capacities(
            connections->background, *analysis, realtime_ports->event_ports);
        if (!captures) return std::unexpected(std::move(captures.error()));

        for (auto& node : connections->background.nodes) {
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
                input.graph,
                *analysis,
                *connections,
                *declarations,
                realtime_ports->sample_ports);
        if (!realtime_port_state_realizations) {
            return std::unexpected(
                std::move(realtime_port_state_realizations.error()));
        }
        auto event_realtime_port_state_realizations =
            plan_event_realtime_port_state_realizations(
                input.graph,
                *analysis,
                *connections,
                *declarations,
                realtime_ports->event_ports);
        if (!event_realtime_port_state_realizations) {
            return std::unexpected(
                std::move(event_realtime_port_state_realizations.error()));
        }
        realtime_port_state_realizations->event_states =
            std::move(*event_realtime_port_state_realizations);
        auto fresh = declare_fresh_sample_storage_values(
            declarations->node_layout,
            realtime_ports->sample_ports.storage,
            connections->realtime_port_states,
            *realtime_port_state_realizations);
        if (!fresh) return std::unexpected(std::move(fresh.error()));

        auto configurations = plan_node_configurations(
            input, *analysis, *connections, *imports);
        if (!configurations) {
            return std::unexpected(std::move(configurations.error()));
        }

        return LoweringPlan{
            .port_realization = std::move(*port_realization),
            .connections = std::move(*connections),
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
        if ((!group.cyclic_regions.empty()
                && group.cyclic_regions.size() < 2)
            || (group.cyclic_regions.empty()
                && group.primitive_steps.size() < 2)) {
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
        if (!group.cyclic_regions.empty()) {
            for (auto const region : group.cyclic_regions) {
                groups.push_back(ExecutionReadyGroupPlan{
                    .primitive_steps = plan.execution.regions[region].primitive_steps,
                    .cyclic_regions = {region},
                });
            }
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
