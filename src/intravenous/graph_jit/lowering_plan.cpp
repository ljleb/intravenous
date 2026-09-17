#include <intravenous/graph_jit/lowering_plan.h>
#include <intravenous/graph_jit/sample_physical_plan.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <llvm/IR/Function.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace iv::graph_jit::detail {
namespace {
struct PrimitiveBundle {
    std::size_t node_bundle = 0;
    std::size_t node_size = 0;
    std::size_t node_alignment = 1;
    std::size_t maximum_block_size = 0;
    bool block_skippable = false;
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
    LoweringInput const& input,
    ConnectionAnalysisPlan const& connections)
{
    if (!connections.event_connections.empty()) {
        return std::unexpected(
            "GraphJit lowering does not yet support event connections");
    }
    std::vector<PrimitiveBundle> primitives;
    std::string structural_error;
    std::size_t boundary_count = 0;
    std::size_t node_bundle = 0;
    input.graph.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            auto const current_bundle = node_bundle++;
            if (!structural_error.empty()) return;

            if (view.kind == ConfiguredNodeBundleKind::boundary) {
                ++boundary_count;
                if (boundary_count != 1
                    || current_bundle != input.graph.public_ports.boundary_handle()) {
                    structural_error =
                        "GraphJit sample-edge slice requires exactly one project boundary";
                }
                return;
            }
            if (view.kind != ConfiguredNodeBundleKind::concrete) {
                structural_error =
                    "GraphJit sample-edge slice supports only flat concrete primitives";
                return;
            }
            if (!view.ports) {
                structural_error =
                    "GraphJit sample-edge slice requires concrete primitive port metadata";
                return;
            }
            if (view.ports->event_input_count() != 0
                || view.ports->event_output_count() != 0) {
                structural_error =
                    "GraphJit sample-edge slice does not yet support primitive event ports";
                return;
            }
            if (!is_power_of_two(view.node_alignment)) {
                structural_error =
                    "configured primitive has invalid node configuration alignment";
                return;
            }
            if ((view.lifetime && view.lifetime->ttl_samples)
                || (view.deferred_detach && view.deferred_detach->has_value())) {
                structural_error =
                    "GraphJit sample-edge slice does not yet support activity or detach semantics";
                return;
            }
            if (!is_power_of_two(view.maximum_block_size)) {
                structural_error =
                    "configured primitive has invalid maximum block size";
                return;
            }
            primitives.push_back(PrimitiveBundle{
                .node_bundle = current_bundle,
                .node_size = view.node_size,
                .node_alignment = view.node_alignment,
                .maximum_block_size = view.maximum_block_size,
                .block_skippable = view.block_skippable,
            });
        });

    if (!structural_error.empty()) return std::unexpected(std::move(structural_error));
    if (boundary_count != 1) {
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
    LoweringInput const& input,
    ConnectionAnalysisPlan const& connections)
{
    if (is_structurally_empty(input.graph)) {
        return GraphAnalysis{.empty = true};
    }

    auto primitive_bundles = supported_primitives(input, connections);
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
        if (!implementation->package_module || !implementation->tick_block
            || !implementation->declare_node || !implementation->node_data) {
            return std::unexpected(
                "resolved primitive implementation is incomplete at the GraphJit lowering boundary");
        }
        if (primitive.block_skippable && !implementation->skip_block) {
            return std::unexpected(
                "block-skippable primitive has no resolved skip_block implementation");
        }
        if (implementation->tick_block->getParent() != implementation->package_module
            || (implementation->skip_block
                && implementation->skip_block->getParent()
                    != implementation->package_module)) {
            return std::unexpected(
                "resolved primitive callbacks do not belong to the selected package LLVM module");
        }
        if (primitive.node_size == 0) {
            return std::unexpected("configured primitive has empty node configuration storage");
        }

        analysis.primitives.push_back(PrimitiveAnalysis{
            .bundle = primitive,
            .implementation = implementation,
        });
    }

    if (input.node_implementations.size() != analysis.primitives.size()) {
        return std::unexpected(
            "GraphJit sample-edge slice requires one resolved implementation per concrete primitive");
    }
    return analysis;
}

