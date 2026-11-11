#include <intravenous/graph_jit/lowering.h>
#include <intravenous/graph_jit/lowering_plan.h>
#include <intravenous/graph_jit/realtime_root.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Alignment.h>

#include <chrono>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv::graph_jit {
namespace {
constexpr std::string_view root_background_forward_symbol =
    "__iv_graph_root_background_forward";
constexpr std::string_view root_background_reverse_symbol =
    "__iv_graph_root_background_reverse";
constexpr std::string_view root_background_evaluate_symbol =
    "__iv_graph_root_background_evaluate";
constexpr std::string_view root_combine_sequential_input_symbol =
    "__iv_graph_root_combine_sequential_input";
constexpr std::string_view root_initialize_sequential_input_symbol =
    "__iv_graph_root_initialize_sequential_input";

struct BackgroundCallByteOffsets {
    std::size_t nodes_data = 0;
    std::size_t nodes_size = 0;
    std::size_t activity = 0;
    std::size_t operation_frame = 0;
    std::size_t prepare_operations = 0;
    std::size_t finalize_operations = 0;
    std::size_t forward_context = 0;
    std::size_t reverse_context = 0;
    std::size_t tock_context = 0;
    std::size_t replay_context = 0;
    std::size_t replay_forward = 0;
    std::size_t replay_reverse = 0;
    std::size_t replay_tick_context = 0;
    std::size_t replay_regions_data = 0;
    std::size_t replay_regions_size = 0;
};

struct EmittedNodeConfiguration {
    llvm::GlobalVariable* node_config = nullptr;
};

constexpr BackgroundCallByteOffsets background_batch_byte_offsets() noexcept
{
    return {
        .nodes_data = offsetof(BackgroundEvaluationCall, nodes)
            + offsetof(ReflectedSpan<BackgroundNodeCall>, pointer),
        .nodes_size = offsetof(BackgroundEvaluationCall, nodes)
            + offsetof(ReflectedSpan<BackgroundNodeCall>, extent),
        .activity = offsetof(BackgroundNodeCall, activity),
        .operation_frame = offsetof(BackgroundNodeCall, operation_frame),
        .prepare_operations = offsetof(
            BackgroundNodeCall, prepare_operations),
        .finalize_operations = offsetof(
            BackgroundNodeCall, finalize_operations),
        .forward_context = offsetof(BackgroundNodeCall, forward),
        .reverse_context = offsetof(BackgroundNodeCall, reverse),
        .tock_context = offsetof(BackgroundNodeCall, tock),
        .replay_context = offsetof(
            BackgroundNodeCall, replay_context),
        .replay_forward = offsetof(
            BackgroundNodeCall, replay_forward),
        .replay_reverse = offsetof(
            BackgroundNodeCall, replay_reverse),
        .replay_tick_context = offsetof(BackgroundNodeCall, replay),
        .replay_regions_data = offsetof(BackgroundNodeCall, replay_regions)
            + offsetof(ReflectedSpan<GlobalIndexRange const>, pointer),
        .replay_regions_size = offsetof(BackgroundNodeCall, replay_regions)
            + offsetof(ReflectedSpan<GlobalIndexRange const>, extent),
    };
}

llvm::FunctionType* primitive_block_operation_type(llvm::LLVMContext& context)
{
    return package_callback_type(context, CallbackImportAbi::block);
}

llvm::FunctionType* background_operation_type(llvm::LLVMContext& context)
{
    return package_callback_type(context, CallbackImportAbi::background);
}

llvm::FunctionType* batch_operation_type(llvm::LLVMContext& context)
{
    return package_callback_type(context, CallbackImportAbi::batch);
}

llvm::FunctionType* background_root_operation_type(llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {pointer}, false);
}

llvm::FunctionType* background_operation_hook_type(
    llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {pointer}, false);
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

// Package code is compiled once per accepted revision (graph_jit_direction.md),
// so the project module imports the selected callbacks and retained globals by
// declaring the symbols that code exports. The generation JITDylib resolves
// them through its link order.
std::expected<void, std::string> emit_package_imports(
    LoweringInput const& input,
    detail::PackageImportPlan const& plan,
    llvm::Module& output_module)
{
    auto& context = output_module.getContext();
    for (auto const& package_plan : plan.packages) {
        if (package_plan.package_index >= input.packages.size()) {
            return std::unexpected("planned package code is not a GraphJit lowering input");
        }
        for (auto const& callback : package_plan.callbacks) {
            auto const name = llvm::StringRef(callback.symbol);
            if (name.empty() || output_module.getNamedValue(name)) {
                return std::unexpected(
                    "selected primitive " + callback.role
                    + " import symbol is empty or collides with a project symbol");
            }
            llvm::Function::Create(
                package_callback_type(context, callback.abi),
                llvm::GlobalValue::ExternalLinkage,
                name,
                output_module);
        }
        for (auto const& global : package_plan.retained_globals) {
            auto const name = llvm::StringRef(global.symbol);
            if (name.empty() || output_module.getNamedValue(name) || global.size == 0) {
                return std::unexpected(
                    "retained package global import is empty or collides with a project symbol");
            }
            auto* declaration = new llvm::GlobalVariable(
                output_module,
                llvm::ArrayType::get(llvm::Type::getInt8Ty(context), global.size),
                true,
                llvm::GlobalValue::ExternalLinkage,
                nullptr,
                name);
            declaration->setAlignment(llvm::Align(global.alignment));
        }
    }
    return {};
}

std::expected<llvm::GlobalVariable*, std::string> immutable_node_configuration_global(
    llvm::Module& module,
    detail::NodeConfigurationPlan const& node)
{
    if (node.relocations.empty()) {
        return immutable_bytes_global(
            module,
            node.bytes.data(),
            node.bytes.size(),
            node.alignment,
            node.node_global_symbol);
    }

    if (module.getDataLayout().getPointerSize() != sizeof(void*)) {
        return std::unexpected(
            "project LLVM pointer size disagrees with node configuration ABI");
    }

    auto& context = module.getContext();
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* byte_type = llvm::Type::getInt8Ty(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    std::vector<llvm::Constant*> fields;
    std::vector<llvm::Type*> field_types;
    auto append_bytes = [&](std::size_t offset, std::size_t size) {
        if (size == 0) return;
        auto bytes = llvm::StringRef(
            reinterpret_cast<char const*>(node.bytes.data() + offset), size);
        auto* constant = llvm::ConstantDataArray::getString(context, bytes, false);
        fields.push_back(constant);
        field_types.push_back(constant->getType());
    };

    std::size_t cursor = 0;
    for (auto const& relocation : node.relocations) {
        if (relocation.byte_offset < cursor
            || relocation.byte_offset > node.bytes.size()
            || sizeof(void*) > node.bytes.size() - relocation.byte_offset) {
            return std::unexpected(
                "planned node configuration relocation is outside configuration bytes");
        }
        append_bytes(cursor, relocation.byte_offset - cursor);

        llvm::Constant* pointer = nullptr;
        if (relocation.retained_global_symbol.empty()) {
            pointer = llvm::ConstantPointerNull::get(pointer_type);
        } else {
            auto* global = module.getNamedGlobal(relocation.retained_global_symbol);
            if (!global) {
                return std::unexpected(
                    "planned node configuration relocation references an unimported retained global");
            }
            pointer = global;
            if (relocation.addend != 0) {
                llvm::Constant* indices[] = {
                    llvm::ConstantInt::get(size_type, relocation.addend)};
                pointer = llvm::ConstantExpr::getInBoundsGetElementPtr(
                    byte_type, global, indices);
            }
        }
        fields.push_back(pointer);
        field_types.push_back(pointer_type);
        cursor = relocation.byte_offset + sizeof(void*);
    }
    append_bytes(cursor, node.bytes.size() - cursor);

    auto* configuration_type = llvm::StructType::get(context, field_types, true);
    auto const configuration_size = module.getDataLayout().getTypeAllocSize(
        configuration_type);
    if (configuration_size.isScalable()
        || configuration_size.getFixedValue() != node.bytes.size()) {
        return std::unexpected(
            "relocated node configuration LLVM layout disagrees with native configuration size");
    }
    auto* initializer = llvm::ConstantStruct::get(configuration_type, fields);
    auto* global = new llvm::GlobalVariable(
        module,
        configuration_type,
        true,
        llvm::GlobalValue::PrivateLinkage,
        initializer,
        node.node_global_symbol);
    global->setAlignment(llvm::Align(node.alignment));
    return global;
}

std::expected<std::vector<EmittedNodeConfiguration>, std::string> emit_node_configurations(
    detail::ConfigurationPlan const& plan,
    llvm::Module& output_module)
{
    std::vector<EmittedNodeConfiguration> emitted;
    emitted.reserve(plan.nodes.size());
    for (auto const& node : plan.nodes) {
        auto node_config = immutable_node_configuration_global(output_module, node);
        if (!node_config) return std::unexpected(std::move(node_config.error()));
        emitted.push_back(EmittedNodeConfiguration{.node_config = *node_config});
    }
    return emitted;
}


llvm::Value* byte_offset_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* base,
    std::size_t offset,
    llvm::Twine const& name)
{
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    return builder.CreateInBoundsGEP(
        llvm::Type::getInt8Ty(builder.getContext()),
        base,
        llvm::ConstantInt::get(size_type, offset),
        name);
}

void store_context_span_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* context_storage,
    std::size_t data_field_offset,
    std::size_t size_field_offset,
    llvm::Value* data,
    std::size_t span_size)
{
    auto& llvm_context = builder.getContext();
    auto* byte_type = llvm::Type::getInt8Ty(llvm_context);
    auto* size_type = llvm::IntegerType::get(
        llvm_context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto offset = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };

    auto* data_index = builder.CreateInBoundsGEP(
        byte_type, context_storage, offset(data_field_offset), "span.data.index");
    builder.CreateStore(data, data_index);
    auto* size_index = builder.CreateInBoundsGEP(
        byte_type, context_storage, offset(size_field_offset), "span.size.index");
    builder.CreateStore(offset(span_size), size_index);
}

enum class BackgroundRootOperation : std::uint8_t {
    forward,
    reverse,
    evaluate,
};

