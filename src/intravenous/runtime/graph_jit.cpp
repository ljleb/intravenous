#include <intravenous/runtime/graph_jit.h>

#include <intravenous/graph_jit/lowering.h>
#include <intravenous/graph_jit/package_callback_abi.h>
#include <intravenous/graph_jit/realtime_root.h>
#include <intravenous/graph_jit/region_specialization.h>
#include <intravenous/graph_jit/region_specialization_emit.h>
#include <intravenous/orc_perf_support.h>
#include <intravenous/ports.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalIFunc.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/PassTimingInfo.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace iv {
namespace {
constexpr std::size_t no_node_bundle = std::numeric_limits<std::size_t>::max();

class GraphJitCompileError : public std::runtime_error {
public:
    GraphJitDiagnosticStage stage;

    GraphJitCompileError(GraphJitDiagnosticStage stage_, std::string message)
        : std::runtime_error(std::move(message))
        , stage(stage_)
    {}
};

[[noreturn]] void fail(GraphJitDiagnosticStage stage, std::string message)
{
    throw GraphJitCompileError(stage, std::move(message));
}

std::string llvm_error_string(llvm::Error error)
{
    std::string message;
    llvm::raw_string_ostream stream(message);
    llvm::logAllUnhandledErrors(std::move(error), stream);
    stream.flush();
    return message;
}

template<class T>
decltype(auto) take_llvm_expected(
    llvm::Expected<T> value,
    GraphJitDiagnosticStage stage,
    std::string_view context)
{
    if (!value) {
        fail(stage, std::string(context) + ": " + llvm_error_string(value.takeError()));
    }
    if constexpr (std::is_reference_v<T>) {
        return *value;
    } else {
        return std::move(*value);
    }
}

void check_llvm_error(
    llvm::Error error,
    GraphJitDiagnosticStage stage,
    std::string_view context)
{
    if (error) {
        fail(stage, std::string(context) + ": " + llvm_error_string(std::move(error)));
    }
}

void initialize_graph_jit_target()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (llvm::InitializeNativeTarget()) {
            throw std::runtime_error("failed to initialize LLVM native target");
        }
        if (llvm::InitializeNativeTargetAsmPrinter()) {
            throw std::runtime_error("failed to initialize LLVM native asm printer");
        }
        if (llvm::InitializeNativeTargetAsmParser()) {
            throw std::runtime_error("failed to initialize LLVM native asm parser");
        }
    });
}

std::string package_root_key(std::filesystem::path const& path)
{
    return path.lexically_normal().generic_string();
}

struct CapturedNode {
    std::size_t node_bundle = 0;
    std::shared_ptr<PackageRevision const> revision{};
    details::NodeCompilerRecord compiler_record{};
    RegisteredNodeTypeIdentity identity{};
    std::uint64_t provider_version = 0;
    std::shared_ptr<void const> configuration_identity{};
    void const* node_data = nullptr;
    NodeStateStructures const* state_structures = nullptr;
    std::size_t (*declare_node)(
        void const*,
        NodeStateStructures const*,
        NodeLayoutBuilder&,
        ReflectedNodePortRealization const&) = nullptr;
};

struct CapturedRelocation {
    std::size_t node_bundle = 0;
    NodeConfigRelocation const* relocation = nullptr;
    std::shared_ptr<PackageRevision const> revision{};
};

struct CapturedInputs {
    std::vector<CapturedNode> nodes{};
    std::vector<CapturedRelocation> relocations{};
    std::unordered_set<PackageRevision const*> used_revisions{};
    std::vector<GraphJitDiagnostic> diagnostics{};
};

GraphJitDiagnostic diagnostic(
    GraphJitDiagnosticStage stage,
    std::string message,
    std::size_t node_bundle = no_node_bundle,
    std::string node_type_id = {},
    std::string package_root = {})
{
    return GraphJitDiagnostic{
        .stage = stage,
        .message = std::move(message),
        .node_bundle = node_bundle,
        .node_type_id = std::move(node_type_id),
        .package_root = std::move(package_root),
    };
}

CapturedInputs capture_inputs(GraphJitCompileRequest const& request)
{
    CapturedInputs result;
    if (!request.graph) {
        result.diagnostics.push_back(diagnostic(
            GraphJitDiagnosticStage::input_capture,
            "GraphJit request has no ConfiguredGraph"));
        return result;
    }
    if (!request.definitions) {
        result.diagnostics.push_back(diagnostic(
            GraphJitDiagnosticStage::input_capture,
            "GraphJit request has no pinned NodeDefinitionsSnapshot"));
        return result;
    }

    std::unordered_map<std::string, std::shared_ptr<PackageRevision const>> revisions_by_root;
    for (auto const& revision : request.definitions->package_revisions) {
        if (!revision) {
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::input_capture,
                "pinned definition snapshot contains a null package revision"));
            continue;
        }
        auto const key = package_root_key(revision->package_root);
        auto [it, inserted] = revisions_by_root.emplace(key, revision);
        if (!inserted && it->second.get() != revision.get()) {
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::input_capture,
                "pinned definition snapshot contains duplicate package root '" + key + "'",
                no_node_bundle,
                {},
                key));
        }
    }

    struct PackageNode {
        std::shared_ptr<PackageRevision const> revision;
        details::NodeCompilerRecord const* record = nullptr;
    };
    // A type's NodeCodeKey can occur in several packages. The declaration
    // address comes from the package that actually constructed this node.
    using DeclarationCallback = decltype(details::NodeCompilerOperations::declare_node);
    std::unordered_multimap<DeclarationCallback, PackageNode> package_nodes;
    for (auto const& [root, revision] : revisions_by_root) {
        for (auto const& record : revision->node_compiler_records) {
            package_nodes.emplace(record.operations.declare_node,
                PackageNode{revision, &record});
        }
    }
    auto capture_node = [&](ConfiguredNodeBundleView const& view,
                            std::size_t bundle,
                            std::shared_ptr<PackageRevision const> const& revision,
                            details::NodeCompilerRecord const& record,
                            RegisteredNodeTypeIdentity const& identity,
                            std::uint64_t provider_version) {
        auto const expected_state = std::ranges::find(
            revision->node_state_structures, record.code_key,
            &details::BuilderNodeStateStructures::code_key);
        auto const* configured_state = view.operations->runtime.state_structures;
        auto const state_matches = expected_state == revision->node_state_structures.end()
            ? configured_state == nullptr
            : configured_state && *configured_state == expected_state->structures;
        if (!state_matches) {
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::input_capture,
                "configured node State/TockState metadata disagrees with pinned package revision",
                bundle, identity.node_type_id, identity.provider_package_root));
            return;
        }
        result.nodes.push_back(CapturedNode{
            .node_bundle = bundle,
            .revision = revision,
            .compiler_record = record,
            .identity = identity,
            .provider_version = provider_version,
            .configuration_identity = view.node_storage
                ? *view.node_storage : std::shared_ptr<void const>{},
            .node_data = view.operations->runtime.node_data,
            .state_structures = configured_state,
            .declare_node = view.operations->runtime.declare_node,
        });
        result.used_revisions.insert(revision.get());
    };

    std::size_t node_bundle = 0;
    request.graph->node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            auto const current_bundle = node_bundle++;
            if (view.kind != ConfiguredNodeBundleKind::concrete) return;

            if (view.registered_node_type_identity) {
                auto const& identity = *view.registered_node_type_identity;
                auto const definition = request.definitions->by_id.find(identity.node_type_id);
                if (definition == request.definitions->by_id.end()) {
                    result.diagnostics.push_back(diagnostic(
                        GraphJitDiagnosticStage::input_capture,
                        "configured registered node is absent from the pinned definition snapshot",
                        current_bundle,
                        identity.node_type_id,
                        identity.provider_package_root));
                } else if (definition->second.kind != NodeDefinitionKind::leaf) {
                    result.diagnostics.push_back(diagnostic(
                        GraphJitDiagnosticStage::input_capture,
                        "configured registered node resolves to a non-leaf definition",
                        current_bundle,
                        identity.node_type_id,
                        identity.provider_package_root));
                } else {
                    auto const* leaf = std::get_if<LeafNodeDefinition>(
                        &definition->second.definition);
                    auto const identity_root = package_root_key(
                        std::filesystem::path{identity.provider_package_root});
                    if (!leaf) {
                        result.diagnostics.push_back(diagnostic(
                            GraphJitDiagnosticStage::input_capture,
                            "leaf definition data is malformed",
                            current_bundle,
                            identity.node_type_id,
                            identity.provider_package_root));
                    } else if (leaf->definition_id != identity.node_type_id) {
                        result.diagnostics.push_back(diagnostic(
                            GraphJitDiagnosticStage::input_capture,
                            "configured node identity disagrees with pinned leaf definition",
                            current_bundle,
                            identity.node_type_id,
                            identity.provider_package_root));
                    } else if (package_root_key(leaf->package_root) != identity_root) {
                        result.diagnostics.push_back(diagnostic(
                            GraphJitDiagnosticStage::input_capture,
                            "configured node provider root disagrees with pinned leaf definition",
                            current_bundle,
                            identity.node_type_id,
                            identity.provider_package_root));
                    } else if (!view.code_key || leaf->compiler_record.code_key != *view.code_key) {
                        result.diagnostics.push_back(diagnostic(
                            GraphJitDiagnosticStage::input_capture,
                            "configured node NodeCodeKey disagrees with pinned leaf definition",
                            current_bundle,
                            identity.node_type_id,
                            identity.provider_package_root));
                    } else {
                        auto const revision = revisions_by_root.find(identity_root);
                        if (revision == revisions_by_root.end()
                            || revision->second->package_id != leaf->package_id) {
                            result.diagnostics.push_back(diagnostic(
                                GraphJitDiagnosticStage::input_capture,
                                "configured node provider revision is absent from the pinned "
                                "snapshot",
                                current_bundle,
                                identity.node_type_id,
                                identity.provider_package_root));
                        } else if (view.operations == nullptr
                            || view.operations->runtime.declare_node == nullptr) {
                            result.diagnostics.push_back(diagnostic(
                                GraphJitDiagnosticStage::input_capture,
                                "configured node has no declaration callback",
                                current_bundle,
                                identity.node_type_id,
                                identity.provider_package_root));
                        } else if (view.operations->runtime.declare_node
                            != leaf->compiler_record.operations.declare_node) {
                            result.diagnostics.push_back(diagnostic(
                                GraphJitDiagnosticStage::input_capture,
                                "configured node declaration callback disagrees with pinned "
                                "leaf definition",
                                current_bundle,
                                identity.node_type_id,
                                identity.provider_package_root));
                        } else {
                            capture_node(view, current_bundle, revision->second,
                                leaf->compiler_record, identity, definition->second.version);
                        }
                    }
                }
            } else {
                PackageNode const* matched = nullptr;
                if (view.code_key && view.operations
                    && view.operations->runtime.declare_node) {
                    auto const [begin, end] = package_nodes.equal_range(
                        view.operations->runtime.declare_node);
                    for (auto it = begin; it != end; ++it) {
                        if (it->second.record->code_key != *view.code_key) continue;
                        if (matched) {
                            result.diagnostics.push_back(diagnostic(
                                GraphJitDiagnosticStage::input_capture,
                                "internal concrete node matches more than one pinned package revision",
                                current_bundle));
                            return;
                        }
                        matched = &it->second;
                    }
                }
                if (!matched) {
                    auto const type = view.reflected_type_name
                        ? *view.reflected_type_name : std::string{};
                    result.diagnostics.push_back(diagnostic(
                        GraphJitDiagnosticStage::input_capture,
                        "internal concrete node '" + type + "' at bundle "
                            + std::to_string(current_bundle)
                            + " has no matching compiler record in the pinned package revisions",
                        current_bundle));
                    return;
                }
                // No public registration is invented for an internal leaf.
                // The provider root still identifies its exact package code.
                capture_node(view, current_bundle, matched->revision, *matched->record,
                    RegisteredNodeTypeIdentity{
                        .node_type_id = {},
                        .provider_package_root = package_root_key(matched->revision->package_root),
                    }, matched->revision->revision);
            }

            if (!view.config_relocations) return;
            for (auto const& relocation : *view.config_relocations) {
                if (relocation.byte_offset > view.node_size
                    || view.node_size - relocation.byte_offset < sizeof(void*)) {
                    result.diagnostics.push_back(diagnostic(
                        GraphJitDiagnosticStage::input_capture,
                        "node configuration pointer relocation is outside configuration storage",
                        current_bundle,
                        view.registered_node_type_identity
                            ? view.registered_node_type_identity->node_type_id
                            : std::string{},
                        relocation.package_root));
                    continue;
                }
                if (!relocation.retained_global_index) {
                    result.relocations.push_back(CapturedRelocation{
                        .node_bundle = current_bundle,
                        .relocation = &relocation,
                    });
                    continue;
                }
                auto const root = package_root_key(
                    std::filesystem::path{relocation.package_root});
                auto const revision = revisions_by_root.find(root);
                if (revision == revisions_by_root.end()) {
                    result.diagnostics.push_back(diagnostic(
                        GraphJitDiagnosticStage::input_capture,
                        "configuration relocation provider revision is absent from the pinned "
                        "snapshot",
                        current_bundle,
                        view.registered_node_type_identity
                            ? view.registered_node_type_identity->node_type_id
                            : std::string{},
                        relocation.package_root));
                    continue;
                }
                auto const index = *relocation.retained_global_index;
                if (index >= revision->second->retained_globals.size()) {
                    result.diagnostics.push_back(diagnostic(
                        GraphJitDiagnosticStage::input_capture,
                        "configuration relocation retained-global index is out of range",
                        current_bundle,
                        view.registered_node_type_identity
                            ? view.registered_node_type_identity->node_type_id
                            : std::string{},
                        relocation.package_root));
                    continue;
                }
                auto const& global = revision->second->retained_globals[index];
                if (global.index != index || relocation.addend >= global.size) {
                    result.diagnostics.push_back(diagnostic(
                        GraphJitDiagnosticStage::input_capture,
                        "configuration relocation retained-global metadata is invalid",
                        current_bundle,
                        view.registered_node_type_identity
                            ? view.registered_node_type_identity->node_type_id
                            : std::string{},
                        relocation.package_root));
                    continue;
                }
                result.relocations.push_back(CapturedRelocation{
                    .node_bundle = current_bundle,
                    .relocation = &relocation,
                    .revision = revision->second,
                });
                result.used_revisions.insert(revision->second.get());
            }
        });
    return result;
}

