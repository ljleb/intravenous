#include <intravenous/graph_jit/lowering_plan.h>
#include <intravenous/graph_jit/sample_physical_plan.h>
#include <intravenous/graph_jit/transient_arena_plan.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <llvm/IR/Function.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace iv::graph_jit::detail {
namespace {
void initialize_event_raw_region(
    std::span<std::byte> storage,
    std::span<std::byte const> payload)
{
    (void)payload;
    std::ranges::fill(storage, std::byte{});
}

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
            if (view.lifetime && view.lifetime->ttl_samples) {
                structural_error =
                    "GraphJit sample-edge slice does not yet support activity semantics";
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
            if (binding.channels.empty()
                || binding.channels.size() != channel_count(binding.channel_layout)) {
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
                >= event_ports.representations.size()
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
    for (auto& representation : event_ports.representations) {
        if (representation.persistent) {
            if (representation.transient_allocation
                    != no_event_transient_allocation
                || representation.migration_identity.empty()) {
                return std::unexpected(
                    "GraphJit persistent event representation has invalid ownership");
            }
            representation.region = layout_builder.declare_raw_region(
                representation.size_bytes,
                representation.alignment,
                representation.migration_identity,
                initialize_event_raw_region);
        } else if (representation.transient_allocation
                       >= event_ports.transient_allocations.size()
            || !representation.migration_identity.empty()) {
            return std::unexpected(
                "GraphJit transient event representation has invalid ownership");
        }
        if (representation.has_producer_overflow_counter) {
            representation.overflow_region = layout_builder.declare_raw_region(
                sizeof(std::uint64_t),
                alignof(std::uint64_t),
                {},
                initialize_event_raw_region);
        }
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
            if (std::any_of(
                    sample_ports.physical.persistent_allocations.begin(),
                    sample_ports.physical.persistent_allocations.end(),
                    [&](SamplePersistentAllocationPlan const& allocation) {
                        return allocation.region.valid()
                            && allocation.region.index == region_index;
                    })) {
                return true;
            }
            return false;
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
                return (representation.region.valid()
                           && representation.region.index == region_index)
                    || (representation.overflow_region.valid()
                        && representation.overflow_region.index == region_index);
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
    for (std::size_t representation_index = 0;
         representation_index < event_ports.representations.size();
         ++representation_index) {
        auto& representation = event_ports.representations[representation_index];
        if (representation.persistent) {
            if (!representation.region.valid()
                || representation.region.index >= node_layout.regions.size()) {
                return std::unexpected(
                    "GraphJit persistent event storage lost its raw region");
            }
            auto const& region = node_layout.regions[representation.region.index];
            if (region.kind != NodeLayout::Region::Kind::raw
                || region.size != representation.size_bytes
                || region.alignment != representation.alignment
                || region.migration_identity != representation.migration_identity
                || region.raw_initialize_fn != initialize_event_raw_region
                || !region.raw_initialize_payload.empty()) {
                return std::unexpected(
                    "GraphJit persistent event storage disagrees with finalized NodeLayout");
            }
            representation.storage_offset = region.storage_offset;
        } else {
            if (representation.region.valid()
                || representation.transient_allocation
                    >= event_ports.transient_allocations.size()) {
                return std::unexpected(
                    "GraphJit transient event storage lost its stack allocation");
            }
            auto const& allocation = event_ports.transient_allocations[
                representation.transient_allocation];
            if (allocation.representation_index != representation_index
                || allocation.size_bytes != representation.size_bytes
                || allocation.alignment != representation.alignment
                || allocation.region_relative_offset
                    > event_ports.transient_arena_size
                || allocation.size_bytes > event_ports.transient_arena_size
                        - allocation.region_relative_offset) {
                return std::unexpected(
                    "GraphJit transient event storage disagrees with its stack arena");
            }
        }
        if (representation.count_relative_offset > representation.size_bytes
            || representation.read_index_relative_offset > representation.size_bytes
            || representation.write_index_relative_offset > representation.size_bytes
            || representation.events_relative_offset > representation.size_bytes
            || (representation.has_source_ordinals
                && representation.source_ordinals_relative_offset
                    > representation.size_bytes)) {
            return std::unexpected(
                "GraphJit event storage contains an invalid relative field offset");
        }
        if (!representation.has_source_ordinals
            && representation.source_ordinals_relative_offset != 0) {
            return std::unexpected(
                "GraphJit ordinary event storage unexpectedly has source ordinals");
        }
        if (representation.has_producer_overflow_counter) {
            if (!representation.overflow_region.valid()
                || representation.overflow_region.index
                    >= node_layout.regions.size()) {
                return std::unexpected(
                    "GraphJit event producer telemetry lost its persistent region");
            }
            auto const& overflow =
                node_layout.regions[representation.overflow_region.index];
            if (overflow.kind != NodeLayout::Region::Kind::raw
                || overflow.size != sizeof(std::uint64_t)
                || overflow.alignment != alignof(std::uint64_t)
                || !overflow.migration_identity.empty()
                || overflow.raw_initialize_fn != initialize_event_raw_region
                || !overflow.raw_initialize_payload.empty()) {
                return std::unexpected(
                    "GraphJit event producer telemetry disagrees with finalized NodeLayout");
            }
            representation.overflow_count_storage_offset = overflow.storage_offset;
        } else if (representation.overflow_region.valid()) {
            return std::unexpected(
                "GraphJit derived event storage unexpectedly owns producer telemetry");
        }
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
        if (connection.access != PlannedConnectionAccess::realtime_to_realtime) {
            return std::unexpected(
                "GraphJit sample-edge slice supports only realtime-to-realtime sample connections");
        }
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
                    "GraphJit normalized sample composition lost canonical target-port coverage");
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
                || connection.projection_contributions.empty()) {
                return std::unexpected(
                    "GraphJit sample composition lost normalized projection metadata");
            }
            std::vector<bool> populated_targets(target_channel_total, false);
            for (auto const& contribution : connection.projection_contributions) {
                if (contribution.source_channel_indices.size()
                        != channel_count(contribution.source_type)
                    || contribution.target_channels.size()
                        != channel_count(contribution.target_type)) {
                    return std::unexpected(
                        "GraphJit sample composition contribution has inconsistent semantic channel counts");
                }
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
                for (auto const source_index : contribution.source_channel_indices) {
                    if (source_index >= connection.source_channel_timings.size()) {
                        return std::unexpected(
                            "GraphJit sample composition contribution lost a source timing");
                    }
                }
                for (auto const target_channel : contribution.target_channels) {
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

        validated_targets.push_back(ValidatedSampleTargetBinding{
            .connection_index = connection_index,
            .target_primitive = *target_primitive,
            .target_port = target_port.port_ordinal,
            .target_layout = connection.target_layout,
            .target_history = connection.target_history,
            .read_latency = target_read_latency,
        });
    }

    struct DisconnectedSampleInput {
        std::size_t primitive = 0;
        std::size_t port = 0;
        SampleInputConfig config{};
    };
    struct DisconnectedSampleOutput {
        std::size_t primitive = 0;
        std::size_t port = 0;
        SampleOutputConfig config{};
    };
    std::vector<DisconnectedSampleInput> disconnected_inputs;
    std::vector<DisconnectedSampleOutput> disconnected_outputs;
    std::vector<SampleConstantInputRequest> constant_input_requests;
    std::vector<SampleSinkPhysicalRequest> sink_requests;

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
        auto const bundle = analysis.primitives[primitive_index].bundle.node_bundle;
        auto execution_position = execution_position_for_bundle(bundle);
        if (!execution_position) {
            return std::unexpected(std::move(execution_position.error()));
        }

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
            if (!is_realtime(config.access)) {
                return std::unexpected(
                    "GraphJit disconnected compiled sample input is not supported");
            }
            disconnected_inputs.push_back(DisconnectedSampleInput{
                .primitive = primitive_index,
                .port = port,
                .config = config,
            });
            constant_input_requests.push_back(SampleConstantInputRequest{
                .channel_layout = config.channel_layout,
                .default_value = config.default_value,
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
            if (!is_realtime(config.access)) {
                return std::unexpected(
                    "GraphJit disconnected compiled sample output is not supported");
            }
            disconnected_outputs.push_back(DisconnectedSampleOutput{
                .primitive = primitive_index,
                .port = port,
                .config = config,
            });
            sink_requests.push_back(SampleSinkPhysicalRequest{
                .channel_layout = config.channel_layout,
                .history = realtime_history(config),
                .latency = realtime_latency(config),
                .execution_position = *execution_position,
                .migration_identity =
                    "graphjit.sample.disconnected_output:"
                    + std::to_string(bundle) + "." + std::to_string(port),
            });
        }
    }

    auto physical = build_sample_physical_plan(
        connections,
        input.specialization.block_size,
        sink_requests,
        constant_input_requests,
        cost_model);
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
            && (source_binding.history != source.source_history
                || source_binding.latency != source.source_latency)) {
            return std::unexpected(
                "GraphJit sample output fanout disagrees on authored output timing");
        }
        source_binding.representation = output_representation;
        source_binding.history = source.source_history;
        source_binding.latency = source.source_latency;
    }

    for (auto const& target : validated_targets) {
        if (target.connection_index
                >= plan.physical.connection_representations.size()
            || target.connection_index
                >= plan.physical.connection_channel_bindings.size()) {
            return std::unexpected(
                "GraphJit sample target lost its connection physical binding");
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

        auto const target_channel_count = channel_count(target.target_layout);
        if (plan.physical.connection_channel_bindings[target.connection_index]) {
            auto const& channel_bindings =
                *plan.physical.connection_channel_bindings[target.connection_index];
            if (channel_bindings.size() != target_channel_count) {
                return std::unexpected(
                    "GraphJit sample target channel binding count disagrees with its target layout");
            }
            target_binding.channels.reserve(channel_bindings.size());
            for (auto const& channel : channel_bindings) {
                if (channel.representation == no_sample_representation
                    || channel.representation >= plan.physical.representations.size()) {
                    return std::unexpected(
                        "GraphJit sample target channel references an invalid physical representation");
                }
                auto const& representation =
                    plan.physical.representations[channel.representation];
                if (channel.representation_channel
                        >= channel_count(representation.channel_layout)
                    || channel.frame_delay >= representation.frame_capacity) {
                    return std::unexpected(
                        "GraphJit sample target channel exceeds its physical representation");
                }
                target_binding.channels.push_back(
                    PrimitiveSampleInputChannelBindingPlan{
                        .representation = channel.representation,
                        .representation_channel = channel.representation_channel,
                        .frame_delay = channel.frame_delay,
                    });
            }
            continue;
        }

        if (!plan.physical.connection_representations[target.connection_index]) {
            return std::unexpected(
                "GraphJit sample target lost its connection physical representation");
        }
        auto const input_representation =
            *plan.physical.connection_representations[target.connection_index];
        if (input_representation >= plan.physical.representations.size()) {
            return std::unexpected(
                "GraphJit sample target references an invalid physical representation");
        }
        auto const& representation =
            plan.physical.representations[input_representation];
        if (channel_count(representation.channel_layout) != target_channel_count) {
            return std::unexpected(
                "GraphJit sample target representation channel count disagrees with its target layout");
        }
        target_binding.channels.reserve(target_channel_count);
        for (std::size_t channel = 0; channel < target_channel_count; ++channel) {
            target_binding.channels.push_back(
                PrimitiveSampleInputChannelBindingPlan{
                    .representation = input_representation,
                    .representation_channel = channel,
                    .frame_delay = 0,
                });
        }
    }

    if (plan.physical.constant_input_representations.size()
        != disconnected_inputs.size()) {
        return std::unexpected(
            "GraphJit disconnected sample input planning lost a constant buffer");
    }
    for (std::size_t i = 0; i < disconnected_inputs.size(); ++i) {
        auto const& disconnected = disconnected_inputs[i];
        auto const representation =
            plan.physical.constant_input_representations[i];
        if (representation >= plan.physical.representations.size()) {
            return std::unexpected(
                "GraphJit disconnected sample input references an invalid constant buffer");
        }
        auto const& storage = plan.physical.representations[representation];
        if (!storage.constant_value
            || storage.channel_layout != disconnected.config.channel_layout) {
            return std::unexpected(
                "GraphJit disconnected sample input lost its declared default buffer");
        }

        auto& binding = plan.primitives[disconnected.primitive]
            .inputs[disconnected.port];
        binding.channel_layout = disconnected.config.channel_layout;
        binding.history = realtime_history(disconnected.config);
        binding.read_latency = 0;
        auto const channels = channel_count(disconnected.config.channel_layout);
        binding.channels.reserve(channels);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            binding.channels.push_back(PrimitiveSampleInputChannelBindingPlan{
                .representation = representation,
                .representation_channel = channel,
                .frame_delay = 0,
            });
        }
    }

    if (plan.physical.sink_representations.size()
        != disconnected_outputs.size()) {
        return std::unexpected(
            "GraphJit disconnected sample output planning lost a writable buffer");
    }
    for (std::size_t i = 0; i < disconnected_outputs.size(); ++i) {
        auto const& disconnected = disconnected_outputs[i];
        auto const representation = plan.physical.sink_representations[i];
        if (representation >= plan.physical.representations.size()) {
            return std::unexpected(
                "GraphJit disconnected sample output references an invalid buffer");
        }
        auto& binding = plan.primitives[disconnected.primitive]
            .outputs[disconnected.port];
        binding.representation = representation;
        binding.history = realtime_history(disconnected.config);
        binding.latency = realtime_latency(disconnected.config);
    }

    for (auto const& primitive : plan.primitives) {
        if (!std::ranges::all_of(
                primitive.inputs,
                [](auto const& binding) {
                    return !binding.channels.empty()
                        && binding.channels.size()
                            == channel_count(binding.channel_layout);
                })
            || !std::ranges::all_of(
                primitive.outputs,
                [](auto const& binding) {
                    return binding.representation.has_value();
                })) {
            return std::unexpected(
                "GraphJit sample port planning left an unresolved primitive port");
        }
    }

    return plan;
}

struct EventGroupCostAlternatives {
    EventConnectionStorageRequirements separate{};
    std::optional<EventConnectionStorageRequirements> producer_home{};
};

struct EventGroupPlanningDecision {
    EventConnectionStoragePlan storage{};
    std::optional<std::size_t> producer_home_source_index{};
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
        plan.primitives[i].inputs.resize(node->event_input_count);
        plan.primitives[i].outputs.resize(node->event_output_count);
    }