std::expected<llvm::Function*, std::string> define_background_root_operation(
    llvm::Module& module,
    std::string_view symbol,
    detail::LoweringPlan const& plan,
    std::span<EmittedNodeConfiguration const> configurations,
    BackgroundRootOperation phase)
{
    auto& context = module.getContext();
    auto* function_type = background_root_operation_type(context);
    auto* function = llvm::Function::Create(
        function_type,
        llvm::GlobalValue::ExternalLinkage,
        llvm::StringRef(symbol.data(), symbol.size()),
        module);
    function->setCallingConv(llvm::CallingConv::C);

    auto arguments = function->arg_begin();
    llvm::Value* batch = &*arguments;
    batch->setName("batch");

    llvm::IRBuilder<> builder(
        llvm::BasicBlock::Create(context, "entry", function));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* byte_type = llvm::Type::getInt8Ty(context);
    auto const offsets = background_batch_byte_offsets();

    auto* nodes = builder.CreateLoad(
        pointer_type,
        byte_offset_pointer(builder, batch, offsets.nodes_data, "nodes.index"),
        "nodes");
    auto* node_count = builder.CreateLoad(
        size_type,
        byte_offset_pointer(builder, batch, offsets.nodes_size, "node.count.index"),
        "node.count");
    auto* frame_complete = builder.CreateICmpUGE(
        node_count,
        llvm::ConstantInt::get(size_type, plan.connections.background.nodes.size()),
        "batch.complete");
    auto* ready = llvm::BasicBlock::Create(context, "batch.ready", function);
    auto* invalid = llvm::BasicBlock::Create(context, "batch.invalid", function);
    builder.CreateCondBr(frame_complete, ready, invalid);
    builder.SetInsertPoint(invalid);
    builder.CreateRetVoid();
    builder.SetInsertPoint(ready);

    auto primitive_for = [&](BackgroundNodeIndex node)
        -> std::expected<std::size_t, std::string> {
        if (node >= plan.connections.background.nodes.size()) {
            return std::unexpected(
                "GraphJit background execution order references a missing node");
        }
        auto const bundle = plan.connections.background.nodes[node].bundle;
        for (std::size_t primitive = 0;
             primitive < plan.imports.primitive_callbacks.size(); ++primitive) {
            if (plan.imports.primitive_callbacks[primitive].bundle == bundle) {
                if (primitive >= configurations.size()
                    || primitive >= plan.declarations.primitive_storage.size()) {
                    return std::unexpected(
                        "GraphJit background node has no configuration/storage plan");
                }
                return primitive;
            }
        }
        return std::unexpected(
            "GraphJit background plan references a non-primitive node");
    };

    auto node_pointer = [&](BackgroundNodeIndex node) {
        return byte_offset_pointer(
            builder,
            nodes,
            node * sizeof(BackgroundNodeCall),
            "background.node");
    };

    auto emit_operation_hook = [&](llvm::Value* node_frame,
                                   std::size_t callback_offset,
                                   llvm::Twine const& name) {
        auto* callback = builder.CreateLoad(
            pointer_type,
            byte_offset_pointer(
                builder,
                node_frame,
                callback_offset,
                name + ".callback.index"),
            name + ".callback");
        auto* operation_frame = builder.CreateLoad(
            pointer_type,
            byte_offset_pointer(
                builder,
                node_frame,
                offsets.operation_frame,
                name + ".frame.index"),
            name + ".frame");
        builder.CreateCall(
            background_operation_hook_type(context),
            callback,
            {operation_frame});
    };

    auto emit_when_active = [&]<typename Emit>(
                                BackgroundNodeIndex node,
                                BackgroundNodeActivity activity,
                                Emit&& emit) {
        auto* node_frame = node_pointer(node);
        auto* activity_value = builder.CreateLoad(
            byte_type,
            byte_offset_pointer(
                builder, node_frame, offsets.activity, "activity.index"),
            "activity");
        auto const bit = static_cast<std::uint8_t>(activity);
        auto* active = builder.CreateICmpNE(
            builder.CreateAnd(
                activity_value,
                llvm::ConstantInt::get(byte_type, bit)),
            llvm::ConstantInt::get(byte_type, 0),
            "active");
        auto* invoke = llvm::BasicBlock::Create(context, "background.invoke", function);
        auto* next = llvm::BasicBlock::Create(context, "background.next", function);
        builder.CreateCondBr(active, invoke, next);
        builder.SetInsertPoint(invoke);
        if (phase == BackgroundRootOperation::evaluate) {
            emit_operation_hook(
                node_frame,
                offsets.prepare_operations,
                "background.prepare");
        }
        emit(node_frame);
        if (phase == BackgroundRootOperation::evaluate) {
            emit_operation_hook(
                node_frame,
                offsets.finalize_operations,
                "background.finalize");
        }
        // Every emitter deliberately leaves its current tail open. Authored and
        // generated callbacks leave `invoke` open; replay leaves its loop's
        // `replay.done` block open. Close that exact tail unconditionally before
        // continuing the statically generated traversal instead of making the
        // control-flow contract depend on inspection of its last instruction.
        builder.CreateBr(next);
        builder.SetInsertPoint(next);
    };

    auto emit_authored = [&](llvm::Value* node_frame,
                             std::size_t primitive,
                             std::string const& callback,
                             std::size_t context_offset) {
        auto* imported = module.getFunction(callback);
        builder.CreateCall(
            background_operation_type(context),
            imported,
            {configurations[primitive].node_config,
             byte_offset_pointer(
                 builder, node_frame, context_offset, "background.context")});
    };

    auto emit_generated = [&](llvm::Value* node_frame,
                                std::size_t callback_offset,
                                std::size_t context_offset) {
        auto* callback = builder.CreateLoad(
            pointer_type,
            byte_offset_pointer(
                builder, node_frame, callback_offset, "generated.callback.index"),
            "generated.callback");
        auto* opaque = builder.CreateLoad(
            pointer_type,
            byte_offset_pointer(
                builder,
                node_frame,
                offsets.replay_context,
                "generated.context.index"),
            "generated.context");
        builder.CreateCall(
            background_operation_type(context),
            callback,
            {opaque,
             byte_offset_pointer(
                 builder, node_frame, context_offset, "background.context")});
    };

    auto try_emit_authored_batch = [&](std::span<BackgroundNodeIndex const> group)
        -> std::expected<bool, std::string> {
        if (group.size() < 2) return false;

        std::vector<std::pair<BackgroundNodeIndex, std::size_t>> candidates;
        candidates.reserve(group.size());
        std::string_view callback_symbol;
        std::size_t scalar_context_offset = 0;
        std::size_t lane_context_offset = 0;
        std::size_t lane_size = 0;
        std::size_t lane_alignment = 1;
        std::size_t batch_size = 0;
        std::size_t batch_alignment = 1;
        BackgroundNodeActivity activity = BackgroundNodeActivity::none;
        switch (phase) {
        case BackgroundRootOperation::forward:
            scalar_context_offset = offsets.forward_context;
            lane_context_offset = offsetof(
                ReflectedNodeForwardCoverageBatchLane, context);
            lane_size = sizeof(ReflectedNodeForwardCoverageBatchLane);
            lane_alignment = alignof(ReflectedNodeForwardCoverageBatchLane);
            batch_size = sizeof(ReflectedNodeForwardCoverageBatchContext);
            batch_alignment = alignof(ReflectedNodeForwardCoverageBatchContext);
            activity = BackgroundNodeActivity::forward;
            break;
        case BackgroundRootOperation::reverse:
            scalar_context_offset = offsets.reverse_context;
            lane_context_offset = offsetof(
                ReflectedNodeReverseCoverageBatchLane, context);
            lane_size = sizeof(ReflectedNodeReverseCoverageBatchLane);
            lane_alignment = alignof(ReflectedNodeReverseCoverageBatchLane);
            batch_size = sizeof(ReflectedNodeReverseCoverageBatchContext);
            batch_alignment = alignof(ReflectedNodeReverseCoverageBatchContext);
            activity = BackgroundNodeActivity::reverse;
            break;
        case BackgroundRootOperation::evaluate:
            scalar_context_offset = offsets.tock_context;
            lane_context_offset = offsetof(
                ReflectedNodeTockCoverageBatchLane, context);
            lane_size = sizeof(ReflectedNodeTockCoverageBatchLane);
            lane_alignment = alignof(ReflectedNodeTockCoverageBatchLane);
            batch_size = sizeof(ReflectedNodeTockCoverageBatchContext);
            batch_alignment = alignof(ReflectedNodeTockCoverageBatchContext);
            activity = BackgroundNodeActivity::evaluate;
            break;
        }

        for (auto const node : group) {
            if (node >= plan.connections.background.nodes.size()) {
                return std::unexpected(
                    "GraphJit background batch references a missing node");
            }
            auto const& background_node =
                plan.connections.background.nodes[node];
            if (!background_node.authored_tock_execution) return false;
            if (phase == BackgroundRootOperation::reverse
                && background_node.accumulators.input_requirement_count == 0) {
                continue;
            }
            auto primitive = primitive_for(node);
            if (!primitive) {
                return std::unexpected(std::move(primitive.error()));
            }
            auto const& callbacks =
                plan.imports.primitive_callbacks[*primitive];
            std::string const* selected = nullptr;
            switch (phase) {
            case BackgroundRootOperation::forward:
                selected = &callbacks.propagate_forward_coverage_batch;
                break;
            case BackgroundRootOperation::reverse:
                selected = &callbacks.propagate_reverse_coverage_batch;
                break;
            case BackgroundRootOperation::evaluate:
                selected = &callbacks.tock_coverage_batch;
                break;
            }
            if (selected == nullptr || selected->empty()) return false;
            if (callback_symbol.empty()) callback_symbol = *selected;
            if (*selected != callback_symbol) return false;
            candidates.emplace_back(node, *primitive);
        }
        if (candidates.size() < 2) return false;

        auto* imported = module.getFunction(
            llvm::StringRef(callback_symbol.data(), callback_symbol.size()));
        if (!imported) {
            return std::unexpected(
                "GraphJit background batch references an unimported callback");
        }
        if (candidates.size() > std::numeric_limits<std::size_t>::max()
                / lane_size) {
            return std::unexpected(
                "GraphJit background batch lane storage overflows size_t");
        }
        auto size_constant = [&](std::size_t value) {
            return llvm::ConstantInt::get(size_type, value);
        };
        auto* lane_storage = builder.CreateAlloca(
            byte_type,
            size_constant(candidates.size() * lane_size),
            "background.batch.lanes");
        lane_storage->setAlignment(llvm::Align(lane_alignment));
        auto* active_count = builder.CreateAlloca(
            size_type, nullptr, "background.batch.active.count");
        builder.CreateStore(size_constant(0), active_count);

        auto emit_if_active = [&](BackgroundNodeIndex node, auto&& emit) {
            auto* node_frame = node_pointer(node);
            auto* activity_value = builder.CreateLoad(
                byte_type,
                byte_offset_pointer(
                    builder, node_frame, offsets.activity,
                    "background.batch.activity.index"),
                "background.batch.activity");
            auto* active = builder.CreateICmpNE(
                builder.CreateAnd(
                    activity_value,
                    llvm::ConstantInt::get(
                        byte_type, static_cast<std::uint8_t>(activity))),
                llvm::ConstantInt::get(byte_type, 0),
                "background.batch.active");
            auto* invoke = llvm::BasicBlock::Create(
                context, "background.batch.collect", function);
            auto* next = llvm::BasicBlock::Create(
                context, "background.batch.next", function);
            builder.CreateCondBr(active, invoke, next);
            builder.SetInsertPoint(invoke);
            emit(node_frame);
            builder.CreateBr(next);
            builder.SetInsertPoint(next);
        };

        for (auto const& [node, primitive] : candidates) {
            emit_if_active(node, [&](llvm::Value* node_frame) {
                if (phase == BackgroundRootOperation::evaluate) {
                    emit_operation_hook(
                        node_frame,
                        offsets.prepare_operations,
                        "background.batch.prepare");
                }
                auto* count = builder.CreateLoad(
                    size_type, active_count, "background.batch.lane.index");
                auto* lane = builder.CreateInBoundsGEP(
                    byte_type,
                    lane_storage,
                    builder.CreateMul(
                        count, size_constant(lane_size),
                        "background.batch.lane.offset"),
                    "background.batch.lane");
                builder.CreateStore(
                    configurations[primitive].node_config,
                    byte_offset_pointer(
                        builder,
                        lane,
                        offsetof(ReflectedNodeForwardCoverageBatchLane, node_data),
                        "background.batch.node.pointer"));
                builder.CreateMemCpy(
                    byte_offset_pointer(
                        builder,
                        lane,
                        lane_context_offset,
                        "background.batch.context.pointer"),
                    llvm::Align(1),
                    byte_offset_pointer(
                        builder,
                        node_frame,
                        scalar_context_offset,
                        "background.batch.scalar.context"),
                    llvm::Align(1),
                    lane_size - lane_context_offset);
                builder.CreateStore(
                    builder.CreateAdd(
                        count, size_constant(1),
                        "background.batch.active.next"),
                    active_count);
            });
        }

        auto* count = builder.CreateLoad(
            size_type, active_count, "background.batch.active.total");
        auto* invoke = llvm::BasicBlock::Create(
            context, "background.batch.invoke", function);
        auto* next = llvm::BasicBlock::Create(
            context, "background.batch.complete", function);
        builder.CreateCondBr(
            builder.CreateICmpNE(
                count, size_constant(0), "background.batch.nonempty"),
            invoke,
            next);
        builder.SetInsertPoint(invoke);
        auto* batch_context = builder.CreateAlloca(
            byte_type, size_constant(batch_size), "background.batch.context");
        batch_context->setAlignment(llvm::Align(batch_alignment));
        store_context_span_pointer(
            builder,
            batch_context,
            offsetof(ReflectedNodeForwardCoverageBatchContext, lanes)
                + offsetof(
                    ReflectedSpan<ReflectedNodeForwardCoverageBatchLane const>,
                    pointer),
            offsetof(ReflectedNodeForwardCoverageBatchContext, lanes)
                + offsetof(
                    ReflectedSpan<ReflectedNodeForwardCoverageBatchLane const>,
                    extent),
            lane_storage,
            0);
        builder.CreateStore(
            count,
            byte_offset_pointer(
                builder,
                batch_context,
                offsetof(ReflectedNodeForwardCoverageBatchContext, lanes)
                    + offsetof(
                        ReflectedSpan<
                            ReflectedNodeForwardCoverageBatchLane const>,
                        extent),
                "background.batch.extent.pointer"));
        auto* call = builder.CreateCall(
            batch_operation_type(context), imported, {batch_context});
        call->setCallingConv(imported->getCallingConv());
        builder.CreateBr(next);
        builder.SetInsertPoint(next);

        if (phase == BackgroundRootOperation::evaluate) {
            for (auto const& [node, primitive] : candidates) {
                (void)primitive;
                emit_if_active(node, [&](llvm::Value* node_frame) {
                    emit_operation_hook(
                        node_frame,
                        offsets.finalize_operations,
                        "background.batch.finalize");
                });
            }
        }
        return true;
    };

    auto emit_node = [&](BackgroundNodeIndex node)
        -> std::expected<void, std::string> {
        if (node >= plan.connections.background.nodes.size()) {
            return std::unexpected(
                "GraphJit background execution order references a missing node");
        }
        auto const& background_node = plan.connections.background.nodes[node];
        auto const applicable = phase == BackgroundRootOperation::forward
            ? background_node.authored_tock_execution
                || background_node.uses_replay_forward_coverage
            : phase == BackgroundRootOperation::reverse
                ? (background_node.authored_tock_execution
                      && background_node.accumulators.input_requirement_count != 0)
                    || background_node.uses_replay_reverse_coverage
                : background_node.authored_tock_execution
                    || background_node.replays_tick;
        if (!applicable) return {};

        auto primitive = primitive_for(node);
        if (!primitive) return std::unexpected(std::move(primitive.error()));
        auto const& callbacks = plan.imports.primitive_callbacks[*primitive];

        if (phase == BackgroundRootOperation::forward) {
            if (background_node.authored_tock_execution) {
                if (callbacks.propagate_forward_coverage.empty()) {
                    return std::unexpected(
                        "GraphJit background forward program has no authored callback");
                }
                emit_when_active(
                    node,
                    BackgroundNodeActivity::forward,
                    [&](llvm::Value* node_frame) {
                        emit_authored(
                            node_frame,
                            *primitive,
                            callbacks.propagate_forward_coverage,
                            offsets.forward_context);
                    });
            } else if (background_node.uses_replay_forward_coverage) {
                emit_when_active(
                    node,
                    BackgroundNodeActivity::forward,
                    [&](llvm::Value* node_frame) {
                        emit_generated(
                            node_frame,
                            offsets.replay_forward,
                            offsets.forward_context);
                    });
            }
            return {};
        }

        if (phase == BackgroundRootOperation::reverse) {
            if (background_node.authored_tock_execution
                && background_node.accumulators.input_requirement_count != 0) {
                if (callbacks.propagate_reverse_coverage.empty()) {
                    return std::unexpected(
                        "GraphJit background reverse program has no authored callback");
                }
                emit_when_active(
                    node,
                    BackgroundNodeActivity::reverse,
                    [&](llvm::Value* node_frame) {
                        emit_authored(
                            node_frame,
                            *primitive,
                            callbacks.propagate_reverse_coverage,
                            offsets.reverse_context);
                    });
            } else if (background_node.uses_replay_reverse_coverage) {
                emit_when_active(
                    node,
                    BackgroundNodeActivity::reverse,
                    [&](llvm::Value* node_frame) {
                        emit_generated(
                            node_frame,
                            offsets.replay_reverse,
                            offsets.reverse_context);
                    });
            }
            return {};
        }

        if (background_node.authored_tock_execution) {
            if (callbacks.tock_coverage.empty()) {
                return std::unexpected(
                    "GraphJit background evaluation program has no authored Tock callback");
            }
            emit_when_active(
                node,
                BackgroundNodeActivity::evaluate,
                [&](llvm::Value* node_frame) {
                    emit_authored(
                        node_frame,
                        *primitive,
                        callbacks.tock_coverage,
                        offsets.tock_context);
                });
            return {};
        }

        if (!background_node.uses_imported_tick_block_for_replay
            || callbacks.tick_block.empty()) {
            return std::unexpected(
                "GraphJit replay program has no imported tick_block callback");
        }
        auto* tick = module.getFunction(callbacks.tick_block);
        emit_when_active(
            node,
            BackgroundNodeActivity::evaluate,
            [&](llvm::Value* node_frame) {
                auto* regions = builder.CreateLoad(
                    pointer_type,
                    byte_offset_pointer(
                        builder,
                        node_frame,
                        offsets.replay_regions_data,
                        "replay.regions.index"),
                    "replay.regions");
                auto* count = builder.CreateLoad(
                    size_type,
                    byte_offset_pointer(
                        builder,
                        node_frame,
                        offsets.replay_regions_size,
                        "replay.region.count.index"),
                    "replay.region.count");
                auto* preheader = builder.GetInsertBlock();
                auto* loop = llvm::BasicBlock::Create(
                    context, "replay.region", function);
                auto* invoke = llvm::BasicBlock::Create(
                    context, "replay.invoke", function);
                auto* advance = llvm::BasicBlock::Create(
                    context, "replay.advance", function);
                auto* done = llvm::BasicBlock::Create(
                    context, "replay.done", function);
                auto* nonempty = builder.CreateICmpNE(
                    count,
                    llvm::ConstantInt::get(size_type, 0),
                    "replay.nonempty");
                builder.CreateCondBr(nonempty, loop, done);

                builder.SetInsertPoint(loop);
                auto* index = builder.CreatePHI(
                    size_type, 2, "replay.region.index");
                index->addIncoming(
                    llvm::ConstantInt::get(size_type, 0), preheader);
                auto* region = builder.CreateInBoundsGEP(
                    byte_type,
                    regions,
                    builder.CreateMul(
                        index,
                        llvm::ConstantInt::get(
                            size_type, sizeof(GlobalIndexRange))),
                    "replay.region.pointer");
                auto* begin = builder.CreateLoad(
                    size_type,
                    byte_offset_pointer(
                        builder,
                        region,
                        offsetof(GlobalIndexRange, begin),
                        "replay.begin.index"),
                    "replay.begin");
                auto* extent = builder.CreateLoad(
                    size_type,
                    byte_offset_pointer(
                        builder,
                        region,
                        offsetof(GlobalIndexRange, extent),
                        "replay.extent.index"),
                    "replay.extent");
                builder.CreateCondBr(
                    builder.CreateICmpNE(
                        extent,
                        llvm::ConstantInt::get(size_type, 0),
                        "replay.region.nonempty"),
                    invoke,
                    advance);

                builder.SetInsertPoint(invoke);
                builder.CreateCall(
                    primitive_block_operation_type(context),
                    tick,
                    {configurations[*primitive].node_config,
                     byte_offset_pointer(
                         builder,
                         node_frame,
                         offsets.replay_tick_context,
                         "replay.context"),
                     begin,
                     extent});
                builder.CreateBr(advance);

                builder.SetInsertPoint(advance);
                auto* next = builder.CreateAdd(
                    index,
                    llvm::ConstantInt::get(size_type, 1),
                    "replay.region.next");
                auto* complete = builder.CreateICmpUGE(
                    next, count, "replay.complete");
                builder.CreateCondBr(complete, done, loop);
                index->addIncoming(next, advance);
                builder.SetInsertPoint(done);
            });
        return {};
    };

    auto const& ready_groups = phase == BackgroundRootOperation::forward
        ? plan.connections.background.forward_ready_groups
        : phase == BackgroundRootOperation::reverse
            ? plan.connections.background.reverse_ready_groups
            : plan.connections.background.evaluation_ready_groups;
    for (auto const& group : ready_groups) {
        auto batched = try_emit_authored_batch(
            std::span<BackgroundNodeIndex const>{group});
        if (!batched) return std::unexpected(std::move(batched.error()));
        if (!*batched) {
            for (auto const node : group) {
                auto emitted = emit_node(node);
                if (!emitted) {
                    return std::unexpected(std::move(emitted.error()));
                }
            }
        }
    }

    builder.CreateRetVoid();
    return function;
}