// The revision's node-code record for a captured node, by code key. The
// finalizer emits node-code records sorted by code key.
NodeCodeRecordData const* node_code_record(
    PackageRevision const& revision,
    NodeCodeKey key)
{
    auto const& records = revision.node_code_records;
    auto const found = std::ranges::lower_bound(records, key, {}, &NodeCodeRecordData::code_key);
    if (found == records.end() || found->code_key != key) return nullptr;
    return &*found;
}

// Node code is compiled from the same finalized package LLVM as the
// NodeCompilerRecord, so it must provide exactly the record's callbacks.
void verify_compiler_record(
    CapturedNode const& captured,
    NodeCodeRecordData const& node_code)
{
    auto const& accepted = captured.compiler_record.operations;
    if (!accepted.valid()) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "accepted NodeCompilerRecord has incomplete runtime callbacks for node '"
                + captured.identity.node_type_id + "'");
    }
    auto const& code = node_code.operations;
    auto const same = [](auto a, auto b) {
        return (a != nullptr) == (b != nullptr);
    };
    if (!same(accepted.tick_block, code.tick_block)
        || !same(accepted.tick_block_batch, code.tick_block_batch)
        || !same(accepted.skip_block, code.skip_block)
        || !same(accepted.skip_block_batch, code.skip_block_batch)
        || !same(accepted.tock_coverage, code.tock_coverage)
        || !same(accepted.tock_coverage_batch, code.tock_coverage_batch)
        || !same(accepted.propagate_forward_coverage, code.propagate_forward_coverage)
        || !same(
            accepted.propagate_forward_coverage_batch,
            code.propagate_forward_coverage_batch)
        || !same(accepted.propagate_reverse_coverage, code.propagate_reverse_coverage)
        || !same(
            accepted.propagate_reverse_coverage_batch,
            code.propagate_reverse_coverage_batch)
        || !same(accepted.initialize_sequential_input, code.initialize_sequential_input)
        || !same(
            accepted.combine_sequential_input_values,
            code.combine_sequential_input_values)) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "NodeCompilerRecord optional-operation ABI disagrees with the package node code for node '"
                + captured.identity.node_type_id + "'");
    }
}

void verify_module(llvm::Module const& module, GraphJitDiagnosticStage stage, std::string_view when)
{
    std::string diagnostics;
    llvm::raw_string_ostream stream(diagnostics);
    if (!llvm::verifyModule(module, &stream)) return;
    stream.flush();
    fail(stage, std::string(when) + ":\n" + diagnostics);
}

bool valid_storage_alignment(std::size_t alignment) noexcept
{
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

void validate_node_layout(NodeLayout const& layout)
{
    if (!valid_storage_alignment(layout.storage_alignment)) {
        fail(
            GraphJitDiagnosticStage::lowering,
            "graph lowerer returned a NodeLayout with non-power-of-two storage alignment");
    }

    for (std::size_t region_index = 0; region_index < layout.regions.size(); ++region_index) {
        auto const& region = layout.regions[region_index];
        if (!valid_storage_alignment(region.alignment)) {
            fail(
                GraphJitDiagnosticStage::lowering,
                "graph lowerer returned a NodeLayout region with invalid alignment at index "
                    + std::to_string(region_index));
        }
        if (region.storage_offset % region.alignment != 0
            || layout.storage_alignment < region.alignment) {
            fail(
                GraphJitDiagnosticStage::lowering,
                "graph lowerer returned a misaligned NodeLayout region at index "
                    + std::to_string(region_index));
        }
        if (region.storage_offset > layout.storage_size
            || region.size > layout.storage_size - region.storage_offset) {
            fail(
                GraphJitDiagnosticStage::lowering,
                "graph lowerer returned an out-of-bounds NodeLayout region at index "
                    + std::to_string(region_index));
        }
    }
}

void validate_tock_state_layout(
    TockStateLayout const& layout, NodeLayout const& node_layout)
{
    if (layout.nodes.size() != node_layout.nodes.size()) {
        fail(
            GraphJitDiagnosticStage::lowering,
            "graph lowerer returned a TockStateLayout whose node count disagrees with NodeLayout");
    }
    if (!valid_storage_alignment(layout.storage_alignment)) {
        fail(
            GraphJitDiagnosticStage::lowering,
            "graph lowerer returned a TockStateLayout with invalid storage alignment");
    }
    for (std::size_t node_index = 0; node_index < layout.nodes.size();
         ++node_index) {
        auto const& state = layout.nodes[node_index];
        if (!valid_storage_alignment(state.state_alignment)) {
            fail(
                GraphJitDiagnosticStage::lowering,
                "graph lowerer returned a TockStateLayout node with invalid alignment at index "
                    + std::to_string(node_index));
        }
        if (state.state_offset % state.state_alignment != 0
            || layout.storage_alignment < state.state_alignment
            || state.state_offset > layout.storage_size
            || state.state_size
                > layout.storage_size - state.state_offset) {
            fail(
                GraphJitDiagnosticStage::lowering,
                "graph lowerer returned an invalid TockStateLayout placement at index "
                    + std::to_string(node_index));
        }
        if (state.state_size != 0
            && (!state.lifecycle.default_construct_background_state_fn
                || !state.lifecycle.destroy_background_state_fn)) {
            fail(
                GraphJitDiagnosticStage::lowering,
                "graph lowerer returned a TockStateLayout node without state lifetime operations at index "
                    + std::to_string(node_index));
        }
    }
}

llvm::FunctionType* root_background_operation_type(llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {pointer}, false);
}

void validate_root_operation(
    llvm::Module const& module,
    std::string const& symbol,
    llvm::FunctionType const* expected_type,
    std::string_view role,
    bool required)
{
    if (symbol.empty()) {
        if (required) {
            fail(
                GraphJitDiagnosticStage::lowering,
                "graph lowerer did not provide a root " + std::string(role)
                    + " symbol");
        }
        return;
    }
    auto const* function = module.getFunction(symbol);
    if (!function || function->isDeclaration() || !function->hasExternalLinkage()) {
        fail(
            GraphJitDiagnosticStage::lowering,
            "graph lowerer root " + std::string(role) + " operation '" + symbol
                + "' is not an externally visible function definition");
    }
    if (function->getCallingConv() != llvm::CallingConv::C
        || function->getFunctionType() != expected_type) {
        fail(
            GraphJitDiagnosticStage::lowering,
            "graph lowerer root " + std::string(role) + " operation '" + symbol
                + "' has the wrong native ABI");
    }
}

void validate_lowering_output(
    llvm::Module const& module,
    graph_jit::LoweringOutput const& output)
{
    validate_node_layout(output.node_layout);
    validate_tock_state_layout(output.tock_state_layout, output.node_layout);
    for (std::size_t node_index = 0;
         node_index < output.background_evaluation_plan.nodes.size();
         ++node_index) {
        auto const& node = output.background_evaluation_plan.nodes[node_index];
        if (node.authored_tock_execution
            && (!node.tock_state_node
                || *node.tock_state_node
                    >= output.tock_state_layout.nodes.size())) {
            fail(
                GraphJitDiagnosticStage::lowering,
                "graph lowerer returned an authored background node without a valid TockState sidecar index at index "
                    + std::to_string(node_index));
        }
    }

    if (!output.realtime_root) {
        fail(
            GraphJitDiagnosticStage::lowering,
            "graph lowerer returned no immutable realtime root plan");
    }
    auto const transition_root_required = output.transition
        && (!output.transition->state_transition.sample_expiry_cursors.empty()
            || output.transition->state_transition.event_expiry_samples != 0);
    if (static_cast<bool>(output.transition_realtime_root)
        != transition_root_required) {
        fail(
            GraphJitDiagnosticStage::lowering,
            "graph lowerer returned inconsistent transition realtime root data");
    }

    auto* background_type = root_background_operation_type(module.getContext());
    auto const background_required = !output.background_evaluation_plan.empty();
    validate_root_operation(
        module,
        output.root_symbols.propagate_background_forward,
        background_type,
        "background forward propagation",
        background_required);
    validate_root_operation(
        module,
        output.root_symbols.propagate_background_reverse,
        background_type,
        "background reverse propagation",
        background_required);
    validate_root_operation(
        module,
        output.root_symbols.evaluate_background,
        background_type,
        "background evaluation",
        background_required);
}

