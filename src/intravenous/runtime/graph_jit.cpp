#include <intravenous/runtime/graph_jit.h>

#include <intravenous/graph_jit/lowering.h>
#include <intravenous/ports.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
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

struct NodeCodeKeyHash {
    std::size_t operator()(NodeCodeKey const& key) const noexcept
    {
        auto const a = static_cast<std::size_t>(key.low ^ (key.low >> 32));
        auto const b = static_cast<std::size_t>(key.high ^ (key.high >> 32));
        return a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
    }
};

struct IrNodeRecord {
    llvm::Function* tick_block = nullptr;
    llvm::Function* skip_block = nullptr;
    llvm::Function* tock_coverage = nullptr;
    llvm::Function* propagate_forward_coverage = nullptr;
    llvm::Function* propagate_reverse_coverage = nullptr;
    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
    std::size_t background_state_size = 0;
    std::size_t background_state_alignment = 1;
    bool intrinsically_replayable = false;
};

std::uint64_t constant_u64(llvm::Value const* value, std::string_view field)
{
    auto const* integer = llvm::dyn_cast<llvm::ConstantInt>(value);
    if (!integer) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "malformed iv_node_types " + std::string(field));
    }
    return integer->getZExtValue();
}

std::size_t constant_size(llvm::Value const* value, std::string_view field)
{
    auto const result = constant_u64(value, field);
    if (result > std::numeric_limits<std::size_t>::max()) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "iv_node_types " + std::string(field) + " does not fit size_t");
    }
    return static_cast<std::size_t>(result);
}

llvm::Function* compiler_callback(
    llvm::Value* value,
    std::string_view field,
    bool required)
{
    if (llvm::isa<llvm::ConstantPointerNull>(value)) {
        if (required) {
            fail(
                GraphJitDiagnosticStage::package_llvm,
                "iv_node_types has null required callback " + std::string(field));
        }
        return nullptr;
    }
    auto* constant = llvm::dyn_cast<llvm::Constant>(value);
    auto* function = constant
        ? llvm::dyn_cast<llvm::Function>(constant->stripPointerCasts())
        : nullptr;
    if (!function) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "iv_node_types callback " + std::string(field)
                + " does not reference an LLVM function");
    }
    return function;
}

std::unordered_map<NodeCodeKey, IrNodeRecord, NodeCodeKeyHash> scan_node_records(
    llvm::Module& module)
{
    std::unordered_map<NodeCodeKey, IrNodeRecord, NodeCodeKeyHash> result;
    for (auto& global : module.globals()) {
        auto const section = global.getSection();
        if (section != "iv_node_types" && !section.ends_with("__iv_node_types")) {
            continue;
        }
        auto* record = llvm::dyn_cast_or_null<llvm::ConstantStruct>(global.getInitializer());
        if (!record || record->getNumOperands() != 9) {
            fail(GraphJitDiagnosticStage::package_llvm, "malformed iv_node_types record");
        }
        auto* key = llvm::dyn_cast<llvm::ConstantStruct>(record->getOperand(0));
        auto* operations = llvm::dyn_cast<llvm::ConstantStruct>(record->getOperand(1));
        if (!key || key->getNumOperands() != 2 || !operations
            || operations->getNumOperands() != 6) {
            fail(GraphJitDiagnosticStage::package_llvm, "malformed iv_node_types record ABI");
        }
        NodeCodeKey code_key{
            .low = constant_u64(key->getOperand(0), "code key low"),
            .high = constant_u64(key->getOperand(1), "code key high"),
        };
        // Configuration-only declare_node must exist in the compiler record,
        // but it is deliberately not exposed to whole-project lowering.
        (void)compiler_callback(operations->getOperand(0), "declare_node", true);
        auto const replay_marker = constant_u64(
            record->getOperand(8), "intrinsic replayability");
        if (replay_marker > 1) {
            fail(GraphJitDiagnosticStage::package_llvm,
                "malformed iv_node_types intrinsic replayability flag");
        }
        IrNodeRecord parsed{
            .tick_block = compiler_callback(operations->getOperand(1), "tick_block", true),
            .skip_block = compiler_callback(operations->getOperand(2), "skip_block", true),
            .tock_coverage = compiler_callback(
                operations->getOperand(3), "tock_coverage", false),
            .propagate_forward_coverage = compiler_callback(
                operations->getOperand(4), "propagate_forward_coverage", false),
            .propagate_reverse_coverage = compiler_callback(
                operations->getOperand(5), "propagate_reverse_coverage", false),
            .state_size = constant_size(record->getOperand(4), "state size"),
            .state_alignment = constant_size(record->getOperand(5), "state alignment"),
            .background_state_size = constant_size(
                record->getOperand(6), "background state size"),
            .background_state_alignment = constant_size(
                record->getOperand(7), "background state alignment"),
            .intrinsically_replayable =
                replay_marker != 0,
        };
        if (!result.emplace(code_key, parsed).second) {
            fail(GraphJitDiagnosticStage::package_llvm, "duplicate NodeCodeKey in iv_node_types");
        }
    }
    return result;
}