std::expected<llvm::Function*, std::string>
define_sequential_input_combiner(
    llvm::Module& module,
    detail::LoweringPlan const& plan)
{
    auto& context = module.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* function_type = llvm::FunctionType::get(
        llvm::Type::getInt1Ty(context),
        {size_type, pointer_type, pointer_type, size_type},
        false);
    auto* function = llvm::Function::Create(
        function_type,
        llvm::GlobalValue::ExternalLinkage,
        llvm::StringRef(
            root_combine_sequential_input_symbol.data(),
            root_combine_sequential_input_symbol.size()),
        module);
    function->setCallingConv(llvm::CallingConv::C);

    auto argument = function->arg_begin();
    llvm::Value* port_index = &*argument++;
    llvm::Value* destination = &*argument++;
    llvm::Value* source = &*argument++;
    llvm::Value* byte_count = &*argument;
    port_index->setName("port");
    destination->setName("destination");
    source->setName("source");
    byte_count->setName("byte_count");

    auto* entry = llvm::BasicBlock::Create(context, "entry", function);
    auto* rejected = llvm::BasicBlock::Create(context, "rejected", function);
    llvm::IRBuilder<> builder(entry);
    auto* select = builder.CreateSwitch(
        port_index,
        rejected,
        plan.connections.background.ports.size());
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };

    for (BackgroundPortIndex port = 0;
         port < plan.connections.background.ports.size(); ++port) {
        auto const& background_port =
            plan.connections.background.ports[port];
        if (background_port.kind != PortKind::sample
            || background_port.direction != PortDirection::input
            || background_port.input_access != InputAccessKind::sequential) {
            continue;
        }
        if (background_port.node
            >= plan.connections.background.nodes.size()) {
            return std::unexpected(
                "GraphJit sequential input combiner references a missing node");
        }
        auto const bundle = plan.connections.background
            .nodes[background_port.node].bundle;
        auto const primitive = std::ranges::find(
            plan.imports.primitive_callbacks,
            bundle,
            &detail::PrimitiveCallbackPlan::bundle);
        if (primitive == plan.imports.primitive_callbacks.end()
            || primitive->combine_sequential_input_values.empty()) {
            return std::unexpected(
                "GraphJit sequential input has no imported combination callback");
        }
        auto* callback = module.getFunction(primitive->combine_sequential_input_values);
        if (!callback) {
            return std::unexpected(
                "GraphJit sequential input combination callback is not imported");
        }

        auto* selected = llvm::BasicBlock::Create(
            context,
            "combine.port." + std::to_string(port),
            function);
        select->addCase(constant(port), selected);
        builder.SetInsertPoint(selected);
        auto* accepted = builder.CreateCall(
            callback,
            {constant(background_port.configured_port.port_index),
             llvm::ConstantInt::get(
                 llvm::Type::getInt8Ty(context),
                 static_cast<std::uint8_t>(
                     background_port.continuous_geometry.value_type)),
             destination,
             source,
             byte_count,
             constant(background_port.continuous_geometry.elements_per_value)});
        accepted->setCallingConv(callback->getCallingConv());
        builder.CreateRet(accepted);
    }

    builder.SetInsertPoint(rejected);
    builder.CreateRet(llvm::ConstantInt::getFalse(context));
    return function;
}