// Profiling knobs, not configuration: IV_GRAPH_JIT_OPT_LEVEL (0-3, default 3)
// and IV_GRAPH_JIT_CODEGEN_LEVEL (0-3, default 3) measure how compile time
// scales with optimization effort.
llvm::OptimizationLevel project_optimization_level()
{
    char const* value = std::getenv("IV_GRAPH_JIT_OPT_LEVEL");
    std::string_view const level = value ? value : "3";
    if (level == "0") return llvm::OptimizationLevel::O0;
    if (level == "1") return llvm::OptimizationLevel::O1;
    if (level == "2") return llvm::OptimizationLevel::O2;
    return llvm::OptimizationLevel::O3;
}

llvm::CodeGenOptLevel project_codegen_level()
{
    char const* value = std::getenv("IV_GRAPH_JIT_CODEGEN_LEVEL");
    std::string_view const level = value ? value : "3";
    if (level == "0") return llvm::CodeGenOptLevel::None;
    if (level == "1") return llvm::CodeGenOptLevel::Less;
    if (level == "2") return llvm::CodeGenOptLevel::Default;
    return llvm::CodeGenOptLevel::Aggressive;
}

// The owning scope of a demangled function name with template and parameter
// lists removed, limited to three components: the inventory's grouping key.
std::string function_group(std::string_view demangled)
{
    std::string plain;
    int angle = 0;
    int paren = 0;
    for (std::size_t index = 0; index < demangled.size(); ++index) {
        char const c = demangled[index];
        if (demangled.substr(index).starts_with("operator")) {
            plain += "operator";
            index += std::string_view("operator").size() - 1;
            continue;
        }
        if (c == '<') { ++angle; continue; }
        if (c == '>' && angle > 0) { --angle; continue; }
        if (c == '(') { ++paren; continue; }
        if (c == ')' && paren > 0) { --paren; continue; }
        if (angle == 0 && paren == 0) plain += c;
    }
    std::vector<std::string_view> components;
    std::string_view rest = plain;
    while (true) {
        auto const separator = rest.find("::");
        if (separator == std::string_view::npos) {
            components.push_back(rest);
            break;
        }
        components.push_back(rest.substr(0, separator));
        rest.remove_prefix(separator + 2);
    }
    if (components.size() > 1) components.pop_back();
    if (components.size() > 3) components.resize(3);
    std::string group;
    for (auto const component : components) {
        if (!group.empty()) group += "::";
        group += component;
    }
    return group.empty() ? std::string("<global>") : group;
}

// The profiling output file `stem` + `suffix` under IV_GRAPH_JIT_PROFILE_DIR,
// or nothing when profiling output is off.
std::optional<std::filesystem::path> profile_path(
    std::string const& stem, std::string_view suffix)
{
    char const* directory = std::getenv("IV_GRAPH_JIT_PROFILE_DIR");
    if (!directory || !*directory) return std::nullopt;
    std::filesystem::create_directories(directory);
    return std::filesystem::path(directory) / (stem + std::string(suffix));
}

// With IV_GRAPH_JIT_PROFILE_DIR set and IV_GRAPH_JIT_DUMP_IR=1, writes the
// module's textual LLVM, to read what the generated root contains.
void write_module_ir(llvm::Module const& module, std::string const& stem)
{
    char const* dump = std::getenv("IV_GRAPH_JIT_DUMP_IR");
    if (!dump || std::string_view(dump) != "1") return;
    auto const path = profile_path(stem, ".ll");
    if (!path) return;
    std::error_code error;
    llvm::raw_fd_ostream out(path->string(), error);
    if (!error) module.print(out, nullptr);
}

// With IV_GRAPH_JIT_PROFILE_DIR set, writes every defined function's
// instruction count and the totals per owning scope, to see what one
// compilation spends its optimization and code generation on.
void write_function_inventory(llvm::Module const& module, std::string const& stem)
{
    char const* directory = std::getenv("IV_GRAPH_JIT_PROFILE_DIR");
    if (!directory || !*directory) return;
    std::filesystem::create_directories(directory);
    struct Entry {
        std::size_t instructions = 0;
        std::string name{};
    };
    std::vector<Entry> entries;
    for (auto const& function : module) {
        if (function.isDeclaration()) continue;
        entries.push_back({
            .instructions = function.getInstructionCount(),
            .name = llvm::demangle(function.getName().str()),
        });
    }
    std::ranges::sort(entries, std::ranges::greater{}, &Entry::instructions);
    std::ofstream functions(std::filesystem::path(directory) / (stem + "-functions.tsv"));
    functions << "instructions\tfunction\n";
    for (auto const& entry : entries) {
        functions << entry.instructions << '\t' << entry.name << '\n';
    }

    struct Group {
        std::size_t functions = 0;
        std::size_t instructions = 0;
    };
    std::unordered_map<std::string, Group> groups;
    for (auto const& entry : entries) {
        auto& group = groups[function_group(entry.name)];
        ++group.functions;
        group.instructions += entry.instructions;
    }
    std::vector<std::pair<std::string, Group>> ordered(groups.begin(), groups.end());
    std::ranges::sort(ordered, std::ranges::greater{}, [](auto const& entry) {
        return entry.second.instructions;
    });
    std::ofstream summary(std::filesystem::path(directory) / (stem + "-groups.tsv"));
    summary << "instructions\tfunctions\tscope\n";
    for (auto const& [name, group] : ordered) {
        summary << group.instructions << '\t' << group.functions << '\t' << name << '\n';
    }
}

enum class ProjectModulePipeline {
    complete,
    simplification,
};

// level overrides IV_GRAPH_JIT_OPT_LEVEL. A region specialization unrolls
// loops: its constant trip counts come from the immutable values GraphJit
// substitutes, and the module is compiled once per cached region.
void optimize_project_module(
    llvm::Module& module, llvm::TargetMachine& target_machine,
    std::string const& profile_stem,
    std::optional<llvm::OptimizationLevel> level_override = std::nullopt,
    bool loop_unrolling = false,
    ProjectModulePipeline pipeline_kind = ProjectModulePipeline::complete)
{
    // The project module contains no package code: each revision's package
    // code was compiled once, before any project compilation used it.
    // Profiling knob, not configuration: IV_GRAPH_JIT_ROOT_OPTNONE=1 leaves the
    // generated root operations unoptimized, to measure how much compile time
    // and realtime cost root optimization accounts for.
    if (char const* value = std::getenv("IV_GRAPH_JIT_ROOT_OPTNONE");
        value && std::string_view(value) == "1") {
        for (auto& function : module) {
            if (function.isDeclaration()
                || !function.getName().starts_with("__iv_graph_root_")) {
                continue;
            }
            function.addFnAttr(llvm::Attribute::OptimizeNone);
            function.addFnAttr(llvm::Attribute::NoInline);
        }
    }
    llvm::PassInstrumentationCallbacks instrumentation;
    // The new pass manager skips optnone functions only through this
    // instrumentation; without it the optnone roots above are still optimized
    // (only inlining into them is refused).
    llvm::OptNoneInstrumentation optnone(false);
    optnone.registerCallbacks(instrumentation);
    // With profiling output on, time every optimization pass and analysis.
    auto const passes_path = profile_path(profile_stem, "-optimization-passes.txt");
    std::optional<llvm::TimePassesHandler> time_passes;
    if (passes_path) {
        time_passes.emplace(true);
        time_passes->registerCallbacks(instrumentation);
    }
    // Loop unrolling (partial, runtime, and full) is off in the per-edit
    // pipeline: at 64 voices it cost about 1.6 s per edit with no measurable
    // realtime benefit (Front 16, 2026-11-07). Loops whose trip counts are
    // package constants are already fully unrolled by the package's pre-link
    // pipeline in iv_package_finalize. Profiling knob, not configuration:
    // IV_GRAPH_JIT_UNROLL=1 re-enables unrolling to compare realtime cost.
    llvm::PipelineTuningOptions tuning;
    tuning.LoopUnrolling = loop_unrolling;
    if (char const* value = std::getenv("IV_GRAPH_JIT_UNROLL");
        value && std::string_view(value) == "1") {
        tuning.LoopUnrolling = true;
    }
    llvm::PassBuilder pass_builder(
        &target_machine, tuning, std::nullopt, &instrumentation);
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager cgscc;
    llvm::ModuleAnalysisManager modules;
    pass_builder.registerModuleAnalyses(modules);
    pass_builder.registerCGSCCAnalyses(cgscc);
    pass_builder.registerFunctionAnalyses(functions);
    pass_builder.registerLoopAnalyses(loops);
    pass_builder.crossRegisterProxies(loops, functions, cgscc, modules);
    auto const level = level_override ? *level_override : project_optimization_level();
    auto pipeline = level == llvm::OptimizationLevel::O0
        ? pass_builder.buildO0DefaultPipeline(level)
        : pipeline_kind == ProjectModulePipeline::simplification
        ? pass_builder.buildModuleSimplificationPipeline(
              level, llvm::ThinOrFullLTOPhase::None)
        : pass_builder.buildPerModuleDefaultPipeline(level);
    pipeline.run(module, modules);
    if (passes_path) {
        std::error_code error;
        llvm::raw_fd_ostream out(passes_path->string(), error);
        if (!error) {
            time_passes->setOutStream(out);
            time_passes->print();
        }
    }
}