std::vector<llvm::GlobalVariable*> scan_retained_globals(
    llvm::Module& module,
    PackageRevision const& revision)
{
    auto* table = module.getNamedGlobal("iv.package_retained_globals");
    if (revision.retained_globals.empty()) {
        if (!table) return {};
        auto const* array = llvm::dyn_cast_or_null<llvm::ConstantArray>(table->getInitializer());
        if (!array || array->getNumOperands() == 0) return {};
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "package LLVM retained-global table disagrees with accepted revision");
    }
    if (!table || !table->hasInitializer()) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "package LLVM is missing iv.package_retained_globals");
    }
    auto* array = llvm::dyn_cast<llvm::ConstantArray>(table->getInitializer());
    if (!array || array->getNumOperands() != revision.retained_globals.size()) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "package LLVM retained-global count disagrees with accepted revision");
    }

    std::vector<llvm::GlobalVariable*> result(array->getNumOperands(), nullptr);
    for (unsigned record_index = 0; record_index < array->getNumOperands(); ++record_index) {
        auto* entry = llvm::dyn_cast<llvm::ConstantStruct>(array->getOperand(record_index));
        if (!entry || entry->getNumOperands() != 3) {
            fail(GraphJitDiagnosticStage::package_llvm, "malformed retained-global record");
        }
        auto const size = constant_size(entry->getOperand(1), "retained-global size");
        auto const index = constant_size(entry->getOperand(2), "retained-global index");
        if (index >= result.size() || result[index] != nullptr) {
            fail(GraphJitDiagnosticStage::package_llvm, "invalid retained-global index");
        }
        auto* address = llvm::dyn_cast<llvm::Constant>(entry->getOperand(0));
        auto* global = address
            ? llvm::dyn_cast<llvm::GlobalVariable>(address->stripPointerCasts())
            : nullptr;
        if (!global) {
            fail(
                GraphJitDiagnosticStage::package_llvm,
                "retained-global record does not reference an LLVM global");
        }
        auto const& accepted = revision.retained_globals[index];
        if (accepted.index != index || accepted.size != size) {
            fail(
                GraphJitDiagnosticStage::package_llvm,
                "retained-global ABI disagrees with accepted revision");
        }
        result[index] = global;
    }
    return result;
}

struct ParsedPackage {
    std::shared_ptr<PackageRevision const> revision{};
    std::unique_ptr<llvm::Module> module{};
    std::unordered_map<NodeCodeKey, IrNodeRecord, NodeCodeKeyHash> node_records{};
    std::vector<llvm::GlobalVariable*> retained_globals{};
};

ParsedPackage parse_package_module(
    std::shared_ptr<PackageRevision const> revision,
    llvm::LLVMContext& context,
    llvm::DataLayout const& target_data_layout,
    llvm::Triple const& target_triple)
{
    if (!revision) {
        fail(GraphJitDiagnosticStage::input_capture, "null package revision");
    }
    auto const& path = revision->compiler_artifact.bitcode_path;
    if (path.empty()) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "accepted package revision has no finalized compiler artifact");
    }
    auto buffer = llvm::MemoryBuffer::getFile(path.string());
    if (!buffer) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "cannot read finalized IV package LLVM '" + path.string()
                + "': " + buffer.getError().message());
    }
    auto module = take_llvm_expected(
        llvm::parseBitcodeFile((*buffer)->getMemBufferRef(), context),
        GraphJitDiagnosticStage::package_llvm,
        "parse finalized IV package LLVM '" + path.string() + "'");
    if (module->getDataLayout().isDefault()) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "finalized IV package LLVM '" + path.string() + "' has no data layout");
    }
    if (module->getDataLayout() != target_data_layout) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "finalized IV package LLVM '" + path.string()
                + "' data layout does not match the project JIT target");
    }
    if (!module->getTargetTriple().empty()
        && llvm::Triple(module->getTargetTriple()) != target_triple) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "finalized IV package LLVM '" + path.string()
                + "' target triple does not match the project JIT target");
    }
    auto node_records = scan_node_records(*module);
    auto retained_globals = scan_retained_globals(*module, *revision);
    return ParsedPackage{
        .revision = std::move(revision),
        .module = std::move(module),
        .node_records = std::move(node_records),
        .retained_globals = std::move(retained_globals),
    };
}