std::expected<llvm::Function*, std::string>
define_sequential_input_initializer(
    llvm::Module& module,
    detail::LoweringPlan const& plan)
{
    auto& context = module.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* function_type = llvm::FunctionType::get(
        llvm::Type::getInt1Ty(context),
        {size_type, pointer_type, size_type},
        false);
    auto* function = llvm::Function::Create(
        function_type,
        llvm::GlobalValue::ExternalLinkage,
        llvm::StringRef(
            root_initialize_sequential_input_symbol.data(),
            root_initialize_sequential_input_symbol.size()),
        module);
    function->setCallingConv(llvm::CallingConv::C);

    auto argument = function->arg_begin();
    llvm::Value* port_index = &*argument++;
    llvm::Value* destination = &*argument++;
    llvm::Value* byte_count = &*argument;
    port_index->setName("port");
    destination->setName("destination");
    byte_count->setName("byte_count");

    auto* entry = llvm::BasicBlock::Create(context, "entry", function);
    auto* rejected = llvm::BasicBlock::Create(context, "rejected", function);
    llvm::IRBuilder<> builder(entry);
    auto* select = builder.CreateSwitch(
        port_index,
        rejected,
        plan.connections.background.ports.size());
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };

    for (BackgroundPortIndex port = 0;
         port < plan.connections.background.ports.size(); ++port) {
        auto const& background_port =
            plan.connections.background.ports[port];
        if (background_port.kind != PortKind::sample
            || background_port.direction != PortDirection::input
            || background_port.input_access != InputAccessKind::sequential) {
            continue;
        }
        if (background_port.node
            >= plan.connections.background.nodes.size()) {
            return std::unexpected(
                "GraphJit sequential input initializer references a missing node");
        }
        auto const bundle = plan.connections.background
            .nodes[background_port.node].bundle;
        auto const primitive = std::ranges::find(
            plan.imports.primitive_callbacks,
            bundle,
            &detail::PrimitiveCallbackPlan::bundle);
        if (primitive == plan.imports.primitive_callbacks.end()
            || primitive->initialize_sequential_input.empty()) {
            return std::unexpected(
                "GraphJit sequential input has no imported neutral initializer");
        }
        auto* callback = module.getFunction(
            primitive->initialize_sequential_input);
        if (!callback) {
            return std::unexpected(
                "GraphJit sequential input neutral initializer is not imported");
        }

        auto* selected = llvm::BasicBlock::Create(
            context,
            "initialize.port." + std::to_string(port),
            function);
        select->addCase(constant(port), selected);
        builder.SetInsertPoint(selected);
        auto* accepted = builder.CreateCall(
            callback,
            {constant(background_port.configured_port.port_index),
             llvm::ConstantInt::get(
                 llvm::Type::getInt8Ty(context),
                 static_cast<std::uint8_t>(
                     background_port.continuous_geometry.value_type)),
             llvm::ConstantInt::get(
                 llvm::Type::getInt8Ty(context),
                 static_cast<std::uint8_t>(
                     iv::details::SequentialInputInitialization::neutral)),
             destination,
             byte_count,
             constant(background_port.continuous_geometry.elements_per_value)});
        accepted->setCallingConv(callback->getCallingConv());
        builder.CreateRet(accepted);
    }

    builder.SetInsertPoint(rejected);
    builder.CreateRet(llvm::ConstantInt::getFalse(context));
    return function;
}

struct TransitionLoweringPlan {
    detail::LoweringPlan root_plan{};
    LoweredGraphTransition output{};
};