std::expected<DeclarationPlan, std::string> plan_declarations(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    SamplePortBindingPlan& sample_ports)
{
    NodeLayoutBuilder layout_builder(input.specialization.block_size);
    if (analysis.empty) {
        return DeclarationPlan{
            .node_layout = std::move(layout_builder).build(),
        };
    }

    std::vector<std::size_t> declared_node_indices;
    declared_node_indices.reserve(analysis.primitives.size());
    for (auto const& primitive : analysis.primitives) {
        auto const& implementation = *primitive.implementation;
        declared_node_indices.push_back(implementation.declare_node(
            implementation.node_data,
            implementation.state_structures,
            layout_builder));
    }

    for (auto const& primitive : sample_ports.primitives) {
        for (auto const& binding : primitive.inputs) {
            if (!binding.representation) {
                return std::unexpected(
                    "GraphJit sample runtime declaration has an unbound input port");
            }
        }
        for (auto const& binding : primitive.outputs) {
            if (!binding.representation) {
                return std::unexpected(
                    "GraphJit sample runtime declaration has an unbound output port");
            }
        }
    }

    auto declared_sample_storage = declare_sample_physical_storage(
        layout_builder, sample_ports.physical);
    if (!declared_sample_storage) {
        return std::unexpected(std::move(declared_sample_storage.error()));
    }

    auto node_layout = std::move(layout_builder).build();
    if (node_layout.nodes.size() != analysis.primitives.size()) {
        return std::unexpected(
            "GraphJit sample-edge slice does not yet support nested node declarations");
    }
    for (std::size_t i = 0; i < declared_node_indices.size(); ++i) {
        if (declared_node_indices[i] != i) {
            return std::unexpected(
                "GraphJit sample-edge slice does not yet support nested node declarations");
        }
    }
    if (!node_layout.imported_arrays.empty() || !node_layout.exported_arrays.empty()) {
        return std::unexpected(
            "GraphJit sample-edge slice does not yet support declared shared-array bindings");
    }
    auto sample_physical_owns_region =
        [&](std::size_t region_index) {
            if (sample_ports.physical.transient_region.valid()
                && region_index == sample_ports.physical.transient_region.index) {
                return true;
            }
            return std::any_of(
                sample_ports.physical.persistent_allocations.begin(),
                sample_ports.physical.persistent_allocations.end(),
                [&](SamplePersistentAllocationPlan const& allocation) {
                    return allocation.region.valid()
                        && allocation.region.index == region_index;
                });
        };

    for (std::size_t region_index = 0;
         region_index < node_layout.regions.size();
         ++region_index) {
        auto const& region = node_layout.regions[region_index];
        if (region.kind == NodeLayout::Region::Kind::state
            || region.kind == NodeLayout::Region::Kind::compiled_state) {
            continue;
        }
        if (region.kind == NodeLayout::Region::Kind::raw
            && sample_physical_owns_region(region_index)) {
            continue;
        }
        return std::unexpected(
            "GraphJit sample-edge slice does not yet support declaration-owned auxiliary storage regions");
    }

    auto finalized_sample_storage = finalize_sample_physical_storage(
        node_layout, sample_ports.physical);
    if (!finalized_sample_storage) {
        return std::unexpected(std::move(finalized_sample_storage.error()));
    }


    DeclarationPlan plan{
        .node_layout = std::move(node_layout),
    };
    plan.primitive_storage.reserve(analysis.primitives.size());
    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const& implementation = *analysis.primitives[i].implementation;
        auto const& node_record = plan.node_layout.nodes[i];
        if (node_record.state_size != implementation.state_size
            || node_record.state_alignment != implementation.state_alignment
            || node_record.compiled_state_size != implementation.compiled_state_size
            || node_record.compiled_state_alignment
                != implementation.compiled_state_alignment) {
            return std::unexpected(
                "declared primitive State/CompiledState layout disagrees with finalized compiler metadata");
        }
        if (node_record.state_size != 0 && node_record.state_offset < 0) {
            return std::unexpected(
                "declared primitive State has no canonical NodeLayout storage offset");
        }
        if (node_record.compiled_state_size != 0
            && node_record.compiled_state_offset < 0) {
            return std::unexpected(
                "declared primitive CompiledState has no canonical NodeLayout storage offset");
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
        if (node_record.compiled_state_size != 0) {
            storage.has_compiled_state = true;
            storage.compiled_state_offset =
                static_cast<std::size_t>(node_record.compiled_state_offset);
            if (storage.compiled_state_offset > plan.node_layout.storage_size
                || node_record.compiled_state_size
                    > plan.node_layout.storage_size - storage.compiled_state_offset) {
                return std::unexpected(
                    "declared primitive CompiledState lies outside canonical NodeStorage");
            }
            storage.compiled_state_size = node_record.compiled_state_size;
        }
        plan.primitive_storage.push_back(storage);
    }
    return plan;
}

