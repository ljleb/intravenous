#include <intravenous/graph_jit/lowering.h>

#include <intravenous/graph/reflected_node_operations.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace iv::graph_jit {
namespace {
constexpr std::string_view root_tick_block_symbol = "__iv_graph_root_tick_block";
constexpr std::string_view root_skip_block_symbol = "__iv_graph_root_skip_block";

struct PrimitiveBundle {
    std::size_t node_bundle = 0;
    std::size_t node_size = 0;
    std::size_t node_alignment = 1;
    std::size_t maximum_block_size = 0;
    bool block_skippable = false;
};

bool is_structurally_empty(ConfiguredGraph const& graph)
{
    if (!graph.connections.configured_sample_connections().empty()
        || !graph.connections.configured_event_connections().empty()
        || !graph.virtual_nodes.records().empty()) {
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

bool valid_alignment(std::size_t alignment) noexcept
{
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

std::expected<PrimitiveBundle, std::string> single_stateless_zero_port_primitive(
    LoweringInput const& input)
{
    if (!input.graph.connections.configured_sample_connections().empty()
        || !input.graph.connections.configured_event_connections().empty()) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice does not yet support graph connections");
    }
    if (!input.graph.virtual_nodes.records().empty()) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice does not yet support virtual nodes");
    }
    if (!input.config_relocations.empty()) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice does not yet support node configuration pointer relocations");
    }

    std::optional<PrimitiveBundle> primitive;
    std::string structural_error;
    std::size_t boundary_count = 0;
    std::size_t node_bundle = 0;
    input.graph.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            auto const current_bundle = node_bundle++;
            if (!structural_error.empty()) return;

            auto const has_ports = view.ports != nullptr
                && (view.ports->sample_input_count() != 0
                    || view.ports->sample_output_count() != 0
                    || view.ports->event_input_count() != 0
                    || view.ports->event_output_count() != 0);

            if (view.kind == ConfiguredNodeBundleKind::boundary) {
                ++boundary_count;
                if (has_ports || boundary_count != 1
                    || current_bundle != input.graph.public_ports.boundary_handle()) {
                    structural_error =
                        "first non-empty GraphJit lowering slice requires exactly one zero-port project boundary";
                }
                return;
            }
            if (view.kind != ConfiguredNodeBundleKind::concrete) {
                structural_error =
                    "first non-empty GraphJit lowering slice supports only flat concrete primitives";
                return;
            }
            if (has_ports) {
                structural_error =
                    "first non-empty GraphJit lowering slice supports only zero-port primitives";
                return;
            }
            if (primitive) {
                structural_error =
                    "first non-empty GraphJit lowering slice supports exactly one concrete primitive";
                return;
            }
            if (!valid_alignment(view.node_alignment)) {
                structural_error =
                    "configured primitive has invalid node configuration alignment";
                return;
            }
            if ((view.lifetime && view.lifetime->ttl_samples)
                || (view.deferred_detach && view.deferred_detach->has_value())) {
                structural_error =
                    "first non-empty GraphJit lowering slice does not yet support activity or detach semantics";
                return;
            }
            primitive = PrimitiveBundle{
                .node_bundle = current_bundle,
                .node_size = view.node_size,
                .node_alignment = view.node_alignment,
                .maximum_block_size = view.maximum_block_size,
                .block_skippable = view.block_skippable,
            };
        });

    if (!structural_error.empty()) return std::unexpected(std::move(structural_error));
    if (boundary_count != 1) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice requires exactly one project boundary");
    }
    if (!primitive) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice found no concrete primitive");
    }
    if (primitive->maximum_block_size < input.specialization.block_size) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice does not yet split blocks for a primitive maximum block size");
    }
    return *primitive;
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

llvm::FunctionType* primitive_block_operation_type(llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {pointer, pointer, size_type, size_type},
        false);
}

llvm::Function* define_noop_root_operation(
    llvm::Module& module,
    llvm::FunctionType* type,
    std::string_view symbol)
{
    auto* function = llvm::Function::Create(
        type,
        llvm::GlobalValue::ExternalLinkage,
        llvm::StringRef(symbol.data(), symbol.size()),
        module);
    function->setCallingConv(llvm::CallingConv::C);

    auto* entry = llvm::BasicBlock::Create(module.getContext(), "entry", function);
    llvm::IRBuilder<> builder(entry);
    builder.CreateRetVoid();
    return function;
}