// Records, for each ResourceTracker, the memory that JITLink's in-process
// memory manager keeps for the tracker's linked code and data. That manager,
// the default of LLJIT's in-process executor, lays out each LinkGraph with
// BasicLayout, reserves every segment in whole pages, and releases the
// finalize-lifetime segments once finalization completes. The memory a linked
// graph keeps is therefore its page-rounded standard-lifetime segments, which
// the post-allocation pass computes with the same layout. ORC's heap
// bookkeeping for a JITDylib, such as its symbol table and definition
// generators, lies outside these pages and is not included.
class AllocatedCodeSizePlugin final
    : public llvm::orc::LinkGraphLinkingLayer::Plugin {
    mutable std::mutex mutex_{};
    std::unordered_map<llvm::orc::ResourceKey, std::size_t> bytes_{};

    static std::size_t add_saturated(
        std::size_t left,
        std::size_t right) noexcept
    {
        return right > std::numeric_limits<std::size_t>::max() - left
            ? std::numeric_limits<std::size_t>::max()
            : left + right;
    }

    void erase(llvm::orc::ResourceKey key)
    {
        std::lock_guard lock(mutex_);
        bytes_.erase(key);
    }

public:
    void modifyPassConfig(
        llvm::orc::MaterializationResponsibility& responsibility,
        llvm::jitlink::LinkGraph&,
        llvm::jitlink::PassConfiguration& configuration) override
    {
        auto const page_size =
            responsibility.getExecutionSession().getPageSize();
        auto error = responsibility.withResourceKeyDo(
            [&](llvm::orc::ResourceKey key) {
                configuration.PostAllocationPasses.push_back(
                    [this, key, page_size](llvm::jitlink::LinkGraph& graph)
                        -> llvm::Error {
                        llvm::jitlink::BasicLayout layout(graph);
                        auto sizes =
                            layout.getContiguousPageBasedLayoutSizes(page_size);
                        if (!sizes) return sizes.takeError();
                        static_assert(sizeof(std::size_t) >= sizeof(std::uint64_t));
                        auto const bytes =
                            static_cast<std::size_t>(sizes->StandardSegs);
                        std::lock_guard lock(mutex_);
                        bytes_[key] = add_saturated(bytes_[key], bytes);
                        return llvm::Error::success();
                    });
            });
        llvm::consumeError(std::move(error));
    }

    llvm::Error notifyEmitted(
        llvm::orc::MaterializationResponsibility&) override
    {
        return llvm::Error::success();
    }

    llvm::Error notifyFailed(
        llvm::orc::MaterializationResponsibility& responsibility) override
    {
        return responsibility.withResourceKeyDo(
            [&](llvm::orc::ResourceKey key) { erase(key); });
    }

    llvm::Error notifyRemovingResources(
        llvm::orc::JITDylib&,
        llvm::orc::ResourceKey key) override
    {
        erase(key);
        return llvm::Error::success();
    }

    void notifyTransferringResources(
        llvm::orc::JITDylib&,
        llvm::orc::ResourceKey destination,
        llvm::orc::ResourceKey source) override
    {
        std::lock_guard lock(mutex_);
        auto const found = bytes_.find(source);
        if (found == bytes_.end()) return;
        bytes_[destination] = add_saturated(bytes_[destination], found->second);
        bytes_.erase(found);
    }

    std::size_t allocated_bytes(
        llvm::orc::ResourceTrackerSP const& resources) const noexcept
    {
        if (!resources) return 0;
        std::size_t result = 0;
        auto error = resources->withResourceKeyDo(
            [&](llvm::orc::ResourceKey key) {
                std::lock_guard lock(mutex_);
                if (auto const found = bytes_.find(key); found != bytes_.end()) {
                    result = found->second;
                }
            });
        llvm::consumeError(std::move(error));
        return result;
    }
};

struct SharedProjectJit {
    std::mutex mutex{};
    std::unique_ptr<llvm::orc::LLJIT> jit{};
    std::shared_ptr<AllocatedCodeSizePlugin> allocated_code_sizes{};
    std::atomic<std::uint64_t> next_generation{0};
};

struct ProjectCodeLifetime {
    std::shared_ptr<SharedProjectJit> shared{};
    // The revisions whose node code this generation calls. Declared first so
    // that they are released only after the generation's own JITDylib.
    std::vector<std::shared_ptr<PackageRevision const>> package_revisions{};
    llvm::orc::JITDylib* jit_dylib = nullptr;
    llvm::orc::ResourceTrackerSP resources{};
    // AllocatedCodeSizePlugin's measure for resources. Region specializations
    // record it once their code is materialized; it is zero for project
    // generation code.
    std::size_t allocated_code_bytes = 0;

    void release_locked() noexcept
    {
        if (!shared || !shared->jit || !jit_dylib) return;
        if (auto error = shared->jit->deinitialize(*jit_dylib)) {
            llvm::consumeError(std::move(error));
        }
        if (resources) {
            if (auto error = resources->remove()) {
                llvm::consumeError(std::move(error));
            }
            resources.reset();
        }
        if (auto error = shared->jit->getExecutionSession().removeJITDylib(*jit_dylib)) {
            llvm::consumeError(std::move(error));
        }
        jit_dylib = nullptr;
    }

    ~ProjectCodeLifetime()
    {
        if (!shared || !shared->jit || !jit_dylib) return;
        std::lock_guard lock(shared->mutex);
        release_locked();
    }
};

// Adds the generators a generation JITDylib needs for the external symbols of
// project code: the iv_builder library and the revisions' native dynamic
// libraries.
void add_project_code_generators(
    llvm::orc::LLJIT& jit,
    llvm::orc::JITDylib& jit_dylib,
    std::span<PackageRevision const* const> revisions)
{
    auto const global_prefix = jit.getDataLayout().getGlobalPrefix();
#if defined(IV_CONFIGURED_IV_BUILDER_LIBRARY)
    if (std::string_view(IV_CONFIGURED_IV_BUILDER_LIBRARY).size()) {
        jit_dylib.addGenerator(take_llvm_expected(
            llvm::orc::DynamicLibrarySearchGenerator::Load(
                IV_CONFIGURED_IV_BUILDER_LIBRARY, global_prefix),
            GraphJitDiagnosticStage::materialization,
            "load iv_builder project symbol resolver"));
    }
#endif
    std::unordered_set<std::string> loaded_libraries;
    for (auto const* revision : revisions) {
        for (auto const& library : revision->compiler_artifact.dynamic_libraries) {
            auto const path = library.lexically_normal().string();
            if (!loaded_libraries.insert(path).second) continue;
            jit_dylib.addGenerator(take_llvm_expected(
                llvm::orc::DynamicLibrarySearchGenerator::Load(
                    path.c_str(), global_prefix),
                GraphJitDiagnosticStage::materialization,
                "load project dynamic library '" + path + "'"));
        }
    }
}

// The node-code addresses one project compilation imports, by the symbol the
// project module declares for each. Node code lives in the package JIT, so the
// generation JITDylib defines these as absolute symbols.
struct ImportedNodeCode {
    struct Symbol {
        std::uint64_t address = 0;
        bool callable = false;
    };
    std::unordered_map<std::string, Symbol> symbols{};

    template<class Function>
    std::string callback(Function* function)
    {
        if (function == nullptr) return {};
        auto const address = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(function));
        auto name = "__iv_node_code_" + llvm::utohexstr(address);
        symbols.try_emplace(name, Symbol{.address = address, .callable = true});
        return name;
    }

    std::string data(void const* pointer)
    {
        auto const address = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(pointer));
        auto name = "__iv_node_data_" + llvm::utohexstr(address);
        symbols.try_emplace(name, Symbol{.address = address, .callable = false});
        return name;
    }
};

void define_imported_node_code(
    llvm::orc::LLJIT& jit,
    llvm::orc::JITDylib& jit_dylib,
    llvm::orc::ResourceTrackerSP const& resources,
    ImportedNodeCode const& imported,
    std::string_view role)
{
    if (imported.symbols.empty()) return;
    llvm::orc::SymbolMap symbols;
    for (auto const& [name, symbol] : imported.symbols) {
        llvm::JITSymbolFlags flags = llvm::JITSymbolFlags::Exported;
        if (symbol.callable) flags |= llvm::JITSymbolFlags::Callable;
        symbols[jit.mangleAndIntern(name)] = llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr(symbol.address), flags);
    }
    check_llvm_error(
        jit_dylib.define(
            llvm::orc::absoluteSymbols(std::move(symbols)), resources),
        GraphJitDiagnosticStage::materialization,
        "define " + std::string(role) + " imported node code");
}

struct MaterializedProjectCode {
    CompiledGraphRootOperations root_operations{};
    CompiledGraphBackgroundOperations background_operations{};
    std::shared_ptr<void const> lifetime{};
};

MaterializedProjectCode materialize_project_module(
    std::shared_ptr<SharedProjectJit> const& shared,
    std::unique_ptr<llvm::Module> module,
    std::unique_ptr<llvm::LLVMContext> context,
    std::span<std::shared_ptr<PackageRevision const> const> package_revisions,
    ImportedNodeCode const& imported,
    graph_jit::LoweredGraphRootSymbols const& root_symbols,
    std::shared_ptr<graph_jit::RealtimeRootPlan const> realtime_root,
    std::shared_ptr<graph_jit::RealtimeRootPlan const> transition_realtime_root,
    std::uint64_t project_generation)
{
    auto lifetime = std::make_shared<ProjectCodeLifetime>();
    lifetime->shared = shared;
    lifetime->package_revisions.assign(package_revisions.begin(), package_revisions.end());

    std::lock_guard lock(shared->mutex);
    auto& jit = *shared->jit;
    auto const suffix = shared->next_generation.fetch_add(1, std::memory_order_relaxed);
    try {
        auto& jit_dylib = take_llvm_expected(
            jit.createJITDylib(
                "iv.project." + std::to_string(project_generation) + "."
                    + std::to_string(suffix)),
            GraphJitDiagnosticStage::materialization,
            "create project JITDylib");
        lifetime->jit_dylib = &jit_dylib;
        lifetime->resources = jit_dylib.createResourceTracker();
        define_imported_node_code(
            jit, jit_dylib, lifetime->resources, imported, "project");
        std::vector<PackageRevision const*> revisions;
        revisions.reserve(package_revisions.size());
        for (auto const& revision : package_revisions) revisions.push_back(revision.get());
        add_project_code_generators(jit, jit_dylib, revisions);

        llvm::orc::ThreadSafeContext thread_safe_context(std::move(context));
        check_llvm_error(
            jit.addIRModule(
                lifetime->resources,
                llvm::orc::ThreadSafeModule(
                    std::move(module), std::move(thread_safe_context))),
            GraphJitDiagnosticStage::materialization,
            "add project LLVM to ORC JIT");
        check_llvm_error(
            jit.initialize(jit_dylib),
            GraphJitDiagnosticStage::materialization,
            "initialize project LLVM");

        auto symbol = [&]<class Function>(
                          std::string const& name,
                          std::string_view role) -> Function {
            auto address = take_llvm_expected(
                jit.lookup(jit_dylib, name),
                GraphJitDiagnosticStage::materialization,
                "lookup project " + std::string(role) + " symbol '" + name + "'");
            return address.template toPtr<Function>();
        };

        CompiledGraphRootOperations root_operations{
            .tick_block = {
                .function = &graph_jit::execute_realtime_root,
                .plan = std::move(realtime_root),
            },
            .transition_tick_block = transition_realtime_root
                ? CompiledGraphBlockOperation{
                    .function = &graph_jit::execute_realtime_root,
                    .plan = std::move(transition_realtime_root),
                }
                : CompiledGraphBlockOperation{},
            .combine_sequential_input = symbol.template operator()<
                CompiledGraphSequentialInputCombineFunction>(
                    root_symbols.combine_sequential_input,
                    "sequential input combiner"),
            .initialize_sequential_input = symbol.template operator()<
                CompiledGraphSequentialInputInitializeFunction>(
                    root_symbols.initialize_sequential_input,
                    "sequential input neutral initializer"),
        };
        if (!root_operations.valid()) {
            fail(
                GraphJitDiagnosticStage::materialization,
                "project LLVM resolved an incomplete root-node execution ABI");
        }
        CompiledGraphBackgroundOperations background_operations;
        if (!root_symbols.propagate_background_forward.empty()
            || !root_symbols.propagate_background_reverse.empty()
            || !root_symbols.evaluate_background.empty()) {
            if (root_symbols.propagate_background_forward.empty()
                || root_symbols.propagate_background_reverse.empty()
                || root_symbols.evaluate_background.empty()) {
                fail(
                    GraphJitDiagnosticStage::materialization,
                    "project LLVM exposed an incomplete background execution ABI");
            }
            background_operations = CompiledGraphBackgroundOperations{
                .propagate_forward = symbol.template operator()<
                    CompiledGraphBackgroundFunction>(
                    root_symbols.propagate_background_forward,
                    "background forward propagation"),
                .propagate_reverse = symbol.template operator()<
                    CompiledGraphBackgroundFunction>(
                    root_symbols.propagate_background_reverse,
                    "background reverse propagation"),
                .evaluate = symbol.template operator()<
                    CompiledGraphBackgroundFunction>(
                    root_symbols.evaluate_background,
                    "background evaluation"),
            };
            if (!background_operations.valid()) {
                fail(
                    GraphJitDiagnosticStage::materialization,
                    "project LLVM resolved an incomplete background execution ABI");
            }
        }
        return MaterializedProjectCode{
            .root_operations = root_operations,
            .background_operations = background_operations,
            .lifetime = std::shared_ptr<void const>(std::move(lifetime)),
        };
    } catch (...) {
        lifetime->release_locked();
        throw;
    }
}