std::expected<PackageImportPlan, std::string> plan_package_imports(
    LoweringInput const& input,
    GraphAnalysis const& analysis)
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
                            std::string role) -> std::string {
        for (auto const& callback : package.callbacks) {
            if (callback.source_symbol == source_symbol) {
                return callback.import_symbol;
            }
        }
        package.callbacks.push_back(CallbackImportPlan{
            .source_symbol = std::move(source_symbol),
            .import_symbol = std::move(import_symbol),
            .role = std::move(role),
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

        PrimitiveCallbackPlan callbacks;
        callbacks.tick_block = add_callback(
            package,
            implementation.tick_block->getName().str(),
            primitive_callback_import_symbol(i, "tick_block"),
            "tick_block");
        if (primitive.bundle.block_skippable) {
            callbacks.skip_block = add_callback(
                package,
                implementation.skip_block->getName().str(),
                primitive_callback_import_symbol(i, "skip_block"),
                "skip_block");
        }
        plan.primitive_callbacks.push_back(std::move(callbacks));
    }

    for (auto const& relocation : input.config_relocations) {
        if (!relocation.relocation) {
            return std::unexpected(
                "GraphJit lowering received an empty configuration relocation record");
        }
        if (!relocation.relocation->retained_global_ordinal) {
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
        auto const ordinal = *relocation.relocation->retained_global_ordinal;
        if (ordinal >= relocation.revision->retained_globals.size()) {
            return std::unexpected(
                "configuration relocation retained-global ordinal is out of range at lowering");
        }
        auto const& accepted = relocation.revision->retained_globals[ordinal];
        if (accepted.ordinal != ordinal || accepted.size == 0
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
        if (!relocation.relocation->retained_global_ordinal) return std::string{};
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
                .scc_feedback_latency = 0,
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
    ConnectionAnalysisPlan const& connections)
{
    SamplePortBindingPlan plan;
    if (analysis.empty) return plan;
    if (!connections.event_connections.empty()) {
        return std::unexpected(
            "GraphJit sample-edge slice does not yet support event connections");
    }
    if (connections.boundary_bundle < input.graph.node_bundles.size()) {
        auto const& boundary = input.graph.node_bundles.bundle(
            connections.boundary_bundle);
        if (boundary.sample_input_count() != 0
            || boundary.sample_output_count() != 0) {
            return std::unexpected(
                "GraphJit sample-edge slice does not yet support external sample boundaries");
        }
        if (boundary.event_input_count() != 0
            || boundary.event_output_count() != 0) {
            return std::unexpected(
                "GraphJit sample-edge slice does not yet support external event boundaries");
        }
    }

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

    struct ValidatedSampleEdge {
        std::size_t group_index = 0;
        std::size_t connection_index = 0;
        std::size_t source_primitive = 0;
        std::size_t source_port = 0;
        std::size_t target_primitive = 0;
        std::size_t target_port = 0;
        std::size_t source_history = 0;
        std::size_t source_latency = 0;
        std::size_t target_history = 0;
    };
    std::vector<ValidatedSampleEdge> validated_edges;
    validated_edges.reserve(connections.sample_producer_groups.size());

    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const* node = planned_node_for_bundle(
            analysis.primitives[i].bundle.node_bundle);
        if (!node) {
            return std::unexpected(
                "GraphJit sample-edge planning lost concrete-node port metadata");
        }
        plan.primitives[i].inputs.resize(node->sample_input_count);
        plan.primitives[i].outputs.resize(node->sample_output_count);
        if (node->event_input_count != 0 || node->event_output_count != 0) {
            return std::unexpected(
                "GraphJit sample-edge slice does not yet support primitive event ports");
        }
    }

    for (std::size_t group_index = 0;
         group_index < connections.sample_producer_groups.size(); ++group_index) {
        auto const& group = connections.sample_producer_groups[group_index];
        if (!group.has_realtime_connections) {
            return std::unexpected(
                "GraphJit sample-edge slice does not yet support compiled-only sample connections");
        }
        if (group.has_compiled_connections) {
            return std::unexpected(
                "GraphJit sample-edge slice does not yet support mixed realtime/compiled sample fanout");
        }
        if (!group.implementation) {
            return std::unexpected(
                "GraphJit sample-edge planning lost its realtime implementation choice");
        }
        if (*group.implementation == SampleConnectionImplementationKind::feedback_ring
            || *group.implementation
                == SampleConnectionImplementationKind::external_boundary) {
            return std::unexpected(
                "GraphJit sample-edge slice does not yet support feedback or external sample storage");
        }
        if (!group.canonical_source_layout) {
            return std::unexpected(
                "GraphJit sample fanout requires one canonical source output port");
        }

        for (auto const connection_index : group.connection_indices) {
            if (connection_index >= connections.sample_connections.size()) {
                return std::unexpected(
                    "GraphJit sample-edge plan contains an invalid connection index");
            }
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime) {
                return std::unexpected(
                    "GraphJit sample-edge slice supports only realtime-to-realtime sample connections");
            }
            if (connection.external_boundary) {
                return std::unexpected(
                    "GraphJit sample-edge slice does not yet support external sample boundaries");
            }
            if (connection.feedback) {
                return std::unexpected(
                    "GraphJit sample-edge slice does not yet support feedback sample connections");
            }
            if (!connection.canonical_source_port
                || !connection.canonical_source_layout) {
                return std::unexpected(
                    "GraphJit sample fanout requires one canonical source output port");
            }

            auto const source_port = *connection.canonical_source_port;
            auto const target_port = connection.target_port;
            auto const source_primitive = primitive_index_for_bundle(
                source_port.node_bundle_handle);
            auto const target_primitive = primitive_index_for_bundle(
                target_port.node_bundle_handle);
            if (!source_primitive || !target_primitive) {
                return std::unexpected(
                    "GraphJit sample-edge slice requires internal concrete sample endpoints");
            }
            if (source_port.port_kind != PortKind::sample
                || target_port.port_kind != PortKind::sample) {
                return std::unexpected(
                    "GraphJit sample-edge planning received a non-sample endpoint");
            }

            auto const source = input.graph.node_bundles
                .resolve_sample_output(source_port).config;
            auto const target = input.graph.node_bundles
                .resolve_sample_input(target_port).config;
            if (!is_realtime(source.access) || !is_realtime(target.access)) {
                return std::unexpected(
                    "GraphJit sample-edge slice requires realtime sample port declarations");
            }
            if (source.channel_layout != *group.canonical_source_layout
                || source.channel_layout != *connection.canonical_source_layout
                || target.channel_layout != connection.target_layout
                || connection.source_type != source.channel_layout.channel_type
                || connection.target_type != target.channel_layout.channel_type) {
                return std::unexpected(
                    "GraphJit point-8 sample fanout requires whole-port source/target channel layouts");
            }

            auto const source_channel_total = channel_count(source.channel_layout);
            auto const target_channel_total = channel_count(target.channel_layout);
            if (connection.source_channels.size() != source_channel_total
                || connection.target_channels.size() != target_channel_total) {
                return std::unexpected(
                    "GraphJit point-8 sample fanout requires whole-port sample connections");
            }
            for (std::size_t channel = 0; channel < source_channel_total; ++channel) {
                auto const& source_channel = connection.source_channels[channel];
                if (source_channel.bundle != source_port.node_bundle_handle
                    || source_channel.port != source_port.port_ordinal
                    || source_channel.channel != channel) {
                    return std::unexpected(
                        "GraphJit point-8 sample fanout does not yet support source-channel projection/remapping");
                }
            }
            for (std::size_t channel = 0; channel < target_channel_total; ++channel) {
                auto const& target_channel = connection.target_channels[channel];
                if (target_channel.bundle != target_port.node_bundle_handle
                    || target_channel.port != target_port.port_ordinal
                    || target_channel.channel != channel) {
                    return std::unexpected(
                        "GraphJit point-8 sample fanout does not yet support target-channel projection/remapping");
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

            auto const& source_ports = plan.primitives[*source_primitive].outputs;
            auto const& target_ports = plan.primitives[*target_primitive].inputs;
            if (source_port.port_ordinal >= source_ports.size()
                || target_port.port_ordinal >= target_ports.size()) {
                return std::unexpected(
                    "GraphJit sample-edge endpoint ordinal is outside primitive port metadata");
            }

            validated_edges.push_back(ValidatedSampleEdge{
                .group_index = group_index,
                .connection_index = connection_index,
                .source_primitive = *source_primitive,
                .source_port = source_port.port_ordinal,
                .target_primitive = *target_primitive,
                .target_port = target_port.port_ordinal,
                .source_history = connection.source_history,
                .source_latency = connection.source_latency,
                .target_history = connection.target_history,
            });
        }
    }

    auto physical = build_sample_physical_plan(
        connections, input.specialization.block_size);
    if (!physical) return std::unexpected(std::move(physical.error()));
    plan.physical = std::move(*physical);

    for (auto const& edge : validated_edges) {
        if (edge.group_index >= plan.physical.producer_groups.size()
            || !plan.physical.producer_groups[edge.group_index]) {
            return std::unexpected(
                "GraphJit sample edge lost its producer physical representation");
        }
        if (edge.connection_index >= plan.physical.connection_representations.size()
            || !plan.physical.connection_representations[edge.connection_index]) {
            return std::unexpected(
                "GraphJit sample edge lost its connection physical representation");
        }
        auto const output_representation =
            plan.physical.producer_groups[edge.group_index]
                ->canonical_representation;
        auto const input_representation =
            *plan.physical.connection_representations[edge.connection_index];
        if (output_representation == no_sample_representation
            || output_representation >= plan.physical.representations.size()
            || input_representation >= plan.physical.representations.size()) {
            return std::unexpected(
                "GraphJit sample edge references an invalid physical representation");
        }

        auto& source_binding =
            plan.primitives[edge.source_primitive].outputs[edge.source_port];
        auto& target_binding =
            plan.primitives[edge.target_primitive].inputs[edge.target_port];
        if (source_binding.representation
            && *source_binding.representation != output_representation) {
            return std::unexpected(
                "GraphJit sample output fanout resolved to conflicting canonical representations");
        }
        if (source_binding.representation
            && source_binding.history != edge.source_history) {
            return std::unexpected(
                "GraphJit sample output fanout disagrees on authored output history");
        }
        if (target_binding.representation) {
            return std::unexpected(
                "GraphJit sample input has more than one realized connection");
        }
        source_binding.representation = output_representation;
        source_binding.history = edge.source_history;
        target_binding.representation = input_representation;
        target_binding.history = edge.target_history;
        target_binding.read_latency = edge.source_latency;
    }

    for (auto const& primitive : plan.primitives) {
        if (!std::ranges::all_of(
                primitive.inputs,
                [](auto const& binding) {
                    return binding.representation.has_value();
                })
            || !std::ranges::all_of(
                primitive.outputs,
                [](auto const& binding) {
                    return binding.representation.has_value();
                })) {
            return std::unexpected(
                "GraphJit sample-edge slice requires every primitive sample port to be connected exactly once");
        }
    }

    return plan;
}

std::expected<ExecutionPlan, std::string> plan_execution(
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    DeclarationPlan const& declarations,
    PackageImportPlan const& imports,
    SamplePortBindingPlan const& sample_ports)
{
    if (analysis.empty) {
        return ExecutionPlan{.root_skippable = true};
    }
    if (declarations.primitive_storage.size() != analysis.primitives.size()) {
        return std::unexpected(
            "GraphJit lowering lost a canonical primitive storage plan");
    }
    if (imports.primitive_callbacks.size() != analysis.primitives.size()) {
        return std::unexpected(
            "GraphJit lowering lost a primitive callback import plan");
    }

    ExecutionPlan plan{
        .root_skippable = true,
    };
    plan.primitive_steps.reserve(analysis.primitives.size());

    // Consume the explicit schedule for both disconnected and feed-forward
    // slices so physical sample lifetimes and primitive execution share one
    // deterministic schedule model.
    std::vector<bool> scheduled(analysis.primitives.size(), false);
    for (auto const region_index : connections.schedule.region_order) {
        if (region_index >= connections.schedule.regions.size()) {
            return std::unexpected(
                "GraphJit connection schedule contains an invalid region index");
        }
        for (auto const bundle :
             connections.schedule.regions[region_index].execution_order) {
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
            scheduled[i] = true;
            auto const& callbacks = imports.primitive_callbacks[i];
            plan.root_skippable =
                plan.root_skippable && primitive->bundle.block_skippable;
            plan.primitive_steps.push_back(PrimitiveExecutionStep{
                .configuration_index = i,
                .storage_index = i,
                .maximum_block_size = primitive->bundle.maximum_block_size,
                .tick_callback_symbol = callbacks.tick_block,
                .skip_callback_symbol = callbacks.skip_block,
            });
        }
    }
    if (!std::ranges::all_of(scheduled, [](bool value) { return value; })) {
        return std::unexpected(
            "GraphJit connection schedule omitted a concrete primitive");
    }

    for (std::size_t carry_index = 0;
         carry_index < sample_ports.physical.carry_operations.size();
         ++carry_index) {
        auto const& carry = sample_ports.physical.carry_operations[carry_index];
        if (carry.producer_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit sample carry operation references an invalid execution position");
        }
        auto& step = plan.primitive_steps[carry.producer_execution_position];
        step.sample_carry_restores_before.push_back(carry_index);
        step.sample_carry_commits_after.push_back(carry_index);
    }

    for (std::size_t materialization_index = 0;
         materialization_index < sample_ports.physical.materializations.size();
         ++materialization_index) {
        auto const& materialization =
            sample_ports.physical.materializations[materialization_index];
        if (materialization.after_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit sample materialization references an invalid execution position");
        }
        plan.primitive_steps[materialization.after_execution_position]
            .sample_materializations_after.push_back(materialization_index);
    }
    return plan;
}
} // namespace

std::expected<LoweringPlan, std::string> build_lowering_plan(
    LoweringInput const& input)
{
    // Build the pure connection/schedule plan before realization. Topology,
    // physical sample planning, declaration, and LLVM emission therefore share
    // one immutable analysis rather than rediscovering graph facts downstream.
    auto connections = build_connection_analysis_plan(
        input.graph, input.specialization.block_size);
    if (!connections) {
        return std::unexpected(std::move(connections.error()));
    }

    auto analysis = analyze_graph(input, *connections);
    if (!analysis) return std::unexpected(std::move(analysis.error()));

    auto sample_ports = plan_sample_ports(input, *analysis, *connections);
    if (!sample_ports) return std::unexpected(std::move(sample_ports.error()));

    auto declarations = plan_declarations(input, *analysis, *sample_ports);
    if (!declarations) return std::unexpected(std::move(declarations.error()));

    auto imports = plan_package_imports(input, *analysis);
    if (!imports) return std::unexpected(std::move(imports.error()));

    auto configurations = plan_node_configurations(input, *analysis, *imports);
    if (!configurations) {
        return std::unexpected(std::move(configurations.error()));
    }

    auto execution = plan_execution(
        *analysis, *connections, *declarations, *imports, *sample_ports);
    if (!execution) return std::unexpected(std::move(execution.error()));

    return LoweringPlan{
        .connections = std::move(*connections),
        .declarations = std::move(*declarations),
        .imports = std::move(*imports),
        .configurations = std::move(*configurations),
        .sample_ports = std::move(*sample_ports),
        .execution = std::move(*execution),
    };
}
} // namespace iv::graph_jit::detail