std::expected<std::optional<TransitionLoweringPlan>, std::string>
specialize_transition_plan(
    detail::LoweringPlan& steady_plan,
    LoweringTransitionInput const* input)
{
    if (input == nullptr) return std::optional<TransitionLoweringPlan>{};

    auto state_transition = plan_realtime_port_state_transition(
        input->predecessor_requirements,
        input->predecessor_realizations,
        steady_plan.connections.realtime_port_states,
        steady_plan.realtime_port_state_realizations);
    if (!state_transition) {
        return std::unexpected(std::move(state_transition.error()));
    }

    auto const needs_transition_root = std::ranges::any_of(
        state_transition->sample_states,
        [](RealtimePortStateTransition const& transition) {
            return transition.inherited_state_realization
                == InheritedRealtimePortStateRealization::
                    transition_only_storage;
        }) || std::ranges::any_of(
        state_transition->sample_transfers,
        [](SampleRealtimePortStateTransfer const& transfer) {
            return transfer.composition_source.has_value();
        }) || std::ranges::any_of(
        state_transition->event_states,
        [](RealtimePortStateTransition const& transition) {
            return transition.inherited_state_realization
                == InheritedRealtimePortStateRealization::
                    transition_only_storage;
        });

    if (!needs_transition_root) {
        return std::optional<TransitionLoweringPlan>{TransitionLoweringPlan{
            .output = {
                .state_transition = std::move(*state_transition),
                .realtime_port_state_realizations =
                    steady_plan.realtime_port_state_realizations,
            },
        }};
    }

    auto transition_plan = steady_plan;
    auto realized = realize_realtime_port_state_transition_storage(
        transition_plan.declarations.node_layout,
        std::move(*state_transition),
        input->predecessor_realizations,
        steady_plan.connections.realtime_port_states,
        steady_plan.realtime_port_state_realizations);
    if (!realized) return std::unexpected(std::move(realized.error()));

    // A shared steady event sequence is deliberately not the destination of
    // any transition-only semantic state. Start that sequence fresh so its
    // normal callback operations accumulate only successor events. The private
    // suffix regions receive predecessor state through event_transfers.
    for (auto const& transition : realized->plan.event_states) {
        if (transition.inherited_state_realization
                != InheritedRealtimePortStateRealization::
                    transition_only_storage
            || transition.current_realization_index
                >= steady_plan.realtime_port_state_realizations
                    .event_states.size()) {
            continue;
        }
        auto const& current = steady_plan.realtime_port_state_realizations
            .event_states[transition.current_realization_index];
        if (!current.retained_storage
            || !current.retained_storage->node_storage_offset) {
            continue;
        }
        auto const offset = *current.retained_storage->node_storage_offset;
        bool found = false;
        for (auto& region : realized->node_layout.regions) {
            if (!region.transition_only
                && region.kind == NodeLayout::Region::Kind::raw
                && region.storage_offset == offset
                && region.size == current.retained_storage->size_bytes
                && region.alignment == current.retained_storage->alignment) {
                if (found) {
                    return std::unexpected(
                        "GraphJit transition event state has duplicate steady raw storage");
                }
                found = true;
                region.migration_identity.clear();
            }
        }
        if (!found) {
            return std::unexpected(
                "GraphJit transition event state lost its steady raw storage");
        }
    }

    transition_plan.declarations.node_layout = realized->node_layout;
    steady_plan.declarations.node_layout = realized->node_layout;
    auto transition_realizations = steady_plan.realtime_port_state_realizations;

    auto primitive_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<std::size_t> {
        for (std::size_t index = 0;
             index < transition_plan.imports.primitive_callbacks.size();
             ++index) {
            if (transition_plan.imports.primitive_callbacks[index].bundle
                == bundle) {
                return index;
            }
        }
        return std::nullopt;
    };
    auto execution_position_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<std::size_t> {
        auto const& schedule = transition_plan.connections.schedule;
        if (bundle < schedule.bundle_execution_position.size()
            && schedule.bundle_execution_position[bundle]) {
            return schedule.bundle_execution_position[bundle];
        }
        if (bundle >= schedule.callback_elided.size()
            || schedule.callback_elided[bundle] == 0) {
            return std::nullopt;
        }
        std::optional<std::size_t> first_consumer;
        for (auto const& connection :
             transition_plan.connections.sample_connections) {
            auto const uses_source = std::ranges::any_of(
                connection.source_channel_timings,
                [&](detail::SampleSourceChannelTimingPlan const& source) {
                    return source.source.bundle == bundle
                        && detail::uses_realtime_storage(source.delivery);
                });
            if (!uses_source) continue;
            auto const target = connection.target_port.node_bundle_handle;
            if (target >= schedule.bundle_execution_position.size()
                || !schedule.bundle_execution_position[target]) {
                continue;
            }
            auto const position = *schedule.bundle_execution_position[target];
            first_consumer = first_consumer
                ? std::min(*first_consumer, position)
                : std::optional<std::size_t>{position};
        }
        return first_consumer;
    };
    auto region_for_view = [&](SampleRealtimePortStateStorageView const& view)
        -> std::optional<NodeLayout::RegionHandle> {
        if (!view.node_storage_offset) return std::nullopt;
        for (std::size_t index = 0;
             index < realized->node_layout.regions.size(); ++index) {
            auto const& region = realized->node_layout.regions[index];
            if (region.transition_only
                && region.kind == NodeLayout::Region::Kind::raw
                && region.storage_offset == *view.node_storage_offset
                && region.size == view.storage_size_bytes) {
                return NodeLayout::RegionHandle{.index = index};
            }
        }
        return std::nullopt;
    };
    auto event_region_for_view = [&](EventRealtimePortStateStorageView const& view)
        -> std::optional<NodeLayout::RegionHandle> {
        if (!view.node_storage_offset) return std::nullopt;
        for (std::size_t index = 0;
             index < realized->node_layout.regions.size(); ++index) {
            auto const& region = realized->node_layout.regions[index];
            if (region.transition_only
                && region.kind == NodeLayout::Region::Kind::raw
                && region.storage_offset == *view.node_storage_offset
                && region.size == view.size_bytes) {
                return NodeLayout::RegionHandle{.index = index};
            }
        }
        return std::nullopt;
    };

    for (std::size_t transition_index = 0;
         transition_index < realized->plan.sample_states.size();
         ++transition_index) {
        auto const& transition = realized->plan.sample_states[transition_index];
        if (transition.inherited_state_realization
            != InheritedRealtimePortStateRealization::
                transition_only_storage) {
            continue;
        }
        if (transition.current_requirement_index
                >= transition_plan.connections.realtime_port_states.states.size()
            || transition.current_realization_index
                >= transition_realizations.sample_states.size()) {
            return std::unexpected(
                "GraphJit transition sample state lost its successor metadata");
        }
        auto const& requirement = transition_plan.connections
            .realtime_port_states.states[transition.current_requirement_index];
        auto const input_history = requirement.direction
                == PortDirection::input
            && requirement.role
                == RealtimePortStateRole::sequential_input_history;
        // A sample output owns one window of past values (RealtimePortStateRole).
        auto const output_window = requirement.direction
                == PortDirection::output
            && requirement.role
                == RealtimePortStateRole::tick_output_history;
        if ((!input_history && !output_window) || !requirement.channel) {
            return std::unexpected(
                "GraphJit transition suffix has an unsupported sample-state role");
        }

        auto const transfer = std::ranges::find_if(
            realized->plan.sample_transfers,
            [&](SampleRealtimePortStateTransfer const& candidate) {
                return candidate.transition_index == transition_index
                    && candidate.target_transition_only;
            });
        if (transfer == realized->plan.sample_transfers.end()) {
            return std::unexpected(
                "GraphJit transition sample state lost its suffix transfer");
        }
        auto const& current_realization = steady_plan
            .realtime_port_state_realizations.sample_states[
                transition.current_realization_index];
        if ((current_realization.retained_access
                    == SampleRealtimePortStateAccess::direct
                && current_realization.retained_sources.size() != 1)
            || (current_realization.retained_access
                    != SampleRealtimePortStateAccess::direct
                && current_realization.retained_access
                    != SampleRealtimePortStateAccess::materialized
                && current_realization.retained_access
                    != SampleRealtimePortStateAccess::composed
                && current_realization.retained_access
                    != SampleRealtimePortStateAccess::immutable_constant)) {
            return std::unexpected(
                "GraphJit transition sample state has an unsupported steady realization");
        }
        auto const target_primitive = primitive_for_bundle(
            requirement.configured_port.node_bundle_handle);
        if (!target_primitive
            || *target_primitive
                >= transition_plan.sample_ports.primitives.size()) {
            return std::unexpected(
                "GraphJit transition sample state lost its authored primitive");
        }
        if (input_history
            && current_realization.retained_access
                == SampleRealtimePortStateAccess::immutable_constant
            && *target_primitive
                < transition_plan.declarations.primitive_activity.size()
            && transition_plan.declarations
                .primitive_activity[*target_primitive]) {
            auto& activity = *transition_plan.declarations
                .primitive_activity[*target_primitive];
            if (requirement.configured_port.port_index
                >= activity.continuous_inputs.size()) {
                return std::unexpected(
                    "GraphJit transition immutable input lost its activity plan");
            }
            activity.continuous_inputs[
                requirement.configured_port.port_index]
                .changed_during_transition = true;
        }
        std::optional<std::size_t> source_representation;
        if (input_history) {
            auto const& steady_inputs = steady_plan.sample_ports
                .primitives[*target_primitive].inputs;
            if (requirement.configured_port.port_index
                    >= steady_inputs.size()
                || *requirement.channel
                    >= steady_inputs[
                        requirement.configured_port.port_index]
                        .channels.size()) {
                return std::unexpected(
                    "GraphJit transition input state lost its steady binding");
            }
            source_representation = steady_inputs[
                requirement.configured_port.port_index]
                .channels[*requirement.channel].storage;
        } else {
            auto const& steady_ports = steady_plan.sample_ports
                .primitives[*target_primitive];
            if (requirement.configured_port.port_index
                    >= steady_ports.outputs.size()
                || !steady_ports.outputs[
                    requirement.configured_port.port_index].storage) {
                return std::unexpected(
                    "GraphJit transition output state lost its steady binding");
            }
            source_representation = *steady_ports.outputs[
                requirement.configured_port.port_index].storage;
        }
        auto const suffix_region = region_for_view(transfer->target_storage);
        if (!source_representation || !suffix_region) {
            return std::unexpected(
                "GraphJit transition sample state lost its source or suffix storage");
        }

        auto const source = transition_plan.sample_ports.storage
            .representations[*source_representation];
        auto const target_position = execution_position_for_bundle(
            requirement.configured_port.node_bundle_handle);
        std::optional<std::size_t> source_position;
        bool source_after_composition = false;
        bool source_before_consumer_materialization = false;
        bool source_before_consumer_composition = false;
        if (output_window) {
            source_position = target_position;
            source_before_consumer_materialization =
                source.constant_value.has_value();
        } else {
            auto const materialization = std::ranges::find_if(
                transition_plan.sample_ports.storage.materializations,
                [&](detail::SampleMaterializationPlan const& candidate) {
                    return candidate.target_representation
                        == *source_representation;
                });
            auto const composition = std::ranges::find_if(
                transition_plan.sample_ports.storage.compositions,
                [&](detail::SampleCompositionPlan const& candidate) {
                    return candidate.target_representation
                        == *source_representation;
                });
            if (materialization
                != transition_plan.sample_ports.storage
                    .materializations.end()) {
                if (materialization->before_execution_position) {
                    source_position =
                        materialization->before_execution_position;
                    source_before_consumer_materialization = true;
                } else {
                    source_position = materialization->after_execution_position;
                }
            } else if (composition
                       != transition_plan.sample_ports.storage
                           .compositions.end()) {
                source_position = composition->before_execution_position
                    ? composition->before_execution_position
                    : std::optional<std::size_t>{
                        composition->after_execution_position};
                // A composition that runs before its consumer is written in
                // the consumer's own step, before its callback.
                source_before_consumer_composition =
                    composition->before_execution_position.has_value();
                source_after_composition =
                    !source_before_consumer_composition;
            } else {
                if (source.producer_group_index
                        >= transition_plan.connections.sample_producer_groups.size()) {
                    return std::unexpected(
                        "GraphJit transition input suffix lost its producer group");
                }
                auto const& producer_group = transition_plan.connections
                    .sample_producer_groups[source.producer_group_index];
                if (!producer_group.source_port) {
                    return std::unexpected(
                        "GraphJit transition suffix lost its source producer");
                }
                source_position = execution_position_for_bundle(
                    producer_group.source_port->node_bundle_handle);
                source_before_consumer_materialization =
                    source.constant_value.has_value();
            }
        }
        if (!source_position || !target_position
            || *source_position
                >= transition_plan.execution.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit transition suffix lost its execution position");
        }
        auto& target_ports = transition_plan.sample_ports
            .primitives[*target_primitive];
        // Delay at which the consumer reads this channel of its steady input
        // representation: physical index - read_latency - frame_delay.
        std::size_t input_read_delay = 0;
        // Delay at which the input's mirror copies its source's values.
        std::size_t mirror_delay = 0;
        // The first frame the mirror may write, relative to the cutover's
        // sequential index, when its first copy would otherwise begin earlier.
        std::optional<std::int64_t> mirror_first_frame;
        if (input_history) {
            if (requirement.configured_port.port_index
                >= target_ports.inputs.size()) {
                return std::unexpected(
                    "GraphJit transition sample state lost its authored input port");
            }
            auto const& steady_binding = steady_plan.sample_ports
                .primitives[*target_primitive]
                .inputs[requirement.configured_port.port_index];
            if (*requirement.channel >= steady_binding.channels.size()
                || steady_binding.channels[*requirement.channel].storage
                    != *source_representation
                || steady_binding.channels[*requirement.channel]
                        .representation_channel
                    != current_realization.authored_storage
                        .representation_channel) {
                return std::unexpected(
                    "GraphJit transition sample state disagrees with its authored input binding");
            }
            auto const frame_delay =
                steady_binding.channels[*requirement.channel].frame_delay;
            if (frame_delay
                > std::numeric_limits<std::size_t>::max()
                    - steady_binding.read_latency) {
                return std::unexpected(
                    "GraphJit transition input suffix read delay overflows size_t");
            }
            input_read_delay = steady_binding.read_latency + frame_delay;
            if (input_read_delay
                > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                return std::unexpected(
                    "GraphJit transition input suffix read delay is not representable");
            }
            // The window holds positions [-history, T), where the source's
            // values resume. Position T lies at frame P + T - d of the suffix,
            // where P is the cutover's sequential index and d the read delay.
            auto const resume = transfer->target_end;
            auto const delay = static_cast<std::int64_t>(input_read_delay);
            if (resume < 0
                || transfer->target_storage.timeline_offset_frames != -delay) {
                return std::unexpected(
                    "GraphJit transition input window disagrees with its read delay");
            }
            auto const window_end = resume - delay;
            if (current_realization.retained_access
                == SampleRealtimePortStateAccess::direct) {
                if (resume > delay) {
                    return std::unexpected(
                        "GraphJit transition input window ends outside its read delay");
                }
                // A direct input's source finalizes each value
                // L = d - in_flight frames before the consumer's newest
                // position. The mirror copies each value as its source
                // finalizes it, but not before T: d - min(T, in_flight)
                // frames back. When T is at most in_flight, its first copy
                // begins exactly where the window ends.
                mirror_delay = input_read_delay
                    - std::min<std::size_t>(
                        static_cast<std::size_t>(resume),
                        requirement.in_flight_samples);
            } else {
                // A converted or composed steady representation holds only the
                // frames its consumer reads in the current block.
                mirror_delay = input_read_delay;
            }
            // When the source's latency grew (T beyond in_flight), or the
            // representation is converted or composed, the first copies would
            // begin inside the window: the mirror skips frames before its end.
            if (-static_cast<std::int64_t>(mirror_delay) < window_end) {
                mirror_first_frame = window_end;
            }
        } else {
            auto const& steady_outputs = steady_plan.sample_ports
                .primitives[*target_primitive].outputs;
            if (requirement.configured_port.port_index
                    >= steady_outputs.size()
                || !steady_outputs[
                    requirement.configured_port.port_index].storage
                || *steady_outputs[
                    requirement.configured_port.port_index].storage
                    != *source_representation) {
                return std::unexpected(
                    "GraphJit transition sample state disagrees with its authored output binding");
            }
        }

        auto& sample_storage = transition_plan.sample_ports.storage;
        std::optional<std::size_t> suffix_representation;
        for (auto const& allocation : sample_storage.persistent_allocations) {
            if (allocation.region != *suffix_region) continue;
            if (suffix_representation
                || allocation.representation_index
                    >= sample_storage.representations.size()
                || allocation.kind
                    != detail::SamplePersistentStorageKind::ring
                || allocation.storage_offset
                    != *transfer->target_storage.node_storage_offset
                || allocation.size_bytes
                    != transfer->target_storage.storage_size_bytes) {
                return std::unexpected(
                    "GraphJit transition suffix region has inconsistent sample representations");
            }
            suffix_representation = allocation.representation_index;
        }
        auto const new_suffix = !suffix_representation;
        if (new_suffix) {
            suffix_representation = sample_storage.representations.size();
            auto suffix = source;
            suffix.canonical_producer_representation = false;
            suffix.storage = RealtimeBufferStorageKind::full_node_storage;
            suffix.channel_layout = transfer->target_storage.channel_layout;
            suffix.geometry = transfer->target_storage.geometry;
            suffix.absolute_pace_exponent =
                transfer->target_storage.absolute_pace_exponent;
            suffix.frame_capacity =
                transfer->target_storage.working_frame_capacity;
            suffix.transient_allocation = detail::no_sample_transient_allocation;
            suffix.persistent_allocation =
                sample_storage.persistent_allocations.size();
            suffix.constant_value.reset();
            sample_storage.representations.push_back(std::move(suffix));
            sample_storage.persistent_allocations.push_back(
                detail::SamplePersistentAllocationPlan{
                    .representation_index = *suffix_representation,
                    .kind = detail::SamplePersistentStorageKind::ring,
                    .channel_layout = transfer->target_storage.channel_layout,
                    .geometry = transfer->target_storage.geometry,
                    .frame_capacity =
                        transfer->target_storage.working_frame_capacity,
                    .size_bytes = transfer->target_storage.storage_size_bytes,
                    .alignment =
                        transfer->target_storage.geometry.element_alignment,
                    .region = *suffix_region,
                    .storage_offset =
                        *transfer->target_storage.node_storage_offset,
                });

            auto const materialization_index =
                sample_storage.materializations.size();
            auto mirror = detail::SampleMaterializationPlan{
                .source_representation = input_history
                    ? *source_representation
                    : *suffix_representation,
                .target_representation = input_history
                    ? *suffix_representation
                    : *source_representation,
                .source_layout = source.channel_layout,
                .target_layout = source.channel_layout,
            };
            if (source_before_consumer_materialization
                || source_before_consumer_composition) {
                mirror.before_execution_position = *source_position;
            } else {
                mirror.after_execution_position = *source_position;
            }
            if (output_window) {
                // The producer may revise its newest latency values through
                // OutputPort::update(), so the steady representation receives
                // those revisions together with the new block.
                mirror.retained_before = target_ports.outputs[
                    requirement.configured_port.port_index].latency;
            } else {
                // The consumer reads its suffix where it would read the steady
                // representation. Its window already holds its inherited and
                // fresh positions, so copy only the values its source finalized
                // in this block. A latent producer finalized them before this
                // copy runs; values it authored in this block may still be
                // provisional and are copied once mirror_delay reaches them.
                mirror.retained_before = mirror_delay;
                mirror.latest_read_latency = mirror_delay;
                mirror.transition_channels = {
                    transfer->target_storage.representation_channel};
                if (mirror_first_frame) {
                    auto const cursor = transfer->target_storage
                        .sequential_cursor_node_storage_offset;
                    auto const cell = std::ranges::find_if(
                        realized->plan.sample_cutover_indices,
                        [&](SampleRealtimePortStateCutoverIndex const& index) {
                            return cursor
                                && index.sequential_cursor_node_storage_offset
                                    == *cursor;
                        });
                    if (cell == realized->plan.sample_cutover_indices.end()) {
                        return std::unexpected(
                            "GraphJit transition input suffix lost its cutover index cell");
                    }
                    mirror.transition_cutover_index_storage_offset =
                        cell->node_storage_offset;
                    mirror.transition_first_frame = *mirror_first_frame;
                }
            }
            sample_storage.materializations.push_back(std::move(mirror));
            auto& source_step = transition_plan.execution
                .primitive_steps[*source_position];
            auto const source_is_feedback_timeline = input_history
                && std::ranges::any_of(
                    sample_storage.feedback_timelines,
                    [&](detail::SampleFeedbackTimelinePlan const& timeline) {
                        return timeline.timeline_representation
                            == *source_representation;
                    });
            if (source_before_consumer_composition) {
                source_step.sample_transition_materializations_before
                    .push_back(materialization_index);
            } else if (source_is_feedback_timeline || source_after_composition) {
                source_step.sample_transition_materializations_after
                    .push_back(materialization_index);
            } else if (source_before_consumer_materialization) {
                source_step.sample_materializations_before.push_back(
                    materialization_index);
            } else if (output_window) {
                source_step.sample_materializations_after.insert(
                    source_step.sample_materializations_after.begin(),
                    materialization_index);
            } else {
                source_step.sample_materializations_after.push_back(
                    materialization_index);
            }
        } else if (input_history) {
            // One mirror copies one window for every channel sharing this
            // suffix, so those channels must be read at the same delay.
            auto const mirror = std::ranges::find(
                sample_storage.materializations,
                *suffix_representation,
                &detail::SampleMaterializationPlan::target_representation);
            if (mirror == sample_storage.materializations.end()
                || mirror->retained_before != mirror_delay
                || mirror->latest_read_latency != mirror_delay
                || mirror->transition_cutover_index_storage_offset.has_value()
                    != mirror_first_frame.has_value()
                || (mirror_first_frame
                    && mirror->transition_first_frame
                        != *mirror_first_frame)) {
                return std::unexpected(
                    "GraphJit transition input suffix mirrors channels read at different delays");
            }
            if (!std::ranges::contains(
                    mirror->transition_channels,
                    transfer->target_storage.representation_channel)) {
                mirror->transition_channels.push_back(
                    transfer->target_storage.representation_channel);
            }
        }
        if (input_history) {
            target_ports.inputs[requirement.configured_port.port_index]
                .channels[*requirement.channel].storage =
                    *suffix_representation;
        } else {
            target_ports.outputs[requirement.configured_port.port_index]
                .storage = *suffix_representation;
        }

        auto& transition_realization = transition_realizations.sample_states[
            transition.current_realization_index];
        transition_realization.authored_storage = transfer->target_storage;
        transition_realization.retained_access =
            SampleRealtimePortStateAccess::direct;
        transition_realization.retained_sources = {transfer->target_storage};
        transition_realization.materialization.reset();

    }

    // A composed input's rewired source keeps its window in a source suffix
    // (SampleRealtimePortStateTransfer::composition_source). The transition
    // root's composition reads that source from the suffix, and a mirror fills
    // the suffix from the new source's storage, as a direct input's suffix is
    // filled: from where the source's values resume.
    for (auto const& transfer : realized->plan.sample_transfers) {
        if (!transfer.composition_source) continue;
        if (transfer.transition_index >= realized->plan.sample_states.size()) {
            return std::unexpected(
                "GraphJit transition source suffix lost its semantic transition");
        }
        auto const& transition =
            realized->plan.sample_states[transfer.transition_index];
        if (transition.current_requirement_index
                >= transition_plan.connections.realtime_port_states.states.size()
            || transition.current_realization_index
                >= steady_plan.realtime_port_state_realizations
                    .sample_states.size()) {
            return std::unexpected(
                "GraphJit transition source suffix lost its successor metadata");
        }
        auto const& requirement = transition_plan.connections
            .realtime_port_states.states[transition.current_requirement_index];
        auto const& current_realization = steady_plan
            .realtime_port_state_realizations.sample_states[
                transition.current_realization_index];
        if (*transfer.composition_source
                >= current_realization.composition_sources.size()
            || !requirement.channel) {
            return std::unexpected(
                "GraphJit transition source suffix lost its composition source");
        }
        auto const& view = current_realization.composition_sources[
            *transfer.composition_source].storage;
        auto const target_primitive = primitive_for_bundle(
            requirement.configured_port.node_bundle_handle);
        if (!target_primitive
            || *target_primitive >= steady_plan.sample_ports.primitives.size()) {
            return std::unexpected(
                "GraphJit transition source suffix lost its authored primitive");
        }
        auto const& steady_inputs =
            steady_plan.sample_ports.primitives[*target_primitive].inputs;
        if (requirement.configured_port.port_index >= steady_inputs.size()
            || *requirement.channel >= steady_inputs[
                requirement.configured_port.port_index].channels.size()) {
            return std::unexpected(
                "GraphJit transition source suffix lost its input binding");
        }
        auto const composed_representation = steady_inputs[
            requirement.configured_port.port_index]
            .channels[*requirement.channel].storage;

        auto& sample_storage = transition_plan.sample_ports.storage;
        // The NodeStorage offset holding a representation across callbacks:
        // its own ring, or its compact carry.
        auto retained_offset = [&](std::size_t representation)
            -> std::optional<std::size_t> {
            if (representation >= sample_storage.representations.size()) {
                return std::nullopt;
            }
            auto const allocation = sample_storage
                .representations[representation].persistent_allocation;
            if (allocation < sample_storage.persistent_allocations.size()) {
                return sample_storage.persistent_allocations[allocation]
                    .storage_offset;
            }
            for (auto const& carry : sample_storage.carry_operations) {
                if (carry.representation_index == representation
                    && carry.persistent_allocation
                        < sample_storage.persistent_allocations.size()) {
                    return sample_storage.persistent_allocations[
                        carry.persistent_allocation].storage_offset;
                }
            }
            return std::nullopt;
        };
        detail::SampleCompositionInputPlan* input = nullptr;
        for (auto& composition : sample_storage.compositions) {
            if (composition.target_representation != composed_representation) {
                continue;
            }
            for (auto& contribution : composition.contributions) {
                for (auto& candidate : contribution.sources) {
                    if (candidate.source_channel == view.representation_channel
                        && static_cast<std::int64_t>(candidate.read_latency)
                            == -view.timeline_offset_frames
                        && retained_offset(candidate.source_representation)
                            == view.node_storage_offset) {
                        if (input) {
                            return std::unexpected(
                                "GraphJit transition source suffix matches two composition inputs");
                        }
                        input = &candidate;
                    }
                }
            }
        }
        if (!input) {
            return std::unexpected(
                "GraphJit transition source suffix lost its composition input");
        }
        auto const source_representation = input->source_representation;
        auto const source = sample_storage.representations[source_representation];
        if (source.producer_group_index
                >= transition_plan.connections.sample_producer_groups.size()) {
            return std::unexpected(
                "GraphJit transition source suffix lost its producer");
        }
        auto const& producer_group = transition_plan.connections
            .sample_producer_groups[source.producer_group_index];
        auto const source_position = producer_group.source_port
            ? execution_position_for_bundle(
                producer_group.source_port->node_bundle_handle)
            : std::nullopt;
        auto const suffix_region = region_for_view(transfer.target_storage);
        if (!source_position || !suffix_region
            || *source_position
                >= transition_plan.execution.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit transition source suffix lost its producer step or region");
        }

        std::optional<std::size_t> suffix_representation;
        for (auto const& allocation : sample_storage.persistent_allocations) {
            if (allocation.region == *suffix_region) {
                suffix_representation = allocation.representation_index;
            }
        }
        if (!suffix_representation) {
            suffix_representation = sample_storage.representations.size();
            auto suffix = source;
            suffix.canonical_producer_representation = false;
            suffix.storage = RealtimeBufferStorageKind::full_node_storage;
            suffix.frame_capacity =
                transfer.target_storage.working_frame_capacity;
            suffix.transient_allocation = detail::no_sample_transient_allocation;
            suffix.persistent_allocation =
                sample_storage.persistent_allocations.size();
            suffix.constant_value.reset();
            sample_storage.representations.push_back(std::move(suffix));
            sample_storage.persistent_allocations.push_back(
                detail::SamplePersistentAllocationPlan{
                    .representation_index = *suffix_representation,
                    .kind = detail::SamplePersistentStorageKind::ring,
                    .channel_layout = transfer.target_storage.channel_layout,
                    .geometry = transfer.target_storage.geometry,
                    .frame_capacity =
                        transfer.target_storage.working_frame_capacity,
                    .size_bytes = transfer.target_storage.storage_size_bytes,
                    .alignment =
                        transfer.target_storage.geometry.element_alignment,
                    .region = *suffix_region,
                    .storage_offset =
                        *transfer.target_storage.node_storage_offset,
                });

            // The source finalizes each value its latency before the newest
            // frame it authored; the mirror copies each value then, but not
            // inside the window the cutover placed in the suffix.
            auto const read_delay = input->read_latency;
            auto const latency = view.producer_latency.value_or(0);
            auto const in_flight = read_delay > latency ? read_delay - latency : 0;
            if (transfer.target_end < 0
                || static_cast<std::size_t>(transfer.target_end) > read_delay) {
                return std::unexpected(
                    "GraphJit transition source suffix window ends outside its read delay");
            }
            auto const mirror_delay = read_delay
                - std::min<std::size_t>(
                    static_cast<std::size_t>(transfer.target_end), in_flight);
            auto const window_end = transfer.target_end
                - static_cast<std::int64_t>(read_delay);
            auto mirror = detail::SampleMaterializationPlan{
                .source_representation = source_representation,
                .target_representation = *suffix_representation,
                .after_execution_position = *source_position,
                .before_execution_position = source.constant_value
                    ? std::optional<std::size_t>{*source_position}
                    : std::nullopt,
                .source_layout = source.channel_layout,
                .target_layout = source.channel_layout,
                .retained_before = mirror_delay,
                .latest_read_latency = mirror_delay,
                .transition_channels = {view.representation_channel},
            };
            if (-static_cast<std::int64_t>(mirror_delay) < window_end) {
                auto const cursor =
                    transfer.target_storage.sequential_cursor_node_storage_offset;
                auto const cell = std::ranges::find_if(
                    realized->plan.sample_cutover_indices,
                    [&](SampleRealtimePortStateCutoverIndex const& index) {
                        return cursor
                            && index.sequential_cursor_node_storage_offset
                                == *cursor;
                    });
                if (cell == realized->plan.sample_cutover_indices.end()) {
                    return std::unexpected(
                        "GraphJit transition source suffix lost its cutover index cell");
                }
                mirror.transition_cutover_index_storage_offset =
                    cell->node_storage_offset;
                mirror.transition_first_frame = window_end;
            }
            auto const materialization_index =
                sample_storage.materializations.size();
            sample_storage.materializations.push_back(std::move(mirror));
            transition_plan.execution.primitive_steps[*source_position]
                .sample_materializations_after.push_back(materialization_index);
        }
        input->source_representation = *suffix_representation;
        // The transition realization's composition reads the suffix.
        transition_realizations.sample_states[
            transition.current_realization_index].composition_sources[
                *transfer.composition_source].storage = transfer.target_storage;
    }

    for (std::size_t transition_index = 0;
         transition_index < realized->plan.event_states.size();
         ++transition_index) {
        auto const& transition = realized->plan.event_states[transition_index];
        if (transition.inherited_state_realization
            != InheritedRealtimePortStateRealization::transition_only_storage) {
            continue;
        }
        if (transition.current_requirement_index
                >= transition_plan.connections.realtime_port_states.states.size()
            || transition.current_realization_index
                >= transition_realizations.event_states.size()) {
            return std::unexpected(
                "GraphJit transition event state lost its successor metadata");
        }
        auto const& requirement = transition_plan.connections
            .realtime_port_states.states[transition.current_requirement_index];
        auto const transfer = std::ranges::find_if(
            realized->plan.event_transfers,
            [&](EventRealtimePortStateTransfer const& candidate) {
                return candidate.transition_index == transition_index
                    && candidate.target_transition_only;
            });
        if (transfer == realized->plan.event_transfers.end()) {
            return std::unexpected(
                "GraphJit transition event state lost its suffix transfer");
        }
        auto const primitive = primitive_for_bundle(
            requirement.configured_port.node_bundle_handle);
        if (!primitive || *primitive >= transition_plan.event_ports.primitives.size()
            || requirement.configured_port.node_bundle_handle
                >= transition_plan.connections.schedule
                    .bundle_execution_position.size()) {
            return std::unexpected(
                "GraphJit transition event state lost its authored primitive");
        }
        auto const position = transition_plan.connections.schedule
            .bundle_execution_position[
                requirement.configured_port.node_bundle_handle];
        if (!position
            || *position >= transition_plan.execution.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit transition event state lost its execution step");
        }
        auto const& steady_primitive_ports =
            steady_plan.event_ports.primitives[*primitive];
        auto& primitive_ports =
            transition_plan.event_ports.primitives[*primitive];
        std::optional<std::size_t> steady_representation;
        auto const input_history = requirement.direction == PortDirection::input
            && requirement.role
                == RealtimePortStateRole::sequential_input_history;
        auto const output_window = requirement.direction == PortDirection::output
            && requirement.role == RealtimePortStateRole::tick_output_history;
        std::size_t retained_history = requirement.extent_samples;
        if (input_history) {
            if (requirement.configured_port.port_index
                    >= steady_primitive_ports.inputs.size()) {
                return std::unexpected(
                    "GraphJit transition event input lost its binding");
            }
            steady_representation = steady_primitive_ports.inputs[
                requirement.configured_port.port_index].storage;
        } else if (output_window) {
            if (requirement.configured_port.port_index
                    >= steady_primitive_ports.outputs.size()) {
                return std::unexpected(
                    "GraphJit transition event output lost its binding");
            }
            auto const& output = steady_primitive_ports.outputs[
                requirement.configured_port.port_index];
            if (!output.realtime || !output.storage
                || !output.canonical_storage
                || output.storage == output.canonical_storage) {
                return std::unexpected(
                    "GraphJit transition event output lost its callback-local retention path");
            }
            steady_representation = output.storage;
            retained_history = output.history;
        } else {
            return std::unexpected(
                "GraphJit transition suffix has an unsupported event-state role");
        }
        auto const region = event_region_for_view(transfer->target_storage);
        if (!steady_representation || !region
            || *steady_representation >= transition_plan.event_ports.ports.size()) {
            return std::unexpected(
                "GraphJit transition event state lost its source or suffix storage");
        }
        auto const source_representation = *steady_representation;
        auto const source = transition_plan.event_ports.ports[
            source_representation];
        std::optional<std::size_t> suffix_representation;
        for (std::size_t representation = 0;
             representation < transition_plan.event_ports.ports.size();
             ++representation) {
            auto const& candidate =
                transition_plan.event_ports.ports[representation];
            if (!candidate.persistent || candidate.region != *region) continue;
            if (suffix_representation
                || !candidate.persistent_ring
                || candidate.type != transfer->target_storage.type
                || candidate.event_capacity
                    != transfer->target_storage.event_capacity
                || candidate.size_bytes != transfer->target_storage.size_bytes
                || candidate.alignment != transfer->target_storage.alignment
                || candidate.storage_offset
                    != *transfer->target_storage.node_storage_offset) {
                return std::unexpected(
                    "GraphJit transition event suffix has inconsistent shared storage");
            }
            suffix_representation = representation;
        }

        auto const new_suffix = !suffix_representation;
        if (new_suffix) {
            suffix_representation = transition_plan.event_ports.ports.size();
            transition_plan.event_ports.ports.push_back(
                detail::EventRepresentationPlan{
                    .producer_group_index = source.producer_group_index,
                    .type = transfer->target_storage.type,
                    .event_capacity = transfer->target_storage.event_capacity,
                    .persistent = true,
                    .persistent_ring = true,
                    .has_producer_overflow_counter = false,
                    .count_relative_offset =
                        transfer->target_storage.count_relative_offset,
                    .read_index_relative_offset =
                        transfer->target_storage.read_index_relative_offset,
                    .write_index_relative_offset =
                        transfer->target_storage.write_index_relative_offset,
                    .has_sequential_timeline = true,
                    .timeline_initialized_relative_offset =
                        transfer->target_storage
                            .timeline_initialized_relative_offset,
                    .expected_next_time_relative_offset =
                        transfer->target_storage
                            .expected_next_time_relative_offset,
                    .events_relative_offset =
                        transfer->target_storage.events_relative_offset,
                    .size_bytes = transfer->target_storage.size_bytes,
                    .alignment = transfer->target_storage.alignment,
                    .region = *region,
                    .overflow_region = NodeLayout::RegionHandle{},
                    .storage_offset =
                        *transfer->target_storage.node_storage_offset,
                    .overflow_count_storage_offset = 0,
                });

            auto const prune_index =
                transition_plan.event_ports.persistent_rings.size();
            transition_plan.event_ports.persistent_rings.push_back(
                detail::EventPersistentRingPlan{
                    .storage = *suffix_representation,
                    .retained_history_samples = retained_history,
                });
            auto& step =
                transition_plan.execution.primitive_steps[*position];
            step.event_operations_before.push_back({
                .kind = detail::EventOperationKind::persistent_ring_prune,
                .index = prune_index,
            });

            if (input_history) {
                constexpr auto event_alignment = alignof(TimedEvent);
                constexpr auto event_mask = event_alignment - 1;
                auto const temp_events_offset =
                    (sizeof(std::size_t) + event_mask) & ~event_mask;
                if (transfer->target_storage.event_capacity
                        > (std::numeric_limits<std::size_t>::max()
                                - temp_events_offset)
                            / sizeof(TimedEvent)
                    || transition_plan.event_ports.transient_arena_size
                        > std::numeric_limits<std::size_t>::max()
                            - event_mask) {
                    return std::unexpected(
                        "GraphJit transition event scratch size overflows size_t");
                }
                auto const temp_size = temp_events_offset
                    + transfer->target_storage.event_capacity
                        * sizeof(TimedEvent);
                auto const temp_offset =
                    (transition_plan.event_ports.transient_arena_size
                        + event_mask)
                    & ~event_mask;
                if (temp_size > std::numeric_limits<std::size_t>::max()
                        - temp_offset) {
                    return std::unexpected(
                        "GraphJit transition event arena size overflows size_t");
                }
                auto const temp_representation =
                    transition_plan.event_ports.ports.size();
                transition_plan.event_ports.ports.push_back(
                    detail::EventRepresentationPlan{
                        .producer_group_index = source.producer_group_index,
                        .type = transfer->target_storage.type,
                        .event_capacity =
                            transfer->target_storage.event_capacity,
                        .events_relative_offset = temp_events_offset,
                        .size_bytes = temp_size,
                        .alignment = event_alignment,
                        .transient_allocation = transition_plan.event_ports
                            .transient_allocations.size(),
                    });
                transition_plan.event_ports.transient_allocations.push_back(
                    detail::EventTransientAllocationPlan{
                        .representation_index = temp_representation,
                        .size_bytes = temp_size,
                        .alignment = event_alignment,
                        .region_relative_offset = temp_offset,
                    });
                transition_plan.event_ports.transient_arena_size =
                    temp_offset + temp_size;
                transition_plan.event_ports.transient_arena_alignment =
                    std::max(
                        transition_plan.event_ports.transient_arena_alignment,
                        event_alignment);

                std::optional<EventConversionPlan> conversion;
                try {
                    conversion = EventConversionRegistry::plan(
                        source.type, transfer->target_storage.type);
                } catch (std::exception const&) {
                    return std::unexpected(
                        "GraphJit transition input suffix lost its event conversion");
                }
                auto const materialization_index =
                    transition_plan.event_ports.materializations.size();
                transition_plan.event_ports.materializations.push_back(
                    detail::EventMaterializationPlan{
                        .source_representation = source_representation,
                        .target_representation = temp_representation,
                        .conversion = *conversion,
                        .history_samples = 0,
                        .select_invocation_window = true,
                    });
                auto const merge_index =
                    transition_plan.event_ports.merges.size();
                transition_plan.event_ports.merges.push_back(
                    detail::EventMergePlan{
                        .source_representations = {temp_representation},
                        .target_representation = *suffix_representation,
                        .ordering_history_samples = retained_history,
                        .preserve_existing_target = true,
                    });
                step.event_operations_before.push_back({
                    .kind = detail::EventOperationKind::materialize,
                    .index = materialization_index,
                });
                step.event_operations_before.push_back({
                    .kind = detail::EventOperationKind::merge,
                    .index = merge_index,
                });
            } else {
                auto const merge_index =
                    transition_plan.event_ports.merges.size();
                transition_plan.event_ports.merges.push_back(
                    detail::EventMergePlan{
                        .source_representations = {source_representation},
                        .target_representation = *suffix_representation,
                        .ordering_history_samples = retained_history,
                        .preserve_existing_target = true,
                        .before_materialization = true,
                    });
                step.event_operations_after.push_back({
                    .kind = detail::EventOperationKind::merge,
                    .index = merge_index,
                });
            }
        }
        if (input_history) {
            primitive_ports.inputs[requirement.configured_port.port_index]
                .storage = *suffix_representation;
        }
        auto& transition_realization = transition_realizations.event_states[
            transition.current_realization_index];
        transition_realization.authored_storage = transfer->target_storage;
        transition_realization.retained_access =
            EventRealtimePortStateAccess::direct;
        transition_realization.retained_storage = transfer->target_storage;
        transition_realization.retained_source_index.reset();
        transition_realization.materialization_conversion.reset();
        transition_realization.materialization_history_samples = 0;
        transition_realization.materialization_selects_invocation_window = false;
    }

    {
        auto align_up = [](std::size_t value, std::size_t alignment)
            -> std::optional<std::size_t> {
            if (!std::has_single_bit(alignment)) return std::nullopt;
            auto const mask = alignment - 1;
            if (value > std::numeric_limits<std::size_t>::max() - mask) {
                return std::nullopt;
            }
            return (value + mask) & ~mask;
        };
        auto const sample_size = transition_plan.sample_ports.storage
            .transient_arena_size;
        auto const sample_alignment = std::max(
            std::size_t{1},
            transition_plan.sample_ports.storage.transient_arena_alignment);
        auto const event_size = transition_plan.event_ports.transient_arena_size;
        auto const event_alignment = std::max(
            std::size_t{1},
            transition_plan.event_ports.transient_arena_alignment);
        auto const event_offset = align_up(sample_size, event_alignment);
        auto const sample_offset = align_up(event_size, sample_alignment);
        if (!event_offset || !sample_offset
            || event_size > std::numeric_limits<std::size_t>::max()
                    - *event_offset
            || sample_size > std::numeric_limits<std::size_t>::max()
                    - *sample_offset) {
            return std::unexpected(
                "GraphJit transition root stack size overflows size_t");
        }
        auto sample_first = *event_offset + event_size;
        auto event_first = *sample_offset + sample_size;
        transition_plan.root_stack = {
            .size_bytes = sample_first,
            .alignment = std::max(sample_alignment, event_alignment),
            .sample_offset = 0,
            .event_offset = *event_offset,
        };
        if (event_first < sample_first) {
            transition_plan.root_stack.size_bytes = event_first;
            transition_plan.root_stack.sample_offset = *sample_offset;
            transition_plan.root_stack.event_offset = 0;
        }
    }

    if (realized->plan.sample_expiry_cursors.empty()
        && realized->plan.event_expiry_samples == 0) {
        return std::unexpected(
            "GraphJit transition root has no finite Sequential cursor horizon");
    }
    transition_plan.realtime_port_state_realizations =
        transition_realizations;
    // The suffix operations above can attach storage operations to a step
    // that the steady plan batched.
    if (auto split = detail::split_unbatched_ready_groups(transition_plan);
        !split) {
        return std::unexpected(std::move(split.error()));
    }
    return std::optional<TransitionLoweringPlan>{TransitionLoweringPlan{
        .root_plan = std::move(transition_plan),
        .output = {
            .state_transition = std::move(realized->plan),
            .realtime_port_state_realizations =
                std::move(transition_realizations),
        },
    }};
}