// One package revision's finalized bitcode, read once per package build, and
// the tick_block definition of each of its nodes.
struct PackageBitcode {
    std::unique_ptr<llvm::MemoryBuffer> buffer{};
    std::vector<std::pair<NodeCodeKey, std::string>> tick_blocks{};
    std::vector<graph_jit::PackageRetainedGlobalDefinition> retained_globals{};
};

struct CachedRegionSpecialization {
    std::weak_ptr<ProjectCodeLifetime> lifetime{};
    // The cache's own strong owner, set only while the entry is in the
    // recent-use list. Compiled graphs keep their own owners independently.
    std::shared_ptr<ProjectCodeLifetime> retained_lifetime{};
    graph_jit::RegionSpecializationFunction function = nullptr;
    std::size_t allocated_code_bytes = 0;
    std::optional<std::list<std::string>::iterator> recent{};
};

struct CachedPackageBitcode {
    // The bitcode is useful only while some accepted package generation with
    // this content-addressed artifact remains alive. A later request for the
    // same path refreshes this owner.
    std::weak_ptr<PackageRevision const> revision{};
    std::shared_ptr<PackageBitcode const> bitcode{};
};

// Region specializations live while a realtime root plan uses them, or while
// the recent-use cache owns them. The cache owns the most recently requested
// specializations whose AllocatedCodeSizePlugin bytes fit in budget_bytes and
// evicts the least recently requested one first. retained_bytes counts every
// entry the cache owns, including one that a live compiled graph also runs, so
// the budget bounds the cache's ownership rather than only the code that the
// cache alone keeps alive. Eviction and release remove only the cache's owner;
// code that a compiled graph still owns remains usable and reusable.
struct RegionSpecializationCache {
    std::unordered_map<std::string, CachedRegionSpecialization> entries{};
    std::unordered_map<std::string, CachedPackageBitcode> package_bitcode{};
    // Keys of the owned entries, least recently requested first.
    std::list<std::string> recent{};
    std::size_t retained_bytes = 0;
    std::size_t budget_bytes = 0;
    std::uint64_t next_name = 0;

    void stop_retaining(CachedRegionSpecialization& entry)
    {
        if (!entry.retained_lifetime) return;
        retained_bytes -= entry.allocated_code_bytes;
        entry.retained_lifetime.reset();
        if (entry.recent) {
            recent.erase(*entry.recent);
            entry.recent.reset();
        }
    }

    void evict_oldest()
    {
        auto const key = std::move(recent.front());
        recent.pop_front();
        auto found = entries.find(key);
        if (found == entries.end()) return;
        auto& entry = found->second;
        entry.recent.reset();
        retained_bytes -= entry.allocated_code_bytes;
        entry.retained_lifetime.reset();
        if (entry.lifetime.expired()) entries.erase(found);
    }

    void retain(
        std::string const& key,
        CachedRegionSpecialization& entry,
        std::shared_ptr<ProjectCodeLifetime> lifetime)
    {
        if (entry.retained_lifetime) {
            recent.splice(recent.end(), recent, *entry.recent);
            return;
        }
        if (!lifetime || entry.allocated_code_bytes == 0 || budget_bytes == 0) {
            return;
        }
        if (entry.allocated_code_bytes > budget_bytes) return;
        while (!recent.empty()
            && retained_bytes > budget_bytes - entry.allocated_code_bytes) {
            evict_oldest();
        }
        entry.retained_lifetime = std::move(lifetime);
        retained_bytes += entry.allocated_code_bytes;
        recent.push_back(key);
        entry.recent = std::prev(recent.end());
    }

    // Stops owning every specialization that calls node code of a package
    // revision outside current, the revisions of the compilation's
    // definitions. Such code can never be reused: region_specialization_key()
    // names each revision's retained-global addresses, which a later revision
    // of the package does not share. The cache's owner would otherwise keep
    // that revision, with its package code and bitcode, alive until eviction.
    void stop_retaining_superseded(
        std::span<std::shared_ptr<PackageRevision const> const> current)
    {
        for (auto& item : entries) {
            auto& entry = item.second;
            if (!entry.retained_lifetime) continue;
            auto const superseded = std::ranges::any_of(
                entry.retained_lifetime->package_revisions,
                [&](std::shared_ptr<PackageRevision const> const& revision) {
                    return !std::ranges::contains(current, revision);
                });
            if (superseded) stop_retaining(entry);
        }
    }

    void remove_expired()
    {
        for (auto entry = entries.begin(); entry != entries.end();) {
            if (entry->second.lifetime.expired()) {
                stop_retaining(entry->second);
                entry = entries.erase(entry);
            } else {
                ++entry;
            }
        }
        std::erase_if(package_bitcode, [](auto const& entry) {
            return entry.second.revision.expired();
        });
    }
};

// Compiles, or finds in the cache, the region specializations of one
// GraphJit compilation, synchronously (human co-author decision,
// 2026-11-11). A failure leaves that region on the interpreter path and is
// reported in the compilation's timings.
class RegionSpecializationCompiler final : public graph_jit::RegionSpecializationProvider {
    std::shared_ptr<SharedProjectJit> const& shared_;
    llvm::TargetMachine& target_;
    RegionSpecializationCache& cache_;
    GraphJitCompileTimings& timings_;
    // One deterministic compiler failure per content key is enough during a
    // GraphJit compilation. Equal regions keep the interpreter without
    // repeating the same expensive failed LLVM work. A later graph
    // compilation gets one fresh attempt.
    std::unordered_map<std::string, std::string> failures_{};

    PackageBitcode const& package_bitcode(
        std::shared_ptr<PackageRevision const> const& revision)
    {
        auto const path = revision->compiler_artifact.bitcode_path.string();
        if (auto found = cache_.package_bitcode.find(path);
            found != cache_.package_bitcode.end()) {
            // The artifact name includes its build signature. Reusing these
            // decoded records for another accepted PackageRevision at the
            // same path is exact; follow the newest owner so the entry is not
            // retained by an unrelated obsolete generation.
            found->second.revision = revision;
            return *found->second.bitcode;
        }
        auto buffer = llvm::MemoryBuffer::getFile(path);
        if (!buffer) {
            fail(
                GraphJitDiagnosticStage::package_llvm,
                "cannot read package bitcode '" + path + "': "
                    + buffer.getError().message());
        }
        auto bitcode = std::make_shared<PackageBitcode>();
        bitcode->buffer = std::move(*buffer);
        {
            llvm::LLVMContext context;
            auto package = take_llvm_expected(
                llvm::getOwningLazyBitcodeModule(
                    llvm::MemoryBuffer::getMemBuffer(
                        bitcode->buffer->getMemBufferRef(), false),
                    context),
                GraphJitDiagnosticStage::package_llvm,
                "read package bitcode '" + path + "'");
            auto symbols = graph_jit::package_tick_block_symbols(*package);
            if (!symbols) {
                fail(GraphJitDiagnosticStage::package_llvm, std::move(symbols.error()));
            }
            bitcode->tick_blocks = std::move(*symbols);
            auto retained = graph_jit::package_retained_global_definitions(*package);
            if (!retained) {
                fail(GraphJitDiagnosticStage::package_llvm, std::move(retained.error()));
            }
            bitcode->retained_globals = std::move(*retained);
        }
        auto const& result = *bitcode;
        cache_.package_bitcode.emplace(path, CachedPackageBitcode{
            .revision = revision,
            .bitcode = std::move(bitcode),
        });
        return result;
    }

