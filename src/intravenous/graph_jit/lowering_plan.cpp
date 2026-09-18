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
    LoweringInput const& input)
{
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
    SamplePortBindingPlan& sample_ports,
    EventPortBindingPlan& event_ports)
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

    for (auto const& primitive : event_ports.primitives) {
        for (auto const& binding : primitive.inputs) {
            if (!binding.representation) {
                return std::unexpected(
                    "GraphJit event runtime declaration has an unbound input port");
            }
        }
        for (auto const& binding : primitive.outputs) {
            if (!binding.representation) {
                return std::unexpected(
                    "GraphJit event runtime declaration has an unbound output port");
            }
        }
    }

    auto declared_sample_storage = declare_sample_physical_storage(
        layout_builder, sample_ports.physical);
    if (!declared_sample_storage) {
        return std::unexpected(std::move(declared_sample_storage.error()));
    }
    for (auto& representation : event_ports.representations) {
        representation.region = layout_builder.declare_raw_region(
            representation.size_bytes,
            representation.alignment,
            representation.persistent ? representation.migration_identity : std::string{});
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
        auto const event_physical_owns_region = std::ranges::any_of(
            event_ports.representations,
            [&](EventRepresentationPlan const& representation) {
                return representation.region.valid()
                    && representation.region.index == region_index;
            });
        if (region.kind == NodeLayout::Region::Kind::raw
            && (sample_physical_owns_region(region_index)
                || event_physical_owns_region)) {
            continue;
        }
        return std::unexpected(
            "GraphJit lowering does not yet support declaration-owned auxiliary storage regions");
    }

    auto finalized_sample_storage = finalize_sample_physical_storage(
        node_layout, sample_ports.physical);
    if (!finalized_sample_storage) {
        return std::unexpected(std::move(finalized_sample_storage.error()));
    }
    for (auto& representation : event_ports.representations) {
        if (!representation.region.valid()
            || representation.region.index >= node_layout.regions.size()) {
            return std::unexpected(
                "GraphJit event physical storage lost its raw region");
        }
        auto const& region = node_layout.regions[representation.region.index];
        if (region.kind != NodeLayout::Region::Kind::raw
            || region.size != representation.size_bytes
            || region.alignment != representation.alignment
            || region.migration_identity
                != (representation.persistent
                        ? representation.migration_identity
                        : std::string{})
            || representation.count_relative_offset > region.size
            || representation.read_index_relative_offset > region.size
            || representation.write_index_relative_offset > region.size
            || (representation.has_producer_overflow_counter
                && representation.overflow_count_relative_offset > region.size)
            || representation.events_relative_offset > region.size) {
            return std::unexpected(
                "GraphJit event physical storage disagrees with finalized NodeLayout");
        }
        representation.count_storage_offset =
            region.storage_offset + representation.count_relative_offset;
        representation.read_index_storage_offset =
            region.storage_offset + representation.read_index_relative_offset;
        representation.write_index_storage_offset =
            region.storage_offset + representation.write_index_relative_offset;
        if (representation.has_producer_overflow_counter) {
            representation.overflow_count_storage_offset =
                region.storage_offset + representation.overflow_count_relative_offset;
        }
        representation.events_storage_offset =
            region.storage_offset + representation.events_relative_offset;
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

    struct ValidatedSampleSourceBinding {
        std::size_t group_index = 0;
        std::size_t source_primitive = 0;
        std::size_t source_port = 0;
        std::size_t source_history = 0;
    };
    struct ValidatedSampleTargetBinding {
        std::size_t connection_index = 0;
        std::size_t target_primitive = 0;
        std::size_t target_port = 0;
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
        plan.primitives[i].inputs.resize(node->sample_input_count);
        plan.primitives[i].outputs.resize(node->sample_output_count);
    }

    for (auto const& group : connections.sample_producer_groups) {
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
        if (!is_realtime(target.access)) {
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
                || target_channel.port != target_port.port_ordinal
                || target_channel.channel != channel) {
                return std::unexpected(
                    "GraphJit sample composition currently requires canonical target-channel ordering");
            }
        }
        if (target_port.port_ordinal
            >= plan.primitives[*target_primitive].inputs.size()) {
            return std::unexpected(
                "GraphJit sample-edge target ordinal is outside primitive port metadata");
        }

        // Validate every actual producer port independently. A composed input
        // can reference several producer groups even though it realizes one
        // consumer-side physical representation.
        std::vector<NodeBundlePortId> source_ports_seen;
        for (std::size_t channel_index = 0;
             channel_index < connection.source_channel_timings.size();
             ++channel_index) {
            auto const& timing = connection.source_channel_timings[channel_index];
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
            if (!is_realtime(source.access)
                || source.channel_layout != timing.source_layout
                || !group.canonical_source_layout
                || *group.canonical_source_layout != source.channel_layout
                || timing.source.channel >= channel_count(source.channel_layout)) {
                return std::unexpected(
                    "GraphJit sample producer channel disagrees with its declared output layout");
            }
            if (source_port.port_ordinal
                >= plan.primitives[*source_primitive].outputs.size()) {
                return std::unexpected(
                    "GraphJit sample-edge source ordinal is outside primitive port metadata");
            }

            if (std::ranges::find(source_ports_seen, source_port)
                == source_ports_seen.end()) {
                source_ports_seen.push_back(source_port);
                validated_sources.push_back(ValidatedSampleSourceBinding{
                    .group_index = *group_index,
                    .source_primitive = *source_primitive,
                    .source_port = source_port.port_ordinal,
                    .source_history = timing.source_history,
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
                    || source_channel.port != source_port.port_ordinal
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
            target_read_latency = connection.read_latency;
        } else {
            // Projection/permutation has already normalized to one semantic
            // source per target channel. Channel-count conversion remains a
            // separate whole-port materialization feature.
            if (connection.canonical_source_layout
                || connection.source_type != connection.target_type
                || connection.source_channels.size() != target_channel_total) {
                return std::unexpected(
                    "GraphJit sample composition currently requires one source channel per target channel without channel-count conversion");
            }
            // The synthetic representation is already timestamp-aligned by
            // its per-channel gather, so the primitive reads it at latency 0.
            target_read_latency = 0;
        }

        validated_targets.push_back(ValidatedSampleTargetBinding{
            .connection_index = connection_index,
            .target_primitive = *target_primitive,
            .target_port = target_port.port_ordinal,
            .target_history = connection.target_history,
            .read_latency = target_read_latency,
        });
    }

    auto physical = build_sample_physical_plan(
        connections, input.specialization.block_size);
    if (!physical) return std::unexpected(std::move(physical.error()));
    plan.physical = std::move(*physical);

    for (auto const& source : validated_sources) {
        if (source.group_index >= plan.physical.producer_groups.size()
            || !plan.physical.producer_groups[source.group_index]) {
            return std::unexpected(
                "GraphJit sample source lost its producer physical representation");
        }
        auto const output_representation =
            plan.physical.producer_groups[source.group_index]
                ->canonical_representation;
        if (output_representation == no_sample_representation
            || output_representation >= plan.physical.representations.size()) {
            return std::unexpected(
                "GraphJit sample source references an invalid physical representation");
        }
        auto& source_binding =
            plan.primitives[source.source_primitive].outputs[source.source_port];
        if (source_binding.representation
            && *source_binding.representation != output_representation) {
            return std::unexpected(
                "GraphJit sample output fanout resolved to conflicting canonical representations");
        }
        if (source_binding.representation
            && source_binding.history != source.source_history) {
            return std::unexpected(
                "GraphJit sample output fanout disagrees on authored output history");
        }
        source_binding.representation = output_representation;
        source_binding.history = source.source_history;
    }

    for (auto const& target : validated_targets) {
        if (target.connection_index
                >= plan.physical.connection_representations.size()
            || !plan.physical.connection_representations[target.connection_index]) {
            return std::unexpected(
                "GraphJit sample target lost its connection physical representation");
        }
        auto const input_representation =
            *plan.physical.connection_representations[target.connection_index];
        if (input_representation >= plan.physical.representations.size()) {
            return std::unexpected(
                "GraphJit sample target references an invalid physical representation");
        }
        auto& target_binding =
            plan.primitives[target.target_primitive].inputs[target.target_port];
        if (target_binding.representation) {
            return std::unexpected(
                "GraphJit sample input has more than one realized connection");
        }
        target_binding.representation = input_representation;
        target_binding.history = target.target_history;
        target_binding.read_latency = target.read_latency;
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

std::expected<EventPortBindingPlan, std::string> plan_event_ports(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections)
{
    EventPortBindingPlan plan;
    if (analysis.empty) return plan;

    if (connections.boundary_bundle < input.graph.node_bundles.size()) {
        auto const& boundary = input.graph.node_bundles.bundle(
            connections.boundary_bundle);
        if (boundary.event_input_count() != 0
            || boundary.event_output_count() != 0) {
            return std::unexpected(
                "GraphJit event flow does not yet support external event boundaries");
        }
    }

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
        plan.primitives[i].inputs.resize(node->event_input_count);
        plan.primitives[i].outputs.resize(node->event_output_count);
    }

    plan.producer_group_representations.resize(
        connections.event_producer_groups.size());

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
        bool persistent_ring = false)
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
        std::size_t overflow_relative = 0;
        std::size_t alignment = std::max(
            alignof(std::size_t), alignof(TimedEvent));
        if (producer_telemetry) {
            auto const aligned_overflow = align_up(
                header_end, alignof(std::uint64_t));
            if (!aligned_overflow
                || *aligned_overflow > std::numeric_limits<std::size_t>::max()
                    - sizeof(std::uint64_t)) {
                return std::unexpected(
                    "GraphJit event producer telemetry header overflows size_t");
            }
            overflow_relative = *aligned_overflow;
            header_end = overflow_relative + sizeof(std::uint64_t);
            alignment = std::max(alignment, alignof(std::uint64_t));
        }
        auto const events_relative = align_up(header_end, alignof(TimedEvent));
        if (!events_relative
            || capacity > (std::numeric_limits<std::size_t>::max()
                    - *events_relative) / sizeof(TimedEvent)) {
            return std::unexpected(
                "GraphJit event storage size overflows size_t");
        }
        auto const representation_index = plan.representations.size();
        plan.representations.push_back(EventRepresentationPlan{
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
            .overflow_count_relative_offset = overflow_relative,
            .events_relative_offset = *events_relative,
            .size_bytes = *events_relative + capacity * sizeof(TimedEvent),
            .alignment = alignment,
        });
        return representation_index;
    };

    for (std::size_t group_index = 0;
         group_index < connections.event_producer_groups.size();
         ++group_index) {
        auto const& group = connections.event_producer_groups[group_index];
        if (!group.has_realtime_connections) {
            return std::unexpected(
                "GraphJit event flow does not yet support compiled-only event connections");
        }
        if (group.has_compiled_connections) {
            return std::unexpected(
                "GraphJit event flow does not yet support mixed realtime/compiled event fanout");
        }
        if (!group.implementation) {
            return std::unexpected(
                "GraphJit event planning lost its realtime implementation choice");
        }
        auto const implementation = *group.implementation;
        if (implementation != EventConnectionImplementationKind::direct
            && implementation != EventConnectionImplementationKind::transient_sequence
            && implementation != EventConnectionImplementationKind::compact_persistent_carry
            && implementation != EventConnectionImplementationKind::persistent_ring) {
            return std::unexpected(
                "GraphJit event flow does not yet support feedback or external event storage");
        }
        auto const transient_materialized =
            implementation == EventConnectionImplementationKind::transient_sequence;
        auto const compact_carry =
            implementation == EventConnectionImplementationKind::compact_persistent_carry;
        auto const persistent_ring =
            implementation == EventConnectionImplementationKind::persistent_ring;
        auto const retained_storage = compact_carry || persistent_ring;
        auto const aggregate_sequence =
            transient_materialized || retained_storage;
        if (group.sources.size() != 1) {
            return std::unexpected(
                "GraphJit event flow requires exactly one producer output per event group");
        }

        auto const source_id = group.sources.front();
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
        if (!is_realtime(source.access)
            || source.type != group.source_type) {
            return std::unexpected(
                "GraphJit event producer disagrees with its declaration");
        }
        if (source_id.port >= plan.primitives[*source_primitive].outputs.size()) {
            return std::unexpected(
                "GraphJit event producer ordinal is outside primitive metadata");
        }

        if (source.max_events_per_sample != group.max_events_per_sample) {
            return std::unexpected(
                "GraphJit event producer max_events_per_sample disagrees with connection analysis");
        }
        auto producer_window_samples = input.specialization.block_size;
        auto const source_history = realtime_history(source);
        auto const source_latency = realtime_latency(source);
        if (source_history > std::numeric_limits<std::size_t>::max()
                - producer_window_samples
            || source_latency > std::numeric_limits<std::size_t>::max()
                - producer_window_samples - source_history) {
            return std::unexpected(
                "GraphJit event producer temporal window overflows size_t");
        }
        producer_window_samples += source_history + source_latency;
        auto const base_max_events = event_count_for_sample_span(
            source.max_events_per_sample, producer_window_samples);
        auto const base_capacity = event_sequence_capacity_for_sample_span(
            source.max_events_per_sample, producer_window_samples);
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
            retained_history = std::max(
                retained_history,
                std::max(connection.source_history, connection.target_history));
            retained_latency = std::max(retained_latency, connection.source_latency);
        }

        std::size_t working_capacity = *base_capacity;
        std::size_t carry_capacity = 0;
        std::size_t ring_capacity = 0;
        if (compact_carry) {
            if (!group.requirements.retained_event_capacity) {
                return std::unexpected(
                    "GraphJit compact event carry lost its hard retained-event bound");
            }
            if (retained_latency > std::numeric_limits<std::size_t>::max()
                    - retained_history) {
                return std::unexpected(
                    "GraphJit compact event carry retained window overflows size_t");
            }
            auto const retained_window_samples = retained_history + retained_latency;
            auto const carry_capacity_bound = event_sequence_capacity_for_sample_span(
                source.max_events_per_sample, retained_window_samples);
            if (!carry_capacity_bound) {
                return std::unexpected(
                    "GraphJit compact event carry exceeds representable static capacity");
            }
            carry_capacity = *carry_capacity_bound;
            auto const carry_max_events = *group.requirements.retained_event_capacity;
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
            auto const ring_span_samples =
                retained_history + input.specialization.block_size + retained_latency;
            auto const ring_capacity_bound = event_sequence_capacity_for_sample_span(
                source.max_events_per_sample, ring_span_samples);
            if (!ring_capacity_bound) {
                return std::unexpected(
                    "GraphJit persistent event ring exceeds representable static capacity");
            }
            ring_capacity = *ring_capacity_bound;
        }

        auto source_representation = [&]()
            -> std::expected<std::size_t, std::string> {
            if (!persistent_ring) {
                return append_representation(
                    group_index, group.source_type, working_capacity, true);
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
                true,
                true,
                std::move(migration_identity),
                true);
        }();
        if (!source_representation) {
            return std::unexpected(std::move(source_representation.error()));
        }
        plan.producer_group_representations[group_index] = *source_representation;

        if (compact_carry) {
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
                .producer_execution_position = group.live_interval.begin,
                .retained_history_samples = retained_history,
                .retained_latency_samples = retained_latency,
            });
        } else if (persistent_ring) {
            plan.persistent_rings.push_back(EventPersistentRingPlan{
                .representation = *source_representation,
                .producer_execution_position = group.live_interval.begin,
                .retained_history_samples = retained_history,
            });
        }

        auto& source_binding =
            plan.primitives[*source_primitive].outputs[source_id.port];
        if (source_binding.representation) {
            return std::unexpected(
                "GraphJit event output belongs to more than one producer group");
        }
        source_binding = PrimitiveEventOutputBindingPlan{
            .representation = *source_representation,
            .source_type = group.source_type,
            .history = realtime_history(source),
            .latency = realtime_latency(source),
            .append_existing = aggregate_sequence,
        };

        for (auto const connection_index : group.connection_indices) {
            auto const& connection = connections.event_connections[connection_index];
            auto const retained_connection =
                connection.source_history != 0
                || connection.source_latency != 0
                || connection.target_history != 0;
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime
                || connection.external_boundary
                || connection.feedback
                || connection.sources.size() != 1
                || connection.sources.front().bundle != source_id.bundle
                || connection.sources.front().port != source_id.port
                || connection.source_type != group.source_type
                || connection.conversion.source_type != connection.source_type
                || connection.conversion.target_type != connection.target_type
                || (retained_connection && !retained_storage)) {
                return std::unexpected(
                    "GraphJit event connection requires unsupported retention, feedback, external, or source-composition semantics");
            }
            auto target_representation = *source_representation;
            if (connection.requires_conversion
                || connection.requires_block_materialization) {
                if (!transient_materialized && !retained_storage) {
                    return std::unexpected(
                        "GraphJit event implementation lost required transient materialization");
                }

                auto existing = std::ranges::find_if(
                    plan.materializations,
                    [&](EventMaterializationPlan const& candidate) {
                        return candidate.source_representation
                                == *source_representation
                            && candidate.conversion == connection.conversion
                            && candidate.target_representation
                                < plan.representations.size()
                            && plan.representations[
                                   candidate.target_representation]
                                   .type
                                == connection.target_type;
                    });
                if (existing != plan.materializations.end()) {
                    target_representation = existing->target_representation;
                    existing->history_samples = std::max(
                        existing->history_samples, connection.target_history);
                    existing->select_root_window =
                        existing->select_root_window || retained_storage;
                } else {
                    // A narrower consumer window cannot safely imply a smaller
                    // event-count capacity: max_events_per_sample is only a
                    // sizing rate, so every source event may legally cluster at
                    // one timestamp inside that narrower window. Non-expanding
                    // implicit conversion therefore inherits source capacity.
                    auto derived = append_representation(
                        group_index,
                        connection.target_type,
                        plan.representations[*source_representation].event_capacity);
                    if (!derived) {
                        return std::unexpected(std::move(derived.error()));
                    }
                    target_representation = *derived;
                    plan.materializations.push_back(EventMaterializationPlan{
                        .source_representation = *source_representation,
                        .target_representation = target_representation,
                        .conversion = connection.conversion,
                        .history_samples = connection.target_history,
                        .select_root_window = retained_storage,
                        .after_execution_position = group.live_interval.begin,
                    });
                }
            }

            for (auto const target_id : connection.targets) {
                auto const target_primitive = primitive_index_for_bundle(
                    target_id.bundle);
                if (!target_primitive) {
                    return std::unexpected(
                        "GraphJit event flow requires internal concrete consumers");
                }
                if (!aggregate_sequence
                    && analysis.primitives[*target_primitive]
                            .bundle.maximum_block_size
                        < input.specialization.block_size) {
                    return std::unexpected(
                        "GraphJit direct event flow requires unsliced consumers");
                }
                NodeBundlePortId const target_port{
                    target_id.bundle, PortKind::event, target_id.port};
                auto const target = input.graph.node_bundles
                    .resolve_event_input(target_port).config;
                if (!is_realtime(target.access)
                    || target.type != connection.target_type
                    || target_id.port
                        >= plan.primitives[*target_primitive].inputs.size()) {
                    return std::unexpected(
                        "GraphJit event consumer disagrees with its declaration");
                }
                auto& target_binding =
                    plan.primitives[*target_primitive].inputs[target_id.port];
                if (target_binding.representation) {
                    return std::unexpected(
                        "GraphJit event input has more than one realized connection");
                }
                target_binding.representation = target_representation;
            }
        }
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
                "GraphJit event flow requires every primitive event port to be connected exactly once");
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

    for (std::size_t composition_index = 0;
         composition_index < sample_ports.physical.compositions.size();
         ++composition_index) {
        auto const& composition =
            sample_ports.physical.compositions[composition_index];
        if (composition.after_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit sample composition references an invalid execution position");
        }
        plan.primitive_steps[composition.after_execution_position]
            .sample_compositions_after.push_back(composition_index);
    }

    for (std::size_t ring_index = 0;
         ring_index < event_ports.persistent_rings.size();
         ++ring_index) {
        auto const& ring = event_ports.persistent_rings[ring_index];
        if (ring.producer_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit persistent event ring references an invalid execution position");
        }
        plan.primitive_steps[ring.producer_execution_position]
            .event_persistent_ring_prunes_before.push_back(ring_index);
    }

    for (std::size_t carry_index = 0;
         carry_index < event_ports.carry_operations.size();
         ++carry_index) {
        auto const& carry = event_ports.carry_operations[carry_index];
        if (carry.producer_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit event carry operation references an invalid execution position");
        }
        auto& step = plan.primitive_steps[carry.producer_execution_position];
        step.event_carry_restores_before.push_back(carry_index);
        step.event_carry_commits_after.push_back(carry_index);
    }

    for (std::size_t materialization_index = 0;
         materialization_index < event_ports.materializations.size();
         ++materialization_index) {
        auto const& materialization =
            event_ports.materializations[materialization_index];
        if (materialization.after_execution_position >= plan.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit event materialization references an invalid execution position");
        }
        auto& step = plan.primitive_steps[materialization.after_execution_position];
        step.event_materializations_after.push_back(materialization_index);
        auto const carry_source = std::ranges::any_of(
            event_ports.carry_operations,
            [&](EventCarryPlan const& carry) {
                return carry.working_representation
                    == materialization.source_representation;
            });
        auto const persistent_ring_source =
            materialization.source_representation
                < event_ports.representations.size()
            && event_ports.representations[
                   materialization.source_representation]
                   .persistent_ring;
        if (!carry_source
            && !persistent_ring_source
            && std::ranges::find(
                   step.event_sequence_resets_before,
                   materialization.source_representation)
                == step.event_sequence_resets_before.end()) {
            step.event_sequence_resets_before.push_back(
                materialization.source_representation);
        }
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
    if (!connections->sample_detaches.empty()
        || !connections->event_detaches.empty()) {
        return std::unexpected(
            "GraphJit detach transport lowering is not yet implemented");
    }

    auto analysis = analyze_graph(input);
    if (!analysis) return std::unexpected(std::move(analysis.error()));

    auto sample_ports = plan_sample_ports(input, *analysis, *connections);
    if (!sample_ports) return std::unexpected(std::move(sample_ports.error()));

    auto event_ports = plan_event_ports(input, *analysis, *connections);
    if (!event_ports) return std::unexpected(std::move(event_ports.error()));

    auto declarations = plan_declarations(
        input, *analysis, *sample_ports, *event_ports);
    if (!declarations) return std::unexpected(std::move(declarations.error()));

    auto imports = plan_package_imports(input, *analysis);
    if (!imports) return std::unexpected(std::move(imports.error()));

    auto configurations = plan_node_configurations(input, *analysis, *imports);
    if (!configurations) {
        return std::unexpected(std::move(configurations.error()));
    }

    auto execution = plan_execution(
        *analysis,
        *connections,
        *declarations,
        *imports,
        *sample_ports,
        *event_ports);
    if (!execution) return std::unexpected(std::move(execution.error()));

    return LoweringPlan{
        .connections = std::move(*connections),
        .declarations = std::move(*declarations),
        .imports = std::move(*imports),
        .configurations = std::move(*configurations),
        .sample_ports = std::move(*sample_ports),
        .event_ports = std::move(*event_ports),
        .execution = std::move(*execution),
    };
}
} // namespace iv::graph_jit::detail