struct CapturedNode {
    std::size_t node_bundle = 0;
    std::shared_ptr<PackageRevision const> revision{};
    details::NodeCompilerRecord compiler_record{};
    RegisteredNodeTypeIdentity identity{};
    void const* node_data = nullptr;
    NodeStateStructures const* state_structures = nullptr;
    std::size_t (*declare_node)(
        void const*, NodeStateStructures const*, NodeLayoutBuilder&) = nullptr;
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
                            auto const expected_state_structures = std::find_if(
                                revision->second->node_state_structures.begin(),
                                revision->second->node_state_structures.end(),
                                [&](details::BuilderNodeStateStructures const& candidate) {
                                    return candidate.code_key
                                        == leaf->compiler_record.code_key;
                                });
                            auto const* configured_state_structures =
                                view.operations->runtime.state_structures;
                            auto const state_structures_match =
                                expected_state_structures
                                    == revision->second->node_state_structures.end()
                                ? configured_state_structures == nullptr
                                : configured_state_structures != nullptr
                                    && *configured_state_structures
                                        == expected_state_structures->structures;
                            if (!state_structures_match) {
                                result.diagnostics.push_back(diagnostic(
                                    GraphJitDiagnosticStage::input_capture,
                                    "configured node State/TockState metadata disagrees "
                                    "with pinned package revision",
                                    current_bundle,
                                    identity.node_type_id,
                                    identity.provider_package_root));
                                return;
                            }
                            result.nodes.push_back(CapturedNode{
                                .node_bundle = current_bundle,
                                .revision = revision->second,
                                .compiler_record = leaf->compiler_record,
                                .identity = identity,
                                .node_data = view.operations->runtime.node_data,
                                .state_structures =
                                    view.operations->runtime.state_structures,
                                .declare_node = view.operations->runtime.declare_node,
                            });
                            result.used_revisions.insert(revision->second.get());
                        }
                    }
                }
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