    std::pair<graph_jit::RegionSpecializationFunction, std::shared_ptr<ProjectCodeLifetime>>
    compile(graph_jit::RegionSpecializationRequest const& request)
    {
        using Clock = std::chrono::steady_clock;
        auto phase_started = Clock::now();
        std::uint64_t prepare_us = 0;
        std::uint64_t emit_link_us = 0;
        std::uint64_t optimization_us = 0;
        std::uint64_t materialization_us = 0;
        std::uint64_t retargeted_functions = 0;
        auto finish_phase = [&](std::uint64_t& destination) {
            auto const now = Clock::now();
            destination += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - phase_started).count());
            phase_started = now;
        };

        auto& jit = *shared_->jit;
        auto const name = "__iv_region_specialization";
        auto const suffix = std::to_string(cache_.next_name++);

        struct PackageLink {
            std::shared_ptr<PackageRevision const> revision{};
            PackageBitcode const* bitcode = nullptr;
            std::vector<std::pair<std::string, std::string>> tick_blocks{};
        };
        std::vector<PackageLink> packages;
        std::vector<std::string> tick_blocks;
        for (auto const& step : request.steps) {
            auto const& bitcode = package_bitcode(step.revision);
            auto const symbol = std::ranges::find(
                bitcode.tick_blocks,
                step.code_key,
                &std::pair<NodeCodeKey, std::string>::first);
            if (symbol == bitcode.tick_blocks.end()) {
                fail(
                    GraphJitDiagnosticStage::package_llvm,
                    "package bitcode has no tick_block for a specialized node");
            }
            auto package = std::ranges::find(
                packages, step.revision, &PackageLink::revision);
            if (package == packages.end()) {
                packages.push_back(PackageLink{
                    .revision = step.revision,
                    .bitcode = &bitcode,
                });
                package = std::prev(packages.end());
            }
            auto renamed = std::ranges::find(
                package->tick_blocks,
                symbol->second,
                &std::pair<std::string, std::string>::first);
            if (renamed == package->tick_blocks.end()) {
                auto unique = "__iv_region_tick_" + suffix + "_"
                    + std::to_string(std::distance(packages.begin(), package)) + "_"
                    + std::to_string(package->tick_blocks.size());
                package->tick_blocks.emplace_back(symbol->second, std::move(unique));
                renamed = std::prev(package->tick_blocks.end());
            }
            tick_blocks.push_back(renamed->second);
        }
        finish_phase(prepare_us);

        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("iv.region." + suffix, *context);
        module->setDataLayout(jit.getDataLayout());
        module->setTargetTriple(jit.getTargetTriple());
        auto emitted = graph_jit::emit_region_specialization(
            *module, request, name, tick_blocks);
        if (!emitted) {
            fail(GraphJitDiagnosticStage::lowering, std::move(emitted.error()));
        }
        auto const workspace_contents = std::move(*emitted);

        ImportedNodeCode imported;
        std::unordered_map<std::string, std::size_t> imported_global_sizes;
        {
            // The Linker tracks metadata of the module's context, so it must
            // be destroyed before the module and context move into ORC.
            llvm::Linker linker(*module);
            for (auto const& package_link : packages) {
                auto package = take_llvm_expected(
                    llvm::getOwningLazyBitcodeModule(
                        llvm::MemoryBuffer::getMemBuffer(
                            package_link.bitcode->buffer->getMemBufferRef(), false),
                        *context),
                    GraphJitDiagnosticStage::package_llvm,
                    "read package bitcode for a region specialization");

                auto const& accepted =
                    package_link.revision->node_code_retained_globals;
                auto const& definitions = package_link.bitcode->retained_globals;
                if (accepted.size() != definitions.size()) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "package bitcode and accepted node code disagree on retained globals");
                }
                for (std::size_t index = 0; index < accepted.size(); ++index) {
                    auto const& native = accepted[index];
                    auto const& definition = definitions[index];
                    if (!native.address) continue;
                    if (native.size != definition.size) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "package retained global changed size before specialization");
                    }
                    if (definition.symbol.empty()) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "accepted retained global has no package LLVM definition");
                    }
                    auto* global = package->getNamedGlobal(definition.symbol);
                    if (!global) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "package retained global disappeared before specialization");
                    }
                    if (auto error = global->materialize()) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "cannot materialize package retained global for specialization: "
                                + llvm_error_string(std::move(error)));
                    }
                    if (!global->isConstant() || !global->hasInitializer()
                        || global->isThreadLocal() || global->getAddressSpace() != 0
                        || native.alignment == 0
                        || !std::has_single_bit(native.alignment)) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "package retained global cannot be imported by specialization");
                    }
                    auto imported_name = imported.data(native.address);
                    auto const [imported_size, inserted] =
                        imported_global_sizes.try_emplace(imported_name, native.size);
                    if (!inserted && imported_size->second != native.size) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "retained globals at one native address disagree on size");
                    }
                    auto* declaration = package->getNamedGlobal(imported_name);
                    if (declaration) {
                        auto const size = package->getDataLayout().getTypeAllocSize(
                            declaration->getValueType());
                        if (!declaration->isDeclaration() || size.isScalable()
                            || size.getFixedValue() != native.size) {
                            fail(
                                GraphJitDiagnosticStage::package_llvm,
                                "retained-global import collides with a package LLVM symbol");
                        }
                    } else {
                        declaration = new llvm::GlobalVariable(
                            *package,
                            llvm::ArrayType::get(
                                llvm::Type::getInt8Ty(package->getContext()), native.size),
                            true,
                            llvm::GlobalValue::ExternalLinkage,
                            nullptr,
                            imported_name);
                        declaration->setAlignment(llvm::Align(native.alignment));
                    }
                    global->replaceAllUsesWith(declaration);
                    global->eraseFromParent();
                }

                for (auto const& [source, renamed] : package_link.tick_blocks) {
                    auto* callback = package->getFunction(source);
                    if (!callback) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "package tick_block changed before specialization linking");
                    }
                    if (auto error = callback->materialize()) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "cannot materialize package tick_block for specialization: "
                                + llvm_error_string(std::move(error)));
                    }
                    if (callback->isDeclaration()
                        || !graph_jit::package_callback_type_matches(
                            *callback, graph_jit::CallbackImportAbi::block)) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "package tick_block changed before specialization linking");
                    }
                    callback->setName(renamed);
                    callback->setLinkage(llvm::GlobalValue::ExternalLinkage);
                    callback->setVisibility(llvm::GlobalValue::DefaultVisibility);
                    callback->setComdat(nullptr);
                    if (callback->getName() != renamed) {
                        fail(
                            GraphJitDiagnosticStage::package_llvm,
                            "region tick_block symbol collides inside package bitcode");
                    }
                }
                for (auto const* list : {"llvm.global_ctors", "llvm.global_dtors",
                         "llvm.used", "llvm.compiler.used"}) {
                    if (auto* global = package->getNamedGlobal(list)) {
                        global->eraseFromParent();
                    }
                }
                for (auto& function : *package) {
                    if (function.isDeclaration()
                        || std::ranges::contains(
                            package_link.tick_blocks,
                            function.getName().str(),
                            &std::pair<std::string, std::string>::second)) {
                        continue;
                    }
                    function.setLinkage(llvm::GlobalValue::InternalLinkage);
                    function.setVisibility(llvm::GlobalValue::DefaultVisibility);
                    function.setComdat(nullptr);
                }
                for (auto& global : package->globals()) {
                    if (global.isDeclaration()) continue;
                    global.setLinkage(llvm::GlobalValue::InternalLinkage);
                    global.setVisibility(llvm::GlobalValue::DefaultVisibility);
                    global.setComdat(nullptr);
                }
                for (auto& alias : package->aliases()) {
                    alias.setLinkage(llvm::GlobalValue::InternalLinkage);
                    alias.setVisibility(llvm::GlobalValue::DefaultVisibility);
                }
                for (auto& ifunc : package->ifuncs()) {
                    ifunc.setLinkage(llvm::GlobalValue::InternalLinkage);
                    ifunc.setVisibility(llvm::GlobalValue::DefaultVisibility);
                }
                if (linker.linkInModule(
                        std::move(package), llvm::Linker::Flags::LinkOnlyNeeded)) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "cannot link package node code into a region specialization");
                }
            }
        }
        retargeted_functions =
            graph_jit::retarget_region_specialization_functions(
                *module,
                target_.getTargetCPU().str(),
                target_.getTargetFeatureString().str());
        graph_jit::internalize_region_specialization(*module, name);
        verify_module(
            *module,
            GraphJitDiagnosticStage::lowering,
            "region specialization LLVM is invalid");
        auto const profile_stem = "region-" + suffix;
        write_function_inventory(*module, profile_stem + "-before");
        write_module_ir(*module, profile_stem + "-before");
        finish_phase(emit_link_us);
        // The workspace's values reach node code once its callbacks are
        // inlined: a simplifying pass inlines them, then every read of a field
        // fixed for the call becomes that field's value. A substitution can
        // fix a loop's trip count, so the pair repeats until none remains.
        // These discovery rounds deliberately use LLVM's repeatable module
        // simplification pipeline. The complete optimization pipeline runs
        // once, after the final substitution.
        for (unsigned round = 0; round < 3; ++round) {
            optimize_project_module(
                *module,
                target_,
                profile_stem,
                llvm::OptimizationLevel::O1,
                true,
                ProjectModulePipeline::simplification);
            auto const replaced = graph_jit::substitute_region_workspace_values(
                *module, name, workspace_contents);
            timings_.region_specialization_substitutions += replaced;
            if (replaced == 0) break;
        }
        write_module_ir(*module, profile_stem + "-substituted");
        optimize_project_module(
            *module, target_, profile_stem, llvm::OptimizationLevel::O3, true);
        write_function_inventory(*module, profile_stem + "-after");
        write_module_ir(*module, profile_stem + "-after");
        verify_module(
            *module,
            GraphJitDiagnosticStage::optimization,
            "optimized region specialization LLVM is invalid");
        finish_phase(optimization_us);

        auto lifetime = std::make_shared<ProjectCodeLifetime>();
        lifetime->shared = shared_;
        for (auto const& package : packages) {
            lifetime->package_revisions.push_back(package.revision);
        }
        std::lock_guard lock(shared_->mutex);
        try {
            auto& jit_dylib = take_llvm_expected(
                jit.createJITDylib("iv.region." + suffix),
                GraphJitDiagnosticStage::materialization,
                "create region specialization JITDylib");
            lifetime->jit_dylib = &jit_dylib;
            lifetime->resources = jit_dylib.createResourceTracker();
            std::vector<PackageRevision const*> libraries;
            for (auto const& package : packages) {
                libraries.push_back(package.revision.get());
            }
            define_imported_node_code(
                jit,
                jit_dylib,
                lifetime->resources,
                imported,
                "region specialization");
            add_project_code_generators(jit, jit_dylib, libraries);
            check_llvm_error(
                jit.addIRModule(
                    lifetime->resources,
                    llvm::orc::ThreadSafeModule(
                        std::move(module),
                        llvm::orc::ThreadSafeContext(std::move(context)))),
                GraphJitDiagnosticStage::materialization,
                "add region specialization LLVM to ORC JIT");
            auto address = take_llvm_expected(
                jit.lookup(jit_dylib, name),
                GraphJitDiagnosticStage::materialization,
                "lookup region specialization");
            lifetime->allocated_code_bytes = shared_->allocated_code_sizes
                ? shared_->allocated_code_sizes->allocated_bytes(lifetime->resources)
                : 0;
            finish_phase(materialization_us);
            timings_.region_specialization_prepare_us += prepare_us;
            timings_.region_specialization_emit_link_us += emit_link_us;
            timings_.region_specialization_optimization_us += optimization_us;
            timings_.region_specialization_materialization_us += materialization_us;
            timings_.region_specialization_retargeted_functions +=
                retargeted_functions;
            return {
                address.toPtr<graph_jit::RegionSpecializationFunction>(),
                std::move(lifetime),
            };
        } catch (...) {
            lifetime->release_locked();
            throw;
        }
    }

public:
    // current_revisions are the package revisions of the compilation's
    // definitions. The cache stops owning code of any other revision before
    // this compilation requests specializations.
    RegionSpecializationCompiler(
        std::shared_ptr<SharedProjectJit> const& shared,
        llvm::TargetMachine& target,
        RegionSpecializationCache& cache,
        GraphJitCompileTimings& timings,
        std::span<std::shared_ptr<PackageRevision const> const> current_revisions)
        : shared_(shared)
        , target_(target)
        , cache_(cache)
        , timings_(timings)
    {
        cache_.stop_retaining_superseded(current_revisions);
        cache_.remove_expired();
        timings_.region_specialization_cache_bytes = cache_.retained_bytes;
        timings_.region_specialization_cache_budget_bytes = cache_.budget_bytes;
    }

    void region_kept_on_interpreter(std::string const& reason) override
    {
        ++timings_.region_specializations_skipped;
        if (timings_.region_specialization_skip.empty()) {
            timings_.region_specialization_skip = reason;
        }
    }

    std::optional<graph_jit::RegionSpecializationCode> region_specialization(
        graph_jit::RegionSpecializationRequest const& request) override
    {
        auto key = graph_jit::region_specialization_key(request);
        if (key.empty()) {
            region_kept_on_interpreter("the specialization request is incomplete");
            return std::nullopt;
        }
        if (auto const failed = failures_.find(key); failed != failures_.end()) {
            ++timings_.region_specialization_failures;
            if (timings_.region_specialization_failure.empty()) {
                timings_.region_specialization_failure = failed->second;
            }
            return std::nullopt;
        }
        if (auto const cached = cache_.entries.find(key); cached != cache_.entries.end()) {
            if (auto lifetime = cached->second.lifetime.lock()) {
                cache_.retain(key, cached->second, lifetime);
                timings_.region_specialization_cache_bytes = cache_.retained_bytes;
                ++timings_.region_specializations_reused;
                return graph_jit::RegionSpecializationCode{
                    .function = cached->second.function,
                    .lifetime = std::move(lifetime),
                };
            }
            cache_.entries.erase(cached);
        }
        using Clock = std::chrono::steady_clock;
        auto const begin = Clock::now();
        try {
            auto [function, lifetime] = compile(request);
            timings_.region_specialization_us += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - begin).count());
            timings_.region_specialization_allocated_bytes +=
                lifetime->allocated_code_bytes;
            ++timings_.region_specializations_compiled;
            auto [cached, inserted] = cache_.entries.try_emplace(key);
            if (!inserted) cache_.stop_retaining(cached->second);
            cached->second = CachedRegionSpecialization{
                .lifetime = lifetime,
                .function = function,
                .allocated_code_bytes = lifetime->allocated_code_bytes,
            };
            cache_.retain(key, cached->second, lifetime);
            timings_.region_specialization_cache_bytes = cache_.retained_bytes;
            return graph_jit::RegionSpecializationCode{
                .function = function,
                .lifetime = std::move(lifetime),
            };
        } catch (GraphJitCompileError const& error) {
            ++timings_.region_specialization_failures;
            if (timings_.region_specialization_failure.empty()) {
                timings_.region_specialization_failure = error.what();
            }
            failures_.try_emplace(std::move(key), error.what());
            return std::nullopt;
        }
    }
};
} // namespace