llvm::GlobalVariable* immutable_bytes_global(
    llvm::Module& module,
    void const* data,
    std::size_t size,
    std::size_t alignment,
    llvm::Twine const& name)
{
    auto bytes = llvm::StringRef(static_cast<char const*>(data), size);
    auto* initializer = llvm::ConstantDataArray::getString(
        module.getContext(), bytes, false);
    auto* global = new llvm::GlobalVariable(
        module,
        initializer->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        initializer,
        name);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(alignment));
    return global;
}

std::expected<llvm::Function*, std::string> declare_primitive_callback(
    llvm::Module& output_module,
    llvm::Function* source,
    llvm::FunctionType* expected_type,
    std::string_view role)
{
    if (!source || source->getFunctionType() != expected_type) {
        return std::unexpected(
            "selected primitive " + std::string(role)
            + " callback has the wrong LLVM ABI");
    }
    if (source->getParent() == nullptr) {
        return std::unexpected(
            "selected primitive " + std::string(role)
            + " callback is detached from its package module");
    }

    auto const name = source->getName();
    if (auto* existing = output_module.getFunction(name)) {
        if (existing->getFunctionType() != source->getFunctionType()) {
            return std::unexpected(
                "selected primitive callback symbol collides with an incompatible project symbol");
        }
        return existing;
    }

    auto* declaration = llvm::Function::Create(
        source->getFunctionType(),
        llvm::GlobalValue::ExternalLinkage,
        name,
        output_module);
    declaration->setCallingConv(source->getCallingConv());
    declaration->setVisibility(source->getVisibility());
    declaration->setDSOLocal(source->isDSOLocal());
    return declaration;
}

llvm::Function* define_single_primitive_root_operation(
    llvm::Module& module,
    llvm::FunctionType* root_type,
    std::string_view symbol,
    llvm::Function* primitive_callback,
    llvm::GlobalVariable* node_config,
    llvm::GlobalVariable* tick_context)
{
    auto* function = llvm::Function::Create(
        root_type,
        llvm::GlobalValue::ExternalLinkage,
        llvm::StringRef(symbol.data(), symbol.size()),
        module);
    function->setCallingConv(llvm::CallingConv::C);

    auto arguments = function->arg_begin();
    auto* storage_base = &*arguments++;
    auto* sample_index = &*arguments++;
    auto* block_size = &*arguments;
    storage_base->setName("storage_base");
    sample_index->setName("sample_index");
    block_size->setName("block_size");
    (void)storage_base;

    auto* entry = llvm::BasicBlock::Create(module.getContext(), "entry", function);
    llvm::IRBuilder<> builder(entry);
    auto* call = builder.CreateCall(
        primitive_callback,
        {node_config, tick_context, sample_index, block_size});
    call->setCallingConv(primitive_callback->getCallingConv());
    builder.CreateRetVoid();
    return function;
}

