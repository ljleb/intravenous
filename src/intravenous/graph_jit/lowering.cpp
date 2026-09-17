#include <intravenous/graph_jit/lowering.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <cstddef>
#include <string_view>
#include <utility>

namespace iv::graph_jit {
namespace {
constexpr std::string_view root_tick_block_symbol = "__iv_graph_root_tick_block";
constexpr std::string_view root_skip_block_symbol = "__iv_graph_root_skip_block";

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
} // namespace

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput const& input,
    llvm::Module& output_module)
{
    // Establish the complete lowering/materialization seam with the semantic
    // identity element first. This path is intentionally real rather than a
    // test-only shortcut: an empty project is a valid executable generation.
    if (!is_structurally_empty(input.graph)) {
        return std::unexpected(
            "ConfiguredGraph -> LLVM IR lowering for non-empty graphs has not been implemented yet");
    }

    if (output_module.getNamedValue(root_tick_block_symbol)
        || output_module.getNamedValue(root_skip_block_symbol)) {
        return std::unexpected(
            "ConfiguredGraph -> LLVM IR lowering output module already contains reserved root symbols");
    }

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
} // namespace iv::graph_jit