class GraphJit::Impl {
    struct TransitionCompilation {
        std::shared_ptr<CompiledGraph const> active_predecessor{};
        CompiledGraphRealtimePhase active_predecessor_phase =
            CompiledGraphRealtimePhase::steady;
    };

    GraphJitKernelSpecialization specialization_{};
    RealtimeStorageCostModel realtime_storage_cost_model_{};
    bool specialize_regions_ = true;
    std::unique_ptr<llvm::TargetMachine> optimization_target_{};
    std::shared_ptr<SharedProjectJit> shared_jit_{};
    // Accessed only under compile_mutex_.
    RegionSpecializationCache region_specializations_{};
    std::mutex compile_mutex_{};

public:
    explicit Impl(GraphJitConfig config)
    {
        if (config.sample_rate == 0) {
            throw std::invalid_argument("GraphJit sample rate must be non-zero");
        }
        if (!is_valid_block_size(config.block_size)) {
            throw std::invalid_argument(
                "GraphJit block size must be a valid Intravenous block size");
        }

        realtime_storage_cost_model_ = config.realtime_storage_cost_model;
        specialize_regions_ = config.specialize_regions;
        region_specializations_.budget_bytes =
            config.region_specialization_cache_budget_bytes;

        initialize_graph_jit_target();
        auto target = take_llvm_expected(
            llvm::orc::JITTargetMachineBuilder::detectHost(),
            GraphJitDiagnosticStage::materialization,
            "detect project ORC target");
        target.setCodeGenOptLevel(project_codegen_level());
        specialization_ = GraphJitKernelSpecialization{
            .sample_rate = config.sample_rate,
            .block_size = config.block_size,
            .target_triple = target.getTargetTriple().str(),
            .target_cpu = target.getCPU(),
            .target_features = target.getFeatures().getString(),
        };
        optimization_target_ = take_llvm_expected(
            target.createTargetMachine(),
            GraphJitDiagnosticStage::materialization,
            "create project optimization target machine");
        auto jit = take_llvm_expected(
            llvm::orc::LLJITBuilder()
                .setJITTargetMachineBuilder(std::move(target))
                .create(),
            GraphJitDiagnosticStage::materialization,
            "create project ORC JIT");
        check_llvm_error(
            details::enable_orc_perf_support_if_requested(*jit),
            GraphJitDiagnosticStage::materialization,
            "enable project ORC perf support");
        auto* object_layer =
            llvm::dyn_cast<llvm::orc::ObjectLinkingLayer>(
                &jit->getObjLinkingLayer());
        if (!object_layer) {
            fail(
                GraphJitDiagnosticStage::materialization,
                "region specialization code sizing requires LLVM's JITLink ObjectLinkingLayer");
        }
        auto allocated_code_sizes = std::make_shared<AllocatedCodeSizePlugin>();
        object_layer->addPlugin(allocated_code_sizes);
        shared_jit_ = std::make_shared<SharedProjectJit>();
        shared_jit_->jit = std::move(jit);
        shared_jit_->allocated_code_sizes = std::move(allocated_code_sizes);
    }

    GraphJitKernelSpecialization const& specialization() const noexcept
    {
        return specialization_;
    }