std::expected<LoweringOutput, std::string> lower_single_stateless_zero_port_primitive(
    LoweringInput const& input,
    llvm::Module& output_module)
{
    auto primitive = single_stateless_zero_port_primitive(input);
    if (!primitive) return std::unexpected(std::move(primitive.error()));

    NodeImplementation const* implementation = nullptr;
    for (auto const& candidate : input.node_implementations) {
        if (candidate.node_bundle != primitive->node_bundle) continue;
        if (implementation) {
            return std::unexpected(
                "GraphJit lowering received duplicate implementations for one primitive bundle");
        }
        implementation = &candidate;
    }
    if (!implementation) {
        return std::unexpected(
            "GraphJit lowering has no resolved implementation for the concrete primitive");
    }
    if (input.node_implementations.size() != 1) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice requires exactly one resolved primitive implementation");
    }
    if (!implementation->package_module || !implementation->tick_block
        || !implementation->declare_node || !implementation->node_data) {
        return std::unexpected(
            "resolved primitive implementation is incomplete at the GraphJit lowering boundary");
    }
    if (implementation->tick_block->getParent() != implementation->package_module
        || (implementation->skip_block
            && implementation->skip_block->getParent() != implementation->package_module)) {
        return std::unexpected(
            "resolved primitive callbacks do not belong to the selected package LLVM module");
    }
    if (implementation->state_size != 0 || implementation->compiled_state_size != 0) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice does not yet materialize State or CompiledState spans");
    }
    if (primitive->node_size == 0) {
        return std::unexpected("configured primitive has empty node configuration storage");
    }

    NodeLayoutBuilder layout_builder(input.specialization.block_size);
    auto const node_index = implementation->declare_node(
        implementation->node_data,
        implementation->state_structures,
        layout_builder);
    auto node_layout = std::move(layout_builder).build();
    if (node_index != 0 || node_layout.nodes.size() != 1
        || node_layout.storage_size != 0
        || node_layout.nodes.front().state_size != 0
        || node_layout.nodes.front().compiled_state_size != 0) {
        return std::unexpected(
            "first non-empty GraphJit lowering slice does not yet support nested or storage-owning primitive declarations");
    }

    auto* primitive_type = primitive_block_operation_type(output_module.getContext());
    auto tick_callback = declare_primitive_callback(
        output_module, implementation->tick_block, primitive_type, "tick_block");
    if (!tick_callback) return std::unexpected(std::move(tick_callback.error()));

    llvm::Function* skip_callback = nullptr;
    if (primitive->block_skippable) {
        auto skip = declare_primitive_callback(
            output_module, implementation->skip_block, primitive_type, "skip_block");
        if (!skip) return std::unexpected(std::move(skip.error()));
        skip_callback = *skip;
    }

    auto* node_config = immutable_bytes_global(
        output_module,
        implementation->node_data,
        primitive->node_size,
        primitive->node_alignment,
        "iv.graph.node_config.0");

    ReflectedNodeTickContext context_template{
        .sample_rate = input.specialization.sample_rate,
        .scc_feedback_latency = 0,
    };
    auto* tick_context = immutable_bytes_global(
        output_module,
        &context_template,
        sizeof(context_template),
        alignof(ReflectedNodeTickContext),
        "iv.graph.tick_context.0");

    auto* root_type = root_block_operation_type(output_module.getContext());
    define_single_primitive_root_operation(
        output_module,
        root_type,
        root_tick_block_symbol,
        *tick_callback,
        node_config,
        tick_context);
    if (skip_callback) {
        define_single_primitive_root_operation(
            output_module,
            root_type,
            root_skip_block_symbol,
            skip_callback,
            node_config,
            tick_context);
    }

    auto source_module = llvm::CloneModule(*implementation->package_module);
    if (llvm::Linker::linkModules(
            output_module,
            std::move(source_module),
            llvm::Linker::Flags::LinkOnlyNeeded)) {
        return std::unexpected(
            "failed to import selected primitive LLVM callback closure into the project module");
    }

    auto* imported_tick = output_module.getFunction(implementation->tick_block->getName());
    if (!imported_tick || imported_tick->isDeclaration()) {
        return std::unexpected(
            "selected primitive tick_block closure was not imported into the project module");
    }
    if (primitive->block_skippable) {
        auto* imported_skip = output_module.getFunction(implementation->skip_block->getName());
        if (!imported_skip || imported_skip->isDeclaration()) {
            return std::unexpected(
                "selected primitive skip_block closure was not imported into the project module");
        }
    }

    return LoweringOutput{
        .node_layout = std::move(node_layout),
        .root_symbols = {
            .tick_block = std::string(root_tick_block_symbol),
            .skip_block = primitive->block_skippable
                ? std::string(root_skip_block_symbol)
                : std::string{},
        },
    };
}
} // namespace

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput const& input,
    llvm::Module& output_module)
{
    if (output_module.getNamedValue(root_tick_block_symbol)
        || output_module.getNamedValue(root_skip_block_symbol)) {
        return std::unexpected(
            "ConfiguredGraph -> LLVM IR lowering output module already contains reserved root symbols");
    }

    // Keep the semantic identity path explicit. Besides making empty projects
    // valid executable generations, it remains the baseline for the complete
    // lowering/materialization seam.
    if (is_structurally_empty(input.graph)) {
        NodeLayoutBuilder layout_builder(input.specialization.block_size);
        auto node_layout = std::move(layout_builder).build();

        auto* block_type = root_block_operation_type(output_module.getContext());
        define_noop_root_operation(output_module, block_type, root_tick_block_symbol);
        define_noop_root_operation(output_module, block_type, root_skip_block_symbol);

        return LoweringOutput{
            .node_layout = std::move(node_layout),
            .root_symbols = {
                .tick_block = std::string(root_tick_block_symbol),
                .skip_block = std::string(root_skip_block_symbol),
            },
        };
    }

    // First non-empty executable slice. It deliberately requires a shape whose
    // complete semantics fit without sample/event physical planning: one flat,
    // stateless zero-port primitive. This establishes exact declaration,
    // configuration embedding, selected LLVM-closure import, and generated-root
    // dispatch before connection and state-context lowering are added.
    return lower_single_stateless_zero_port_primitive(input, output_module);
}
} // namespace iv::graph_jit