void verify_compiler_record(
    CapturedNode const& captured,
    IrNodeRecord const& ir_record)
{
    auto const& accepted = captured.compiler_record;
    if (!accepted.operations.valid()) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "accepted NodeCompilerRecord has incomplete runtime callbacks for node '"
                + captured.identity.node_type_id + "'");
    }
    if (accepted.state_size != ir_record.state_size
        || accepted.state_alignment != ir_record.state_alignment
        || accepted.background_state_size != ir_record.background_state_size
        || accepted.background_state_alignment != ir_record.background_state_alignment
        || accepted.intrinsically_replayable != ir_record.intrinsically_replayable) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "NodeCompilerRecord state/replay ABI disagrees with retained package LLVM for node '"
                + captured.identity.node_type_id + "'");
    }
    if (static_cast<bool>(accepted.operations.tock_coverage)
            != static_cast<bool>(ir_record.tock_coverage)
        || static_cast<bool>(accepted.operations.propagate_forward_coverage)
            != static_cast<bool>(ir_record.propagate_forward_coverage)
        || static_cast<bool>(accepted.operations.propagate_reverse_coverage)
            != static_cast<bool>(ir_record.propagate_reverse_coverage)) {
        fail(
            GraphJitDiagnosticStage::package_llvm,
            "NodeCompilerRecord background-callback ABI disagrees with retained package LLVM for node '"
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

llvm::FunctionType* root_block_operation_type(llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {pointer, size_type, size_type},
        false);
}

llvm::FunctionType* root_background_operation_type(llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {pointer, pointer}, false);
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

    auto* block_type = root_block_operation_type(module.getContext());
    validate_root_operation(
        module, output.root_symbols.tick_block, block_type, "tick_block", true);

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

void optimize_project_module(llvm::Module& module, llvm::TargetMachine& target_machine)
{
    // The finalized package artifacts deliberately carry Clang O0 barriers.
    // Once the lowerer has imported only the selected project execution closure,
    // every remaining definition is graph-runtime code and should participate in
    // the whole-project optimization pipeline.
    for (auto& function : module) {
        if (function.isDeclaration()) continue;
        function.removeFnAttr(llvm::Attribute::OptimizeNone);
        function.removeFnAttr(llvm::Attribute::NoInline);
    }

    llvm::PassBuilder pass_builder(&target_machine);
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager cgscc;
    llvm::ModuleAnalysisManager modules;
    pass_builder.registerModuleAnalyses(modules);
    pass_builder.registerCGSCCAnalyses(cgscc);
    pass_builder.registerFunctionAnalyses(functions);
    pass_builder.registerLoopAnalyses(loops);
    pass_builder.crossRegisterProxies(loops, functions, cgscc, modules);
    auto pipeline = pass_builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    pipeline.run(module, modules);
}

struct SharedProjectJit {
    std::mutex mutex{};
    std::unique_ptr<llvm::orc::LLJIT> jit{};
    std::atomic<std::uint64_t> next_generation{0};
};

struct ProjectCodeLifetime {
    std::shared_ptr<SharedProjectJit> shared{};
    llvm::orc::JITDylib* jit_dylib = nullptr;
    llvm::orc::ResourceTrackerSP resources{};

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
    graph_jit::LoweredGraphRootSymbols const& root_symbols,
    std::uint64_t project_generation)
{
    auto lifetime = std::make_shared<ProjectCodeLifetime>();
    lifetime->shared = shared;

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
        for (auto const& revision : package_revisions) {
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
            .tick_block = symbol.template operator()<CompiledGraphBlockFunction>(
                root_symbols.tick_block, "root tick_block"),
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
} // namespace

class GraphJit::Impl {
    GraphJitKernelSpecialization specialization_{};
    RealtimeStorageCostModel realtime_storage_cost_model_{};
    std::unique_ptr<llvm::TargetMachine> optimization_target_{};
    std::shared_ptr<SharedProjectJit> shared_jit_{};
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

        initialize_graph_jit_target();
        auto target = take_llvm_expected(
            llvm::orc::JITTargetMachineBuilder::detectHost(),
            GraphJitDiagnosticStage::materialization,
            "detect project ORC target");
        target.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
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
        shared_jit_ = std::make_shared<SharedProjectJit>();
        shared_jit_->jit = std::move(jit);
    }

    GraphJitKernelSpecialization const& specialization() const noexcept
    {
        return specialization_;
    }

    GraphJitCompileResult compile(GraphJitCompileRequest const& request)
    {
        std::lock_guard compile_lock(compile_mutex_);
        GraphJitCompileResult result{.attempted = true};
        auto captured = capture_inputs(request);
        if (!captured.diagnostics.empty()) {
            result.diagnostics = std::move(captured.diagnostics);
            return result;
        }

        auto context = std::make_unique<llvm::LLVMContext>();
        std::vector<ParsedPackage> parsed_packages;
        parsed_packages.reserve(captured.used_revisions.size());
        std::unordered_map<PackageRevision const*, std::size_t> parsed_by_revision;
        for (auto const& revision : request.definitions->package_revisions) {
            if (!revision || !captured.used_revisions.contains(revision.get())) continue;
            try {
                auto parsed = parse_package_module(
                    revision,
                    *context,
                    shared_jit_->jit->getDataLayout(),
                    shared_jit_->jit->getTargetTriple());
                parsed_by_revision.emplace(revision.get(), parsed_packages.size());
                parsed_packages.push_back(std::move(parsed));
            } catch (GraphJitCompileError const& error) {
                result.diagnostics.push_back(diagnostic(
                    error.stage,
                    error.what(),
                    no_node_bundle,
                    {},
                    revision->package_root.generic_string()));
            }
        }
        if (!result.diagnostics.empty()) return result;

        std::vector<graph_jit::PackageModule> package_modules;
        package_modules.reserve(parsed_packages.size());
        for (auto& parsed : parsed_packages) {
            package_modules.push_back(graph_jit::PackageModule{
                .revision = parsed.revision,
                .module = std::move(parsed.module),
                .retained_globals = parsed.retained_globals,
            });
        }

        std::vector<graph_jit::NodeImplementation> implementations;
        implementations.reserve(captured.nodes.size());
        try {
            for (auto const& captured_node : captured.nodes) {
                auto const parsed_index = parsed_by_revision.find(captured_node.revision.get());
                if (parsed_index == parsed_by_revision.end()) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "internal GraphJit error: provider package was not parsed");
                }
                auto& parsed = parsed_packages[parsed_index->second];
                auto const record = parsed.node_records.find(
                    captured_node.compiler_record.code_key);
                if (record == parsed.node_records.end()) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "retained package LLVM has no NodeCompilerRecord for node '"
                            + captured_node.identity.node_type_id + "'");
                }
                verify_compiler_record(captured_node, record->second);
                implementations.push_back(graph_jit::NodeImplementation{
                    .node_bundle = captured_node.node_bundle,
                    .identity = captured_node.identity,
                    .code_key = captured_node.compiler_record.code_key,
                    .revision = captured_node.revision,
                    .package_module = package_modules[parsed_index->second].module.get(),
                    .state_size = record->second.state_size,
                    .state_alignment = record->second.state_alignment,
                    .background_state_size = record->second.background_state_size,
                    .background_state_alignment = record->second.background_state_alignment,
                    .intrinsically_replayable = record->second.intrinsically_replayable,
                    .node_data = captured_node.node_data,
                    .state_structures = captured_node.state_structures,
                    .declare_node = captured_node.declare_node,
                    .tick_block = record->second.tick_block,
                    .skip_block = record->second.skip_block,
                    .tock_coverage = record->second.tock_coverage,
                    .propagate_forward_coverage =
                        record->second.propagate_forward_coverage,
                    .propagate_reverse_coverage =
                        record->second.propagate_reverse_coverage,
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
                auto const parsed_index = parsed_by_revision.find(
                    captured_relocation.revision.get());
                if (parsed_index == parsed_by_revision.end()) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "internal GraphJit error: relocation package was not parsed");
                }
                auto& parsed = parsed_packages[parsed_index->second];
                auto const index = *captured_relocation.relocation->retained_global_index;
                if (index >= parsed.retained_globals.size()
                    || parsed.retained_globals[index] == nullptr) {
                    fail(
                        GraphJitDiagnosticStage::package_llvm,
                        "retained package LLVM is missing a configuration relocation global");
                }
                relocations.push_back(graph_jit::ConfigRelocation{
                    .node_bundle = captured_relocation.node_bundle,
                    .relocation = captured_relocation.relocation,
                    .revision = captured_relocation.revision,
                    .retained_global = parsed.retained_globals[index],
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

        graph_jit::LoweringInput lowering_input{
            .graph = *request.graph,
            .specialization = specialization_,
            .realtime_storage_cost_model = realtime_storage_cost_model_,
            .packages = package_modules,
            .node_implementations = implementations,
            .config_relocations = relocations,
        };
        auto lowering = graph_jit::lower_configured_graph_to_llvm(lowering_input, *output);
        if (!lowering) {
            result.diagnostics.push_back(diagnostic(
                GraphJitDiagnosticStage::lowering,
                std::move(lowering.error())));
            return result;
        }

        try {
            validate_lowering_output(*output, *lowering);
            verify_module(
                *output,
                GraphJitDiagnosticStage::lowering,
                "graph lowerer produced invalid LLVM");
            optimize_project_module(*output, *optimization_target_);
            verify_module(
                *output,
                GraphJitDiagnosticStage::optimization,
                "optimized project LLVM is invalid");

            std::vector<std::shared_ptr<PackageRevision const>> package_revisions;
            package_revisions.reserve(package_modules.size());
            for (auto const& package : package_modules) {
                package_revisions.push_back(package.revision);
            }

            // Package LLVM parses are compile-local. Lowering consumes selected
            // modules directly into the project module; any unused package parses
            // are released with package_modules after this compilation.
            auto materialized = materialize_project_module(
                shared_jit_,
                std::move(output),
                std::move(context),
                package_revisions,
                lowering->root_symbols,
                request.project_generation);
            result.compiled_graph = std::make_shared<CompiledGraph>(CompiledGraph{
                .project_generation = request.project_generation,
                .definitions_generation = request.definitions->generation,
                .specialization = specialization_,
                .configured_graph = request.graph,
                .package_revisions = std::move(package_revisions),
                .node_layout = std::move(lowering->node_layout),
                .background_evaluation_plan = std::move(lowering->background_evaluation_plan),
                .root_operations = materialized.root_operations,
                .background_operations = materialized.background_operations,
                .code_lifetime = std::move(materialized.lifetime),
            });
        } catch (GraphJitCompileError const& error) {
            result.diagnostics.push_back(diagnostic(error.stage, error.what()));
        }
        return result;
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

GraphJitCompileResult GraphJit::handle_project_graph_transaction(
    GraphJitCompileRequest const& request)
{
    return compile(request);
}
} // namespace iv