    GraphJitCompileResult compile_impl(
        GraphJitCompileRequest const& request,
        TransitionCompilation const* transition)
    {
        std::lock_guard compile_lock(compile_mutex_);
        using Clock = std::chrono::steady_clock;
        auto const microseconds_since = [](Clock::time_point begin) {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - begin).count());
        };
        auto const compile_begin = Clock::now();
        GraphJitCompileTimings timings{.transition = transition != nullptr};
        GraphJitCompileResult result{.attempted = true};
        auto captured = capture_inputs(request);
        timings.capture_us = microseconds_since(compile_begin);
        if (!captured.diagnostics.empty()) {
            result.diagnostics = std::move(captured.diagnostics);
            return result;
        }

        // Each used revision carries its node code, compiled once per package
        // build. A compilation only names the addresses it imports.
        std::vector<std::shared_ptr<PackageRevision const>> package_revisions;
        package_revisions.reserve(captured.used_revisions.size());
        std::unordered_map<PackageRevision const*, std::size_t> package_by_revision;
        for (auto const& revision : request.definitions->package_revisions) {
            if (!revision || !captured.used_revisions.contains(revision.get())) continue;
            package_by_revision.emplace(revision.get(), package_revisions.size());
            package_revisions.push_back(revision);
        }
        ImportedNodeCode imported;
        std::vector<std::vector<graph_jit::PackageRetainedGlobal>> retained_globals(
            package_revisions.size());
        std::vector<graph_jit::PackageCode> package_inputs;
        package_inputs.reserve(package_revisions.size());
        for (std::size_t index = 0; index < package_revisions.size(); ++index) {
            auto& globals = retained_globals[index];
            for (auto const& global : package_revisions[index]->node_code_retained_globals) {
                if (!global.address) {
                    globals.emplace_back();
                    continue;
                }
                globals.push_back(graph_jit::PackageRetainedGlobal{
                    .symbol = imported.data(global.address),
                    .address = global.address,
                    .size = global.size,
                    .alignment = global.alignment,
                });
            }
            package_inputs.push_back(graph_jit::PackageCode{
                .revision = package_revisions[index],
                .retained_globals = globals,
            });
        }
        auto context = std::make_unique<llvm::LLVMContext>();

        std::vector<graph_jit::NodeImplementation> implementations;
        implementations.reserve(captured.nodes.size());
        try {
            for (auto const& captured_node : captured.nodes) {
                auto const package = package_by_revision.find(captured_node.revision.get());
                if (package == package_by_revision.end()) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "internal GraphJit error: provider package is not a used revision");
                }
                auto const& accepted = captured_node.compiler_record;
                auto const* record = node_code_record(*captured_node.revision, accepted.code_key);
                if (!record) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "package node code has no record for node '"
                            + captured_node.identity.node_type_id + "'");
                }
                verify_compiler_record(captured_node, *record);
                auto const& code = record->operations;
                implementations.push_back(graph_jit::NodeImplementation{
                    .node_bundle = captured_node.node_bundle,
                    .identity = captured_node.identity,
                    .code_key = accepted.code_key,
                    .revision = captured_node.revision,
                    .provider_version = captured_node.provider_version,
                    .configuration_identity =
                        captured_node.configuration_identity,
                    .package = &package_inputs[package->second],
                    .state_size = accepted.state_size,
                    .state_alignment = accepted.state_alignment,
                    .background_state_size = accepted.background_state_size,
                    .background_state_alignment = accepted.background_state_alignment,
                    .intrinsically_replayable = accepted.intrinsically_replayable,
                    .pointwise_tick = accepted.pointwise_tick,
                    .block_input_access = accepted.block_input_access,
                    .node_data = captured_node.node_data,
                    .state_structures = captured_node.state_structures,
                    .declare_node = captured_node.declare_node,
                    .tick_block = imported.callback(code.tick_block),
                    .tick_block_batch = imported.callback(code.tick_block_batch),
                    .skip_block = imported.callback(code.skip_block),
                    .skip_block_batch = imported.callback(code.skip_block_batch),
                    .tock_coverage = imported.callback(code.tock_coverage),
                    .tock_coverage_batch = imported.callback(code.tock_coverage_batch),
                    .propagate_forward_coverage =
                        imported.callback(code.propagate_forward_coverage),
                    .propagate_forward_coverage_batch =
                        imported.callback(code.propagate_forward_coverage_batch),
                    .propagate_reverse_coverage =
                        imported.callback(code.propagate_reverse_coverage),
                    .propagate_reverse_coverage_batch =
                        imported.callback(code.propagate_reverse_coverage_batch),
                    .initialize_sequential_input =
                        imported.callback(code.initialize_sequential_input),
                    .combine_sequential_input_values =
                        imported.callback(code.combine_sequential_input_values),
                    .native_operations = code,
                    .host_initialize_sequential_input =
                        accepted.operations.initialize_sequential_input,
                });
            }
        } catch (GraphJitCompileError const& error) {
            result.diagnostics.push_back(diagnostic(error.stage, error.what()));
            return result;
        }

        std::vector<graph_jit::ConfigRelocation> relocations;
        relocations.reserve(captured.relocations.size());
        try {
            for (auto const& captured_relocation : captured.relocations) {
                if (!captured_relocation.relocation->retained_global_index) {
                    relocations.push_back(graph_jit::ConfigRelocation{
                        .node_bundle = captured_relocation.node_bundle,
                        .relocation = captured_relocation.relocation,
                    });
                    continue;
                }
                auto const package = package_by_revision.find(
                    captured_relocation.revision.get());
                if (package == package_by_revision.end()) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "internal GraphJit error: relocation package is not a used revision");
                }
                auto const& globals = retained_globals[package->second];
                auto const index = *captured_relocation.relocation->retained_global_index;
                if (index >= globals.size()) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "package node code is missing a configuration relocation global");
                }
                relocations.push_back(graph_jit::ConfigRelocation{
                    .node_bundle = captured_relocation.node_bundle,
                    .relocation = captured_relocation.relocation,
                    .revision = captured_relocation.revision,
                    .retained_global = &globals[index],
                });
            }
        } catch (GraphJitCompileError const& error) {
            result.diagnostics.push_back(diagnostic(error.stage, error.what()));
            return result;
        }

        auto output = std::make_unique<llvm::Module>(
            "iv.project." + std::to_string(request.project_generation), *context);
        output->setDataLayout(shared_jit_->jit->getDataLayout());
        output->setTargetTriple(shared_jit_->jit->getTargetTriple());

        std::optional<graph_jit::LoweringTransitionInput> transition_input;
        // A first generation starts from the empty graph, which owns no state.
        static graph_jit::RealtimePortStateRequirements const
            empty_graph_requirements{};
        static graph_jit::RealtimePortStateRealizations const
            empty_graph_realizations{};
        if (transition != nullptr && !transition->active_predecessor) {
            transition_input.emplace(graph_jit::LoweringTransitionInput{
                .predecessor_requirements = empty_graph_requirements,
                .predecessor_realizations = empty_graph_realizations,
            });
        } else if (transition != nullptr) {
            auto const* predecessor_realizations =
                &transition->active_predecessor
                    ->realtime_port_state_realizations;
            if (transition->active_predecessor_phase
                == CompiledGraphRealtimePhase::transition) {
                auto const& predecessor_cutover =
                    transition->active_predecessor->cutover;
                if (!predecessor_cutover
                    || predecessor_cutover->state_transition
                        .sample_expiry_cursors.empty()
                    || !transition->active_predecessor->root_operations
                        .transition_tick_block) {
                    result.diagnostics.push_back(diagnostic(
                        GraphJitDiagnosticStage::input_capture,
                        "active predecessor has no transition realization"));
                    return result;
                }
                predecessor_realizations =
                    &predecessor_cutover->transition_realizations;
            }
            transition_input.emplace(graph_jit::LoweringTransitionInput{
                .predecessor_requirements = transition->active_predecessor
                    ->realtime_port_state_requirements,
                .predecessor_realizations = *predecessor_realizations,
            });
        }

        // Every accepted revision of the definition world, not only the ones
        // this graph uses: code of a package that the graph stopped using can
        // still be requested again by a later edit.
        RegionSpecializationCompiler region_specializations(
            shared_jit_,
            *optimization_target_,
            region_specializations_,
            timings,
            request.definitions->package_revisions);
        graph_jit::LoweringInput lowering_input{
            .graph = *request.graph,
            .specialization = specialization_,
            .realtime_storage_cost_model = realtime_storage_cost_model_,
            .packages = package_inputs,
            .node_implementations = implementations,
            .config_relocations = relocations,
            .transition = transition_input ? &*transition_input : nullptr,
            .region_specializations =
                specialize_regions_ ? &region_specializations : nullptr,
        };
        auto lowering = graph_jit::lower_configured_graph_to_llvm(lowering_input, *output);
        if (!lowering) {
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::lowering,
                std::move(lowering.error())));
            return result;
        }

        timings.lowering_plan_us = lowering->plan_us;
        timings.lowering_transition_us = lowering->transition_us;
        timings.lowering_emit_us = lowering->emit_us;
        timings.realtime_storage = lowering->realtime_storage;

        try {
            auto const verify_begin = Clock::now();
            validate_lowering_output(*output, *lowering);
            verify_module(
                *output,
                GraphJitDiagnosticStage::lowering,
                "graph lowerer produced invalid LLVM");
            timings.verify_us = microseconds_since(verify_begin);
            for (auto const& function : *output) {
                if (!function.isDeclaration()) ++timings.functions;
            }
            timings.instructions_before_optimization = output->getInstructionCount();
            auto const inventory_stem = "g" + std::to_string(request.project_generation)
                + (transition ? "-transition" : "-steady");
            write_function_inventory(*output, inventory_stem + "-before");
            write_module_ir(*output, inventory_stem + "-before");
            auto const optimization_begin = Clock::now();
            optimize_project_module(*output, *optimization_target_, inventory_stem);
            timings.optimization_us = microseconds_since(optimization_begin);
            timings.instructions_after_optimization = output->getInstructionCount();
            write_function_inventory(*output, inventory_stem + "-after");
            write_module_ir(*output, inventory_stem + "-after");
            auto const second_verify_begin = Clock::now();
            verify_module(
                *output,
                GraphJitDiagnosticStage::optimization,
                "optimized project LLVM is invalid");
            timings.verify_us += microseconds_since(second_verify_begin);

            // With profiling output on, time code generation's legacy passes.
            auto const codegen_passes_path =
                profile_path(inventory_stem, "-codegen-passes.txt");
            if (codegen_passes_path) llvm::TimePassesIsEnabled = true;
            auto const materialization_begin = Clock::now();
            auto materialized = materialize_project_module(
                shared_jit_,
                std::move(output),
                std::move(context),
                package_revisions,
                imported,
                lowering->root_symbols,
                lowering->realtime_root,
                lowering->transition_realtime_root,
                request.project_generation);
            timings.materialization_us = microseconds_since(materialization_begin);
            if (codegen_passes_path) {
                llvm::TimePassesIsEnabled = false;
                std::error_code error;
                llvm::raw_fd_ostream out(codegen_passes_path->string(), error);
                if (!error) llvm::reportAndResetTimings(&out);
            }
            std::optional<CompiledGraphCutover> cutover;
            if (lowering->transition) {
                if (transition == nullptr) {
                    fail(
                        GraphJitDiagnosticStage::lowering,
                        "GraphJit transition lowering lost its compiler input");
                }
                auto const from_empty_graph = !transition->active_predecessor;
                auto const& state = lowering->transition->state_transition;
                // A first generation needs no cutover when NodeStorage
                // initialization already gives every window its fresh value:
                // no state needs transition-only storage, and every fresh-only
                // transfer writes its region's declared initial value, or zero
                // bytes into a region that declares none.
                auto const initialized_by_storage = from_empty_graph
                    && state.sample_expiry_cursors.empty()
                    && state.event_expiry_samples == 0
                    && state.event_transfers.empty()
                    && std::ranges::all_of(
                        state.sample_transfers,
                        [](graph_jit::SampleRealtimePortStateTransfer const& transfer) {
                            auto const& initial =
                                transfer.target_storage.initial_value;
                            return initial
                                ? *initial == transfer.fresh_value
                                : std::ranges::all_of(
                                    transfer.fresh_value,
                                    [](std::byte value) {
                                        return value == std::byte{0};
                                    });
                        });
                if (!initialized_by_storage) cutover.emplace(CompiledGraphCutover{
                    .from_empty_graph = from_empty_graph,
                    .predecessor = transition->active_predecessor,
                    .predecessor_phase =
                        transition->active_predecessor_phase,
                    .state_transition = std::move(
                        lowering->transition->state_transition),
                    .transition_realizations = std::move(
                        lowering->transition
                            ->realtime_port_state_realizations),
                });
            }
            result.compiled_graph = std::make_shared<CompiledGraph>(CompiledGraph{
                .project_generation = request.project_generation,
                .definitions_generation = request.definitions->generation,
                .specialization = specialization_,
                .configured_graph = request.graph,
                .package_revisions = std::move(package_revisions),
                .port_realization = std::move(lowering->port_realization),
                .node_layout = std::move(lowering->node_layout),
                .tock_state_layout = std::move(lowering->tock_state_layout),
                .background_evaluation_plan = std::move(lowering->background_evaluation_plan),
                .realtime_port_state_requirements =
                    std::move(lowering->realtime_port_state_requirements),
                .realtime_port_state_realizations =
                    std::move(lowering->realtime_port_state_realizations),
                .manual_controls = std::move(lowering->manual_controls),
                .cutover = std::move(cutover),
                .root_operations = materialized.root_operations,
                .background_operations = materialized.background_operations,
                .code_lifetime = std::move(materialized.lifetime),
            });
        } catch (GraphJitCompileError const& error) {
            result.diagnostics.push_back(diagnostic(error.stage, error.what()));
        }
        timings.total_us = microseconds_since(compile_begin);
        result.timings.push_back(timings);
        return result;
    }

    GraphJitCompileResult compile(GraphJitCompileRequest const& request)
    {
        return compile_impl(request, nullptr);
    }

    // The executor's first generation: a cutover from the empty graph, so
    // that every Sequential input starts at its fresh values even where two
    // of them disagree in one region. Without such a need the result is the
    // steady artifact.
    GraphJitCompileResult compile_initial_generation(
        GraphJitCompileRequest const& request)
    {
        TransitionCompilation const from_empty_graph{};
        return compile_impl(request, &from_empty_graph);
    }

    // The ProjectGraph edit path with an accepted predecessor: one compilation
    // whose module defines both the transition root and the steady root that
    // follows it. A separate steady compilation would be discarded; its layout
    // equals the steady plan this compilation builds first.
    GraphJitCompileResult compile_cutover(
        GraphJitCompileRequest const& successor,
        std::shared_ptr<CompiledGraph const> const& predecessor)
    {
        if (predecessor->project_generation >= successor.project_generation) {
            GraphJitCompileResult result{.attempted = true};
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::input_capture,
                "transition compilation predecessor is not older than its successor"));
            return result;
        }
        TransitionCompilation transition{
            .active_predecessor = predecessor,
            .active_predecessor_phase = CompiledGraphRealtimePhase::steady,
        };
        return compile_impl(successor, &transition);
    }

    GraphJitCompileResult compile_transition(
        GraphJitTransitionCompileRequest const& request)
    {
        GraphJitCompileResult result{.attempted = true};
        auto const& successor = request.successor;
        if (!request.steady_successor || !request.active_predecessor) {
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::input_capture,
                "transition compilation requires steady successor and active predecessor artifacts"));
            return result;
        }
        if (request.steady_successor->cutover
            || request.steady_successor->project_generation
                != successor.project_generation
            || request.steady_successor->configured_graph != successor.graph
            || !successor.definitions
            || request.steady_successor->definitions_generation
                != successor.definitions->generation) {
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::input_capture,
                "transition compilation successor does not match its steady artifact"));
            return result;
        }
        if (request.active_predecessor->project_generation
            >= successor.project_generation) {
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::input_capture,
                "transition compilation predecessor is not older than its successor"));
            return result;
        }
        TransitionCompilation transition{
            .active_predecessor = request.active_predecessor,
            .active_predecessor_phase =
                request.active_predecessor_phase,
        };
        auto compiled = compile_impl(successor, &transition);
        if (compiled.compiled_graph
            && compiled.compiled_graph->node_layout.steady_storage_size
                != request.steady_successor->node_layout.storage_size) {
            compiled.compiled_graph.reset();
            compiled.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::lowering,
                "transition specialization changed the steady NodeStorage extent"));
        }
        return compiled;
    }
};

GraphJit::GraphJit(GraphJitConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{}

GraphJit::~GraphJit() = default;
GraphJit::GraphJit(GraphJit&&) noexcept = default;
GraphJit& GraphJit::operator=(GraphJit&&) noexcept = default;

GraphJitKernelSpecialization const& GraphJit::specialization() const noexcept
{
    return impl_->specialization();
}

GraphJitCompileResult GraphJit::compile(GraphJitCompileRequest const& request)
{
    return impl_->compile(request);
}

GraphJitCompileResult GraphJit::compile_transition(
    GraphJitTransitionCompileRequest const& request)
{
    return impl_->compile_transition(request);
}

GraphJitCompileResult GraphJit::handle_project_graph_transaction(
    ProjectGraphGraphJitRequest const& request)
{
    if (!request.predecessor) {
        return request.initial_generation
            ? impl_->compile_initial_generation(request.successor)
            : compile(request.successor);
    }
    return impl_->compile_cutover(request.successor, request.predecessor);
}
} // namespace iv