std::expected<LoweringOutput, std::string> emit_lowering_plan(
    LoweringInput& input,
    detail::LoweringPlan&& plan,
    std::optional<TransitionLoweringPlan>&& transition,
    llvm::Module& output_module)
{
    // Executor-side manual-control metadata is read before the realtime root
    // takes ownership of sample_ports.
    std::vector<ManualControlRuntimeBinding> manual_controls;
    manual_controls.reserve(plan.sample_ports.manual_controls.size());
    for (auto const& control : plan.sample_ports.manual_controls) {
        manual_controls.push_back(ManualControlRuntimeBinding{
            .configured = control.configured,
            .state_storage_offset = control.state_storage_offset,
            .participates = control.participates,
        });
    }

    // Building a realtime root validates every plan invariant it relies on,
    // so both roots are built before output LLVM is mutated. The transition
    // root consumes the whole transition plan, which nothing else reads.
    auto const has_transition_root = transition
        && (!transition->output.state_transition.sample_expiry_cursors.empty()
            || transition->output.state_transition.event_expiry_samples != 0);
    std::shared_ptr<RealtimeRootPlan const> transition_realtime_root;
    if (has_transition_root) {
        auto built = build_realtime_root_plan(input, transition->root_plan);
        if (!built) return std::unexpected(std::move(built.error()));
        transition_realtime_root = std::move(*built);
    }
    auto realtime_root = build_realtime_root_plan(input, plan);
    if (!realtime_root) {
        return std::unexpected(std::move(realtime_root.error()));
    }

    auto imported = emit_package_imports(input, plan.imports, output_module);
    if (!imported) return std::unexpected(std::move(imported.error()));

    auto configurations =
        emit_node_configurations(plan.configurations, output_module);
    if (!configurations) {
        return std::unexpected(std::move(configurations.error()));
    }

    auto combiner = define_sequential_input_combiner(output_module, plan);
    if (!combiner) {
        return std::unexpected(std::move(combiner.error()));
    }
    auto initializer = define_sequential_input_initializer(output_module, plan);
    if (!initializer) {
        return std::unexpected(std::move(initializer.error()));
    }

    auto const has_background_program = !plan.connections.background.empty();
    if (has_background_program) {
        auto forward = define_background_root_operation(
            output_module,
            root_background_forward_symbol,
            plan,
            *configurations,
            BackgroundRootOperation::forward);
        if (!forward) return std::unexpected(std::move(forward.error()));
        auto reverse = define_background_root_operation(
            output_module,
            root_background_reverse_symbol,
            plan,
            *configurations,
            BackgroundRootOperation::reverse);
        if (!reverse) return std::unexpected(std::move(reverse.error()));
        auto evaluate = define_background_root_operation(
            output_module,
            root_background_evaluate_symbol,
            plan,
            *configurations,
            BackgroundRootOperation::evaluate);
        if (!evaluate) return std::unexpected(std::move(evaluate.error()));
    }

    return LoweringOutput{
        .port_realization = std::move(plan.port_realization),
        .node_layout = std::move(plan.declarations.node_layout),
        .tock_state_layout = std::move(plan.declarations.tock_state_layout),
        .background_evaluation_plan = std::move(plan.connections.background),
        .realtime_port_state_requirements =
            std::move(plan.connections.realtime_port_states),
        .realtime_port_state_realizations =
            std::move(plan.realtime_port_state_realizations),
        .manual_controls = std::move(manual_controls),
        .transition = transition
            ? std::optional<LoweredGraphTransition>{
                std::move(transition->output)}
            : std::nullopt,
        .realtime_root = std::move(*realtime_root),
        .transition_realtime_root = std::move(transition_realtime_root),
        .root_symbols = {
            .propagate_background_forward = has_background_program
                ? std::string(root_background_forward_symbol)
                : std::string{},
            .propagate_background_reverse = has_background_program
                ? std::string(root_background_reverse_symbol)
                : std::string{},
            .evaluate_background = has_background_program
                ? std::string(root_background_evaluate_symbol)
                : std::string{},
            .combine_sequential_input =
                std::string(root_combine_sequential_input_symbol),
            .initialize_sequential_input =
                std::string(root_initialize_sequential_input_symbol),
        },
    };
}
} // namespace

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput& input,
    llvm::Module& output_module)
{
    if (output_module.getNamedValue(root_background_forward_symbol)
        || output_module.getNamedValue(root_background_reverse_symbol)
        || output_module.getNamedValue(root_background_evaluate_symbol)
        || output_module.getNamedValue(root_combine_sequential_input_symbol)
        || output_module.getNamedValue(root_initialize_sequential_input_symbol)) {
        return std::unexpected(
            "ConfiguredGraph -> LLVM IR lowering output module already contains reserved root symbols");
    }

    // Host-side graph/declaration/import/configuration/execution planning and
    // the realtime roots built from it must succeed before output LLVM is
    // mutated. The emitter then realizes the background part of that immutable
    // plan in one direction.
    using Clock = std::chrono::steady_clock;
    auto const microseconds = [](Clock::time_point begin, Clock::time_point end) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
    };
    auto const plan_begin = Clock::now();
    auto plan = detail::build_lowering_plan(input);
    if (!plan) return std::unexpected(std::move(plan.error()));

    auto const transition_begin = Clock::now();
    auto transition = specialize_transition_plan(*plan, input.transition);
    if (!transition) return std::unexpected(std::move(transition.error()));

    auto const emit_begin = Clock::now();
    auto output = emit_lowering_plan(
        input, std::move(*plan), std::move(*transition), output_module);
    auto const emit_end = Clock::now();
    if (output) {
        output->plan_us = microseconds(plan_begin, transition_begin);
        output->transition_us = microseconds(transition_begin, emit_begin);
        output->emit_us = microseconds(emit_begin, emit_end);
    }
    return output;
}
} // namespace iv::graph_jit