    plan.producer_group_storage_plans.resize(
        connections.event_producer_groups.size());
    plan.producer_home_source_indices.resize(
        connections.event_producer_groups.size());
    plan.producer_group_representations.resize(
        connections.event_producer_groups.size());
    std::vector<EventGroupCostAlternatives> group_cost_alternatives(
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
        bool source_ordinals = false)
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
        auto const events_relative = align_up(header_end, alignof(TimedEvent));
        if (!events_relative
            || capacity > (std::numeric_limits<std::size_t>::max()
                    - *events_relative) / sizeof(TimedEvent)) {
            return std::unexpected(
                "GraphJit event storage size overflows size_t");
        }
        auto const events_end = *events_relative + capacity * sizeof(TimedEvent);
        std::size_t source_ordinals_relative = 0;
        auto size_bytes = events_end;
        if (source_ordinals) {
            auto const ordinals_relative = align_up(
                events_end, alignof(std::size_t));
            if (!ordinals_relative
                || capacity > (std::numeric_limits<std::size_t>::max()
                        - *ordinals_relative) / sizeof(std::size_t)) {
                return std::unexpected(
                    "GraphJit event source-ordinal storage size overflows size_t");
            }
            source_ordinals_relative = *ordinals_relative;
            size_bytes = source_ordinals_relative
                + capacity * sizeof(std::size_t);
            alignment = std::max(alignment, alignof(std::size_t));
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
            .events_relative_offset = *events_relative,
            .has_source_ordinals = source_ordinals,
            .source_ordinals_relative_offset = source_ordinals_relative,
            .size_bytes = size_bytes,
            .alignment = alignment,
        });
        return representation_index;
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
        for (auto const source : group.sources) {
            identity += ':' + std::to_string(source.bundle) + '.'
                + std::to_string(source.port);
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
        if (!group.storage_plan) {
            return std::unexpected(
                "GraphJit event planning has no internal realtime storage plan");
        }
        auto selected_group_plan = decisions
            ? decisions->producer_groups[group_index].storage
            : *group.storage_plan;
        auto storage_kind = selected_group_plan.kind;
        auto producer_home_source_index = decisions
            ? decisions->producer_groups[group_index]
                .producer_home_source_index
            : std::optional<std::size_t>{};
        plan.producer_group_storage_plans[group_index] = selected_group_plan;
        plan.producer_home_source_indices[group_index] =
            producer_home_source_index;
        auto compact_carry =
            storage_kind
            == RealtimeBufferStorageKind::stack_with_persistent_carry;
        auto persistent_ring =
            storage_kind == RealtimeBufferStorageKind::full_node_storage;
        auto retained_storage = compact_carry || persistent_ring;
        auto aggregate_sequence =
            group.requires_invocation_aggregate || retained_storage;

        if (group.sources.size() > 1) {
            std::optional<std::size_t> cyclic_source_region;
            bool has_acyclic_source = false;
            bool incompatible_cyclic_sources = false;
            for (auto const source : group.sources) {
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
            // producers still cannot append concurrently into one sequence
            // without disturbing global order. Lowering costs both concrete
            // choices below: either every producer writes a local sequence, or
            // semantic source 0 writes directly into the aggregate and the
            // remaining source sequences are merged afterward.
            // Retained producer-home restores/prunes the canonical prefix before
            // source 0 executes and merges the remaining streams afterward.
            struct ValidatedSource {
                EventOutputPortId id{};
                std::size_t primitive = 0;
                std::size_t execution_position = 0;
                EventOutputConfig config{};
                std::size_t capacity = 0;
                std::size_t aggregate_event_bound = 0;
                std::optional<std::size_t> cyclic_region{};
            };
            std::vector<ValidatedSource> validated_sources;
            validated_sources.reserve(group.sources.size());
            std::size_t total_local_capacity = 0;
            std::size_t total_invocation_local_capacity = 0;
            std::size_t producer_execution_position = 0;
            double observed_rate = 0.0;

            for (auto const source_id : group.sources) {
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
                if (!is_realtime(source.access)
                    || source.type != group.source_type) {
                    return std::unexpected(
                        "GraphJit event fan-in producer disagrees with its declaration");
                }
                auto const source_history = realtime_history(source);
                auto const source_latency = realtime_latency(source);
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
                if (*capacity
                    > std::numeric_limits<std::size_t>::max()
                        - total_invocation_local_capacity) {
                    return std::unexpected(
                        "GraphJit event fan-in local capacity overflows size_t");
                }
                total_invocation_local_capacity += *capacity;
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
                    .primitive = *source_primitive,
                    .execution_position = execution_position,
                    .config = source,
                    .capacity = *capacity,
                    .aggregate_event_bound = *aggregate_event_bound,
                    .cyclic_region = source_cyclic_region,
                });
            }
            if (observed_rate != group.max_events_per_index) {
                return std::unexpected(
                    "GraphJit event fan-in aggregate max_events_per_index disagrees with connection analysis");
            }

            std::size_t retained_history = 0;
            std::size_t retained_latency = 0;
            for (auto const connection_index : group.connection_indices) {
                if (connection_index >= connections.event_connections.size()) {
                    return std::unexpected(
                        "GraphJit event producer group references an invalid connection");
                }
                auto const& connection =
                    connections.event_connections[connection_index];
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
                group_cost_alternatives[group_index] = EventGroupCostAlternatives{
                    .separate = staged_requirements,
                };

                auto selected_plan = decisions
                    ? decisions->producer_groups[group_index].storage
                    : choose_event_connection_storage_plan(
                        staged_requirements, cost_model);
                if (decisions
                    && decisions->producer_groups[group_index]
                        .producer_home_source_index) {
                    return std::unexpected(
                        "GraphJit staged event fan-in cannot use producer-home storage");
                }
                storage_kind = selected_plan.kind;
                plan.producer_group_storage_plans[group_index] = selected_plan;
                plan.producer_home_source_indices[group_index].reset();
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
                        .representation = *canonical,
                        .prune_scope = first_scope,
                        .retained_history_samples = retained_history,
                    });
                } else {
                    plan.sequence_resets.push_back(EventSequenceResetPlan{
                        .representation = *canonical,
                        .scope = first_scope,
                    });
                }

                for (auto const& stage : stages) {
                    EventMergePlan merge{
                        .target_representation = *canonical,
                        .scope = primitive_scope(
                            stage.execution_position,
                            EventOperationPhase::after),
                        .target_is_semantic_source = false,
                        .preserve_existing_target = true,
                    };
                    merge.source_representations.reserve(
                        stage.source_indices.size());
                    merge.source_ordinals.reserve(stage.source_indices.size());
                    for (auto const source_index : stage.source_indices) {
                        auto const& source = validated_sources[source_index];
                        auto const local_capacity = source.cyclic_region
                            ? source.capacity
                            : source.aggregate_event_bound;
                        auto local = append_representation(
                            group_index,
                            group.source_type,
                            local_capacity,
                            true);
                        if (!local) {
                            return std::unexpected(std::move(local.error()));
                        }
                        merge.source_representations.push_back(*local);
                        merge.source_ordinals.push_back(source_index);

                        auto& source_binding = plan.primitives[
                            source.primitive].outputs[source.id.port];
                        if (source_binding.representation) {
                            return std::unexpected(
                                "GraphJit event output belongs to more than one producer group");
                        }
                        source_binding = PrimitiveEventOutputBindingPlan{
                            .representation = *local,
                            .source_type = group.source_type,
                            .history = realtime_history(source.config),
                            .latency = realtime_latency(source.config),
                            .append_existing = !source.cyclic_region
                                && analysis.primitives[source.primitive]
                                       .bundle.maximum_block_size
                                    < input.specialization.block_size,
                        };
                    }
                    plan.merges.push_back(std::move(merge));
                }

                for (auto const connection_index : group.connection_indices) {
                    auto const& connection =
                        connections.event_connections[connection_index];
                    if (connection.detach) continue;
                    auto const retained_connection =
                        connection.source_history != 0
                        || connection.source_latency != 0
                        || connection.target_history != 0;
                    if (connection.access
                            != PlannedConnectionAccess::realtime_to_realtime
                        || connection.external_boundary
                        || connection.sources != group.sources
                        || connection.source_type != group.source_type
                        || connection.conversion.source_type
                            != connection.source_type
                        || connection.conversion.target_type
                            != connection.target_type
                        || (retained_connection && !retained_storage)) {
                        return std::unexpected(
                            "GraphJit staged event fan-in connection requires unsupported retention, feedback, external, or source semantics");
                    }

                    for (auto const target_id : connection.targets) {
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
                        if (!is_realtime(target.access)
                            || target.type != connection.target_type) {
                            return std::unexpected(
                                "GraphJit staged event fan-in consumer disagrees with its declaration");
                        }

                        auto target_representation = *canonical;
                        if (connection.requires_conversion) {
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
                                        && candidate.conversion
                                            == connection.conversion
                                        && candidate.scope == scope
                                        && candidate.target_representation
                                            < plan.representations.size()
                                        && plan.representations[
                                               candidate.target_representation]
                                               .type
                                            == connection.target_type;
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
                                    plan.representations[*canonical]
                                        .event_capacity);
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
                        if (target_binding.representation) {
                            return std::unexpected(
                                "GraphJit event input has more than one realized connection");
                        }
                        target_binding.representation = target_representation;
                    }
                }
                continue;
            }

            // Build and cost the fan-in alternatives here, next to the exact
            // producer-local buffers and merge operation that lowering emits.
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

            auto sequential_merge_writes = [&](std::size_t initial_values,
                                                std::size_t first_source) {
                auto target_values = initial_values;
                std::size_t writes = 0;
                if (!cyclic_source_region) {
                    for (std::size_t source_index = first_source;
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
                    for (std::size_t source_index = first_source;
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

            auto sequential_ring_accesses = [&](std::size_t initial_values,
                                                 std::size_t first_source) {
                auto target_values = initial_values;
                std::size_t accesses = 0;
                auto account_source = [&](std::size_t produced) {
                    auto const previous_target = target_values;
                    target_values = saturating_add_values(
                        target_values, produced);
                    // event_sequence_merge() reads the existing target sequence
                    // and writes the complete merged target sequence. Source
                    // reads remain in producer-local stack buffers.
                    accesses = saturating_add_values(
                        accesses,
                        saturating_add_values(previous_target, target_values));
                };

                if (!cyclic_source_region) {
                    for (std::size_t source_index = first_source;
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
                    for (std::size_t source_index = first_source;
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

            auto const separate_transient_writes =
                sequential_merge_writes(0, 0);
            auto const separate_retained_writes =
                sequential_merge_writes(retained_values, 0);
            auto const separate_ring_accesses =
                sequential_ring_accesses(retained_values, 0);
            auto separate_requirements = group.storage_requirements;
            separate_requirements.operations = RealtimeStorageOperationCounts{
                .transient_extra_copied_values = separate_transient_writes,
                .carry_extra_copied_values = separate_retained_writes,
                .full_extra_copied_values = separate_retained_writes,
                .full_ring_addressed_values = separate_ring_accesses,
                .transient_extra_stack_values =
                    total_invocation_local_capacity,
                .carry_extra_stack_values = total_invocation_local_capacity,
                .full_extra_stack_values = total_invocation_local_capacity,
            };
            auto const separate_plan = choose_event_connection_storage_plan(
                separate_requirements, cost_model);

            bool retained_home_is_order_safe = true;
            if (retained_values != 0) {
                for (std::size_t source_index = 0;
                     source_index < validated_sources.size(); ++source_index) {
                    auto const& source = validated_sources[source_index].config;
                    if (realtime_latency(source) != 0
                        || (source_index == 0
                            && realtime_history(source) != 0)) {
                        retained_home_is_order_safe = false;
                        break;
                    }
                }
            }
            auto const home_legal = !cyclic_source_region
                && retained_home_is_order_safe;

            std::optional<EventConnectionStoragePlan> home_plan;
            EventConnectionStorageRequirements home_requirements{};
            if (home_legal) {
                auto const source_zero_values =
                    validated_sources.front().aggregate_event_bound;
                auto const home_transient_writes = total_local_capacity;
                auto const home_prefix = saturating_add_values(
                    retained_values, source_zero_values);
                auto const home_retained_writes =
                    sequential_merge_writes(home_prefix, 1);
                auto const home_ring_accesses = saturating_add_values(
                    source_zero_values,
                    sequential_ring_accesses(home_prefix, 1));
                auto const home_local_stack = total_invocation_local_capacity
                    - validated_sources.front().capacity;

                home_requirements = group.storage_requirements;
                home_requirements.operations = RealtimeStorageOperationCounts{
                    .transient_extra_copied_values = home_transient_writes,
                    .carry_extra_copied_values = home_retained_writes,
                    .full_extra_copied_values = home_retained_writes,
                    .full_ring_addressed_values = home_ring_accesses,
                    .transient_extra_stack_values = home_local_stack,
                    .carry_extra_stack_values = home_local_stack,
                    .full_extra_stack_values = home_local_stack,
                };
                home_plan = choose_event_connection_storage_plan(
                    home_requirements, cost_model);
            }
            group_cost_alternatives[group_index] = EventGroupCostAlternatives{
                .separate = separate_requirements,
                .producer_home = home_legal
                    ? std::optional<EventConnectionStorageRequirements>{
                        home_requirements}
                    : std::nullopt,
            };

            auto selected_cost = [](EventConnectionStoragePlan const& candidate)
                -> std::optional<std::size_t> {
                auto const& cost = candidate.candidate_costs.for_kind(
                    candidate.kind);
                if (!cost.legal) return std::nullopt;
                return cost.weighted_cost;
            };
            auto const separate_cost = selected_cost(separate_plan);
            auto const home_cost = home_plan
                ? selected_cost(*home_plan)
                : std::optional<std::size_t>{};

            EventConnectionStoragePlan selected_plan{};
            if (decisions) {
                selected_plan = decisions->producer_groups[group_index].storage;
                producer_home_source_index = decisions->producer_groups[
                    group_index].producer_home_source_index;
                if (producer_home_source_index
                    && (!home_legal || *producer_home_source_index != 0)) {
                    return std::unexpected(
                        "GraphJit event costing selected an invalid producer-home fan-in");
                }
            } else if (home_cost
                       && (!separate_cost || *home_cost < *separate_cost)) {
                producer_home_source_index = 0;
                selected_plan = *home_plan;
            } else if (separate_cost) {
                producer_home_source_index.reset();
                selected_plan = separate_plan;
            } else if (home_cost) {
                producer_home_source_index = 0;
                selected_plan = *home_plan;
            } else {
                return std::unexpected(
                    "GraphJit event fan-in has no storage realization within the compile-time stack budget");
            }

            storage_kind = selected_plan.kind;
            plan.producer_group_storage_plans[group_index] = selected_plan;
            plan.producer_home_source_indices[group_index] =
                producer_home_source_index;
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

            if (producer_home_source_index
                && *producer_home_source_index != 0) {
                return std::unexpected(
                    "GraphJit event fan-in producer-home source must be semantic source 0");
            }
            auto const producer_home =
                producer_home_source_index.has_value()
                && !cyclic_source_region;
            auto const transient_producer_home =
                producer_home && !retained_storage;
            auto const retained_producer_home =
                producer_home && retained_storage;
            auto const identity_base = event_group_identity(group);
            auto canonical = append_representation(
                group_index,
                group.source_type,
                canonical_capacity,
                producer_home,
                persistent_ring,
                persistent_ring
                    ? identity_base + ":kind=persistent_ring:history="
                        + std::to_string(retained_history) + ":latency="
                        + std::to_string(retained_latency) + ":capacity="
                        + std::to_string(canonical_capacity)
                    : std::string{},
                persistent_ring);
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
                        + std::to_string(carry_capacity));
                if (!persistent) {
                    return std::unexpected(std::move(persistent.error()));
                }
                auto const restore_execution_position =
                    retained_producer_home
                    ? validated_sources.front().execution_position
                    : producer_execution_position;
                auto const restore_scope = cyclic_source_region
                    ? region_scope(
                        *cyclic_source_region, EventOperationPhase::before)
                    : primitive_scope(
                        restore_execution_position,
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
                    .representation = *canonical,
                    .prune_scope = cyclic_source_region
                        ? region_scope(
                            *cyclic_source_region,
                            EventOperationPhase::before)
                        : primitive_scope(
                            retained_producer_home
                                ? validated_sources.front().execution_position
                                : producer_execution_position,
                            EventOperationPhase::before),
                    .retained_history_samples = retained_history,
                });
            }

            EventMergePlan merge{
                .target_representation = *canonical,
                .scope = primitive_scope(
                    producer_execution_position, EventOperationPhase::after),
                .target_is_semantic_source = transient_producer_home,
                .preserve_existing_target =
                    retained_storage || cyclic_source_region.has_value(),
            };
            merge.source_representations.reserve(
                validated_sources.size() - (producer_home ? 1u : 0u));
            for (std::size_t source_index = 0;
                 source_index < validated_sources.size(); ++source_index) {
                auto const& source = validated_sources[source_index];
                std::size_t representation = *canonical;
                if (!producer_home || source_index != 0) {
                    auto local = append_representation(
                        group_index,
                        group.source_type,
                        source.capacity,
                        true);
                    if (!local) {
                        return std::unexpected(std::move(local.error()));
                    }
                    representation = *local;
                    merge.source_representations.push_back(representation);
                }

                auto& source_binding =
                    plan.primitives[source.primitive].outputs[source.id.port];
                if (source_binding.representation) {
                    return std::unexpected(
                        "GraphJit event output belongs to more than one producer group");
                }
                source_binding = PrimitiveEventOutputBindingPlan{
                    .representation = representation,
                    .source_type = group.source_type,
                    .history = realtime_history(source.config),
                    .latency = realtime_latency(source.config),
                    .append_existing = !cyclic_source_region
                        && ((retained_producer_home && source_index == 0)
                        || analysis.primitives[source.primitive]
                                .bundle.maximum_block_size
                            < input.specialization.block_size),
                };
            }
            if (cyclic_source_region && !retained_storage) {
                plan.sequence_resets.push_back(EventSequenceResetPlan{
                    .representation = *canonical,
                    .scope = region_scope(
                        *cyclic_source_region, EventOperationPhase::before),
                });
            }
            plan.merges.push_back(std::move(merge));

            for (auto const connection_index : group.connection_indices) {
                auto const& connection =
                    connections.event_connections[connection_index];
                if (connection.detach) continue;
                auto const retained_connection =
                    connection.source_history != 0
                    || connection.source_latency != 0
                    || connection.target_history != 0;
                if (connection.access
                        != PlannedConnectionAccess::realtime_to_realtime
                    || connection.external_boundary
                    || connection.sources != group.sources
                    || connection.source_type != group.source_type
                    || connection.conversion.source_type
                        != connection.source_type
                    || connection.conversion.target_type
                        != connection.target_type
                    || (retained_connection && !retained_storage)) {
                    return std::unexpected(
                        "GraphJit event fan-in connection requires unsupported retention, feedback, external, or source semantics");
                }

                for (auto const target_id : connection.targets) {
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
                    if (!is_realtime(target.access)
                        || target.type != connection.target_type) {
                        return std::unexpected(
                            "GraphJit event fan-in consumer disagrees with its declaration");
                    }
                    auto target_representation = *canonical;
                    if (connection.requires_conversion) {
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
                                    && candidate.conversion == connection.conversion
                                    && candidate.scope == *scope
                                    && candidate.target_representation
                                        < plan.representations.size()
                                    && plan.representations[
                                           candidate.target_representation].type
                                        == connection.target_type;
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
                                plan.representations[*canonical].event_capacity);
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
                    if (target_binding.representation) {
                        return std::unexpected(
                            "GraphJit event input has more than one realized connection");
                    }
                    target_binding.representation = target_representation;
                }
            }
            continue;
        }
        if (group.sources.size() != 1) {
            return std::unexpected(
                "GraphJit event producer group has no semantic sources");
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
        auto const source_history = realtime_history(source);
        auto const source_latency = realtime_latency(source);
        auto const requires_cyclic_local_merge = source_region_is_cyclic
            && (source_history != 0 || source_latency != 0);
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
            retained_history = std::max(
                retained_history,
                std::max(connection.source_history, connection.target_history));
            retained_latency = std::max(retained_latency, connection.source_latency);
        }
        group_cost_alternatives[group_index].separate =
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

        auto source_representation = [&]()
            -> std::expected<std::size_t, std::string> {
            if (!persistent_ring) {
                return append_representation(
                    group_index,
                    group.source_type,
                    working_capacity,
                    !requires_cyclic_local_merge);
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
                !requires_cyclic_local_merge,
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
                .restore_scope = source_before_scope,
                .commit_scope = source_after_scope,
                .retained_history_samples = retained_history,
                .retained_latency_samples = retained_latency,
            });
        } else if (persistent_ring) {
            plan.persistent_rings.push_back(EventPersistentRingPlan{
                .representation = *source_representation,
                .prune_scope = source_before_scope,
                .retained_history_samples = retained_history,
            });
        }

        auto& source_binding =
            plan.primitives[*source_primitive].outputs[source_id.port];
        if (source_binding.representation) {
            return std::unexpected(
                "GraphJit event output belongs to more than one producer group");
        }
        auto producer_representation = *source_representation;
        if (requires_cyclic_local_merge) {
            auto local = append_representation(
                group_index, group.source_type, *base_capacity, true);
            if (!local) {
                return std::unexpected(std::move(local.error()));
            }
            producer_representation = *local;
            plan.merges.push_back(EventMergePlan{
                .source_representations = {producer_representation},
                .target_representation = *source_representation,
                .scope = primitive_scope(
                    group.live_interval.begin, EventOperationPhase::after),
                .target_is_semantic_source = false,
                .preserve_existing_target = true,
            });
            if (!retained_storage) {
                plan.sequence_resets.push_back(EventSequenceResetPlan{
                    .representation = *source_representation,
                    .scope = source_before_scope,
                });
            }
        }
        source_binding = PrimitiveEventOutputBindingPlan{
            .representation = producer_representation,
            .source_type = group.source_type,
            .history = realtime_history(source),
            .latency = realtime_latency(source),
            .append_existing = aggregate_sequence
                && !requires_cyclic_local_merge,
        };

        for (auto const connection_index : group.connection_indices) {
            auto const& connection = connections.event_connections[connection_index];
            if (connection.detach) continue;
            auto const retained_connection =
                connection.source_history != 0
                || connection.source_latency != 0
                || connection.target_history != 0;
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime
                || connection.external_boundary
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
            for (auto const target_id : connection.targets) {
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
                if (!is_realtime(target.access)
                    || target.type != connection.target_type
                    || target_id.port
                        >= plan.primitives[*target_primitive].inputs.size()) {
                    return std::unexpected(
                        "GraphJit event consumer disagrees with its declaration");
                }
                auto target_representation = *source_representation;
                if (connection.requires_conversion) {
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
                                && candidate.conversion == connection.conversion
                                && candidate.scope == *scope
                                && candidate.target_representation
                                    < plan.representations.size()
                                && plan.representations[
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
                            plan.representations[*source_representation]
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
                if (target_binding.representation) {
                    return std::unexpected(
                        "GraphJit event input has more than one realized connection");
                }
                target_binding.representation = target_representation;
            }
        }
    }

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
        if (connection.access != PlannedConnectionAccess::realtime_to_realtime
            || connection.external_boundary) {
            return std::unexpected(
                "GraphJit event feedback requires internal realtime transport");
        }

        auto const source_group_it = std::ranges::find_if(
            connections.event_producer_groups,
            [&](EventProducerGroupPlan const& group) {
                return group.source_type == connection.source_type
                    && group.sources == connection.sources;
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
                "GraphJit event detach source has no realized event representation");
        }
        auto const canonical_source_representation =
            *plan.producer_group_representations[source_group_index];
        if (canonical_source_representation >= plan.representations.size()) {
            return std::unexpected(
                "GraphJit event detach source representation is invalid");
        }
        // Detached feedback must observe the producer group's canonical
        // root-call stream, not a producer-local per-slice buffer. Any local
        // source-history/latency buffer is merged into this stream immediately
        // before feedback_append runs. The feedback cursor then selects only
        // the newly appended suffix, so retained events already present in a
        // carry buffer or persistent ring are not copied into feedback again.
        auto const source_representation = canonical_source_representation;
        auto const& source_storage = plan.representations[source_representation];
        if (source_storage.type != connection.source_type) {
            return std::unexpected(
                "GraphJit event detach source representation disagrees with detach type");
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
                // counts below, not in the current-footprint term. Physical
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
        // representation and one producer-side append operation across all such
        // consumers, regardless of whether that representation is transient,
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
                .retained_event_count = *retained_event_count,
                .retained_window_samples = retained_window_samples,
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
            if (!is_realtime(target.access)
                || target.type != connection.target_type) {
                return std::unexpected(
                    "GraphJit event feedback consumer disagrees with its declaration");
            }
            auto consumer_representation = target_representation;
            if (connection.requires_conversion) {
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
                            && candidate.conversion == connection.conversion
                            && candidate.scope == scope
                            && candidate.target_representation
                                < plan.representations.size()
                            && plan.representations[
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
                        plan.representations[target_representation]
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
            if (target_binding.representation) {
                return std::unexpected(
                    "GraphJit event feedback consumer input is connected more than once");
            }
            target_binding.representation = consumer_representation;
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
            if (feedback.source_representation >= plan.representations.size()) {
                return std::unexpected(
                    "GraphJit event costing found an invalid feedback source");
            }
            auto const source_group = plan.representations[
                feedback.source_representation].producer_group_index;
            if (source_group >= group_operations.size()) {
                return std::unexpected(
                    "GraphJit event costing lost a feedback producer group");
            }
            group_operations[source_group].full_ring_addressed_values =
                iv::detail::saturating_add(
                    group_operations[source_group].full_ring_addressed_values,
                    feedback.authored_event_count);
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
                >= plan.representations.size()) {
                return std::unexpected(
                    "GraphJit event costing found an invalid materialization source");
            }
            auto const source_group = plan.representations[
                materialization.source_representation].producer_group_index;
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
        selected.producer_groups.reserve(group_cost_alternatives.size());
        for (std::size_t group_index = 0;
             group_index < group_cost_alternatives.size(); ++group_index) {
            auto const& alternatives = group_cost_alternatives[group_index];
            auto separate_requirements = add_operations(
                alternatives.separate, group_operations[group_index]);
            auto const separate = choose_event_connection_storage_plan(
                separate_requirements, cost_model);
            auto const separate_cost = selected_cost(separate);

            std::optional<EventConnectionStoragePlan> home;
            std::optional<std::size_t> home_cost;
            if (alternatives.producer_home) {
                auto home_requirements = add_operations(
                    *alternatives.producer_home,
                    group_operations[group_index]);
                home = choose_event_connection_storage_plan(
                    home_requirements, cost_model);
                home_cost = selected_cost(*home);
            }

            if (home_cost && (!separate_cost || *home_cost < *separate_cost)) {
                selected.producer_groups.push_back(EventGroupPlanningDecision{
                    .storage = *home,
                    .producer_home_source_index = 0,
                });
            } else if (separate_cost) {
                selected.producer_groups.push_back(EventGroupPlanningDecision{
                    .storage = separate,
                });
            } else if (home_cost) {
                selected.producer_groups.push_back(EventGroupPlanningDecision{
                    .storage = *home,
                    .producer_home_source_index = 0,
                });
            } else {
                return std::unexpected(
                    "GraphJit event operations have no storage realization within the compile-time stack budget");
            }
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
            input, analysis, connections, cost_model, &selected);
    }
    if (plan.feedback_operations.size() != decisions->feedback.size()) {
        return std::unexpected(
            "GraphJit event costing decisions disagree with feedback operations");
    }

    // Complete disconnected realtime ports explicitly. Inputs bind an empty
    // bounded sequence; outputs bind a producer-sized sink so the callback keeps
    // its normal ABI and overflow telemetry without introducing a fake graph
    // edge or any audio-thread allocation.
    for (std::size_t primitive_index = 0;
        primitive_index < plan.primitives.size(); ++primitive_index) {
        auto const bundle = analysis.primitives[primitive_index].bundle.node_bundle;
        if (bundle >= connections.schedule.bundle_execution_position.size()
            || !connections.schedule.bundle_execution_position[bundle]) {
            return std::unexpected(
                "GraphJit disconnected event port has no schedule position");
        }
        auto const position =
            *connections.schedule.bundle_execution_position[bundle];

        for (std::size_t port = 0;
             port < plan.primitives[primitive_index].inputs.size(); ++port) {
            auto& binding = plan.primitives[primitive_index].inputs[port];
            if (binding.representation) continue;
            NodeBundlePortId const port_id{bundle, PortKind::event, port};
            auto const config = input.graph.node_bundles
                .resolve_event_input(port_id).config;
            if (!is_realtime(config.access)) {
                return std::unexpected(
                    "GraphJit disconnected compiled event inputs are not yet supported");
            }
            auto empty = append_representation(
                connections.event_producer_groups.size(), config.type, 0);
            if (!empty) return std::unexpected(std::move(empty.error()));
            binding.representation = *empty;
            plan.sequence_resets.push_back(EventSequenceResetPlan{
                .representation = *empty,
                .scope = primitive_scope(
                    position, EventOperationPhase::before),
            });
        }

        for (std::size_t port = 0;
             port < plan.primitives[primitive_index].outputs.size(); ++port) {
            auto& binding = plan.primitives[primitive_index].outputs[port];
            if (binding.representation) continue;
            NodeBundlePortId const port_id{bundle, PortKind::event, port};
            auto const config = input.graph.node_bundles
                .resolve_event_output(port_id).config;
            if (!is_realtime(config.access)) {
                return std::unexpected(
                    "GraphJit disconnected compiled event outputs are not yet supported");
            }
            auto window_samples = input.specialization.block_size;
            auto const history = realtime_history(config);
            auto const latency = realtime_latency(config);
            if (history > std::numeric_limits<std::size_t>::max()
                    - window_samples
                || latency > std::numeric_limits<std::size_t>::max()
                    - window_samples - history) {
                return std::unexpected(
                    "GraphJit disconnected event output window overflows size_t");
            }
            window_samples += history + latency;
            auto capacity = event_sequence_capacity_for_sample_span(
                config.max_events_per_index, window_samples);
            if (!capacity) {
                return std::unexpected(
                    "GraphJit disconnected event output capacity is not representable");
            }
            auto sink = append_representation(
                connections.event_producer_groups.size(),
                config.type,
                *capacity,
                true);
            if (!sink) return std::unexpected(std::move(sink.error()));
            binding = PrimitiveEventOutputBindingPlan{
                .representation = *sink,
                .source_type = config.type,
                .history = history,
                .latency = latency,
                .append_existing = false,
            };
        }
    }

    // Derive stack-buffer lifetimes from the operations that actually access
    // each event sequence. Producer-group intervals are intentionally not used
    // here: a conversion result, merge input, or feedback working buffer often
    // exists for only a small suffix of the producer group's full interval.
    std::vector<std::optional<ConnectionLiveIntervalPlan>> stack_buffer_lifetimes(
        plan.representations.size());

    std::vector<std::size_t> primitive_execution_positions(
        analysis.primitives.size());
    for (std::size_t primitive_index = 0;
         primitive_index < analysis.primitives.size(); ++primitive_index) {
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
        if (representation_index >= plan.representations.size()) {
            return std::unexpected(
                "GraphJit event stack lifetime references an invalid buffer");
        }
        if (begin > end) {
            return std::unexpected(
                "GraphJit event stack lifetime has an inverted schedule interval");
        }
        if (plan.representations[representation_index].persistent) return {};
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
            if (!output.representation) continue;
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
                    *output.representation, begin, end);
                !touched) {
                return std::unexpected(std::move(touched.error()));
            }
        }
        for (auto const& input_binding : plan.primitives[primitive_index].inputs) {
            if (!input_binding.representation) continue;
            if (auto touched = touch_at(*input_binding.representation, position);
                !touched) {
                return std::unexpected(std::move(touched.error()));
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
        if (auto touched = touch_at(
                merge.target_representation, *operation_position);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
    }

    // Scope is part of the physical operation. Lifetime planning therefore
    // consumes the selected slice/root-call boundary directly and never has to
    // reverse-engineer it from representation consumers.
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
    transient_requests.reserve(plan.representations.size());
    transient_representations.reserve(plan.representations.size());
    for (std::size_t representation_index = 0;
         representation_index < plan.representations.size();
         ++representation_index) {
        auto const& representation = plan.representations[representation_index];
        if (representation.persistent) continue;
        if (!stack_buffer_lifetimes[representation_index]) {
            return std::unexpected(
                "GraphJit transient event buffer has no scheduled reader or writer");
        }
        transient_requests.push_back(TransientArenaAllocationRequest{
            .size_bytes = representation.size_bytes,
            .alignment = representation.alignment,
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
        plan.representations[representation_index].transient_allocation =
            allocation_index;
    }

    return plan;
}

std::expected<EventPortBindingPlan, std::string> plan_event_ports(
    LoweringInput const& input,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    RealtimeStorageCostModel const& cost_model)
{
    return plan_event_ports_once(
        input, analysis, connections, cost_model, nullptr);
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

    // Keep one flattened primitive-step namespace because all physical plans
    // refer to producer execution positions in that namespace. Execution regions
    // add the information LLVM realization needs to switch cyclic SCCs from the
    // ordinary primitive-major traversal to slice-major traversal. Detach is
    // connection metadata, so there are no synthetic detach primitives or
    // schedule vertices to filter from this namespace.
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
            scheduled[i] = true;
            auto const& callbacks = imports.primitive_callbacks[i];
            auto const step_index = plan.primitive_steps.size();
            plan.primitive_steps.push_back(PrimitiveExecutionStep{
                .configuration_index = i,
                .storage_index = i,
                .maximum_block_size = primitive->bundle.maximum_block_size,
                .tick_callback_symbol = callbacks.tick_block,
                .skip_callback_symbol = callbacks.skip_block,
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
    if (!std::ranges::all_of(scheduled, [](bool value) { return value; })) {
        return std::unexpected(
            "GraphJit connection schedule omitted a concrete primitive");
    }

    // Map flattened execution positions back to their region before assigning
    // physical event operations. Operations whose semantic lifetime is one
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
         carry_index < sample_ports.physical.carry_operations.size();
         ++carry_index) {
        auto const& carry = sample_ports.physical.carry_operations[carry_index];
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
         timeline_index < sample_ports.physical.feedback_timelines.size();
         ++timeline_index) {
        auto const& timeline = sample_ports.physical.feedback_timelines[timeline_index];
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
         materialization_index < sample_ports.physical.materializations.size();
         ++materialization_index) {
        auto const& materialization =
            sample_ports.physical.materializations[materialization_index];
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
            {EventOperationKind::sequence_reset, reset.representation});
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
        if (primitive_index >= configuration_steps.size()
            || !configuration_steps[primitive_index]) {
            return std::unexpected(
                "GraphJit event output is absent from the execution schedule");
        }
        auto const step_index = *configuration_steps[primitive_index];
        for (auto const& output : event_ports.primitives[primitive_index].outputs) {
            if (!output.append_existing || !output.representation) continue;
            auto const representation = *output.representation;
            if (representation >= event_ports.representations.size()) {
                return std::unexpected(
                    "GraphJit event output reset references an invalid representation");
            }
            auto const initialized_elsewhere = std::ranges::any_of(
                event_ports.carry_operations,
                [&](EventCarryPlan const& carry) {
                    return carry.working_representation == representation;
                }) || event_ports.representations[representation].persistent_ring;
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
                        && operation.index == representation;
                });
            if (!already_scheduled) {
                // reset_scope is constructed from step_regions above, so a
                // region index here is already an ExecutionPlan::regions index
                // rather than a ConnectionAnalysisPlan schedule-region index.
                if (reset_scope.kind == EventOperationScopeKind::region) {
                    plan.regions[reset_scope.index].event_operations_before.push_back(
                        {EventOperationKind::sequence_reset, representation});
                } else {
                    plan.primitive_steps[reset_scope.index]
                        .event_operations_before.push_back(
                            {EventOperationKind::sequence_reset, representation});
                }
            }
        }
    }

    auto operation_rank = [](EventOperationRef operation) {
        switch (operation.kind) {
        case EventOperationKind::sequence_reset: return 0;
        case EventOperationKind::persistent_ring_prune: return 1;
        case EventOperationKind::carry_restore: return 2;
        case EventOperationKind::merge: return 3;
        case EventOperationKind::feedback_append: return 4;
        case EventOperationKind::materialize: return 5;
        case EventOperationKind::carry_commit: return 6;
        }
        return 7;
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

    auto const sample_size = sample_ports.physical.transient_arena_size;
    auto const sample_alignment = std::max(
        std::size_t{1}, sample_ports.physical.transient_arena_alignment);
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
} // namespace

std::expected<LoweringPlan, std::string> build_lowering_plan(
    LoweringInput const& input)
{
    auto analysis = analyze_graph(input);
    if (!analysis) return std::unexpected(std::move(analysis.error()));

    struct RealtimePortPlans {
        ConnectionAnalysisPlan connections{};
        SamplePortBindingPlan sample_ports{};
        EventPortBindingPlan event_ports{};
        RootStackBufferPlan root_stack{};
    };

    auto plan_realtime_ports = [&](RealtimeStorageCostModel const& cost_model)
        -> std::expected<RealtimePortPlans, std::string> {
        auto connections = build_connection_analysis_plan(
            input.graph, input.specialization.block_size, cost_model);
        if (!connections) {
            return std::unexpected(std::move(connections.error()));
        }
        if (connections->boundary_bundle < input.graph.node_bundles.size()) {
            auto const& boundary = input.graph.node_bundles.bundle(
                connections->boundary_bundle);
            if (boundary.sample_input_count() != 0
                || boundary.sample_output_count() != 0
                || boundary.event_input_count() != 0
                || boundary.event_output_count() != 0) {
                return std::unexpected(
                    "GraphJit root graph must not declare boundary ports; system I/O and inter-module communication belong in concrete node types");
            }
        }

        auto sample_ports = plan_sample_ports(
            input, *analysis, *connections, cost_model);
        if (!sample_ports) {
            return std::unexpected(std::move(sample_ports.error()));
        }
        auto event_ports = plan_event_ports(
            input, *analysis, *connections, cost_model);
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
    auto realtime_ports = plan_realtime_ports(configured_cost_model);
    if (!realtime_ports) {
        return std::unexpected(std::move(realtime_ports.error()));
    }

    auto const stack_budget = configured_cost_model.stack_budget_bytes;
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
        realtime_ports->sample_ports,
        realtime_ports->event_ports);
    if (!declarations) return std::unexpected(std::move(declarations.error()));

    auto imports = plan_package_imports(input, *analysis);
    if (!imports) return std::unexpected(std::move(imports.error()));

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

    return LoweringPlan{
        .connections = std::move(realtime_ports->connections),
        .declarations = std::move(*declarations),
        .imports = std::move(*imports),
        .configurations = std::move(*configurations),
        .sample_ports = std::move(realtime_ports->sample_ports),
        .event_ports = std::move(realtime_ports->event_ports),
        .root_stack = realtime_ports->root_stack,
        .execution = std::move(*execution),
    };
}
} // namespace iv::graph_jit::detail
