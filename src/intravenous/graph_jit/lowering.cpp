#include <intravenous/graph_jit/lowering.h>
#include <intravenous/graph_jit/lowering_plan.h>

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

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iv::graph_jit {
namespace {
constexpr std::string_view root_tick_block_symbol = "__iv_graph_root_tick_block";
constexpr std::string_view root_skip_block_symbol = "__iv_graph_root_skip_block";

struct ReflectedContextByteOffsets {
    std::size_t compiled_state_data = 0;
    std::size_t compiled_state_size = 0;
    std::size_t state_data = 0;
    std::size_t state_size = 0;
};

struct EmittedNodeConfiguration {
    llvm::GlobalVariable* node_config = nullptr;
    llvm::GlobalVariable* tick_context_template = nullptr;
};

constexpr ReflectedContextByteOffsets reflected_context_byte_offsets() noexcept
{
    return {
        .compiled_state_data =
            offsetof(ReflectedNodeTickContext, compiled_state)
            + offsetof(ReflectedSpan<std::byte>, pointer),
        .compiled_state_size =
            offsetof(ReflectedNodeTickContext, compiled_state)
            + offsetof(ReflectedSpan<std::byte>, extent),
        .state_data = offsetof(ReflectedNodeTickContext, state)
            + offsetof(ReflectedSpan<std::byte>, pointer),
        .state_size = offsetof(ReflectedNodeTickContext, state)
            + offsetof(ReflectedSpan<std::byte>, extent),
    };
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

std::expected<llvm::Function*, std::string> prepare_primitive_callback_import(
    llvm::Module& output_module,
    llvm::Module& source_module,
    llvm::StringRef source_name,
    llvm::FunctionType* expected_type,
    std::string_view import_symbol,
    std::string_view role)
{
    auto* source = source_module.getFunction(source_name);
    if (!source || source->getFunctionType() != expected_type) {
        return std::unexpected(
            "selected primitive " + std::string(role)
            + " callback has the wrong LLVM ABI");
    }
    if (source->isDeclaration()) {
        return std::unexpected(
            "selected primitive " + std::string(role)
            + " callback has no definition in its finalized package LLVM");
    }

    auto const import_name = llvm::StringRef(import_symbol.data(), import_symbol.size());
    if (auto* collision = source_module.getNamedValue(import_name); collision != nullptr
        && collision != source) {
        return std::unexpected(
            "selected primitive " + std::string(role)
            + " import symbol collides with a package symbol");
    }
    if (output_module.getNamedValue(import_name)) {
        return std::unexpected(
            "selected primitive " + std::string(role)
            + " import symbol collides with a project symbol");
    }

    // Callback wrappers instantiated for package-local node types may have
    // local/linkonce linkage. Such a definition cannot satisfy an external
    // declaration in the destination module under LinkOnlyNeeded. Promote only
    // each selected root, under a GraphJit-owned name. Package import planning
    // collects every root first so each package module is consumed exactly once.
    source->setName(import_name);
    source->setLinkage(llvm::GlobalValue::ExternalLinkage);
    source->setVisibility(llvm::GlobalValue::DefaultVisibility);
    source->setDSOLocal(false);
    if (source->hasComdat()) source->setComdat(nullptr);

    auto* declaration = llvm::Function::Create(
        expected_type,
        llvm::GlobalValue::ExternalLinkage,
        import_name,
        output_module);
    declaration->setCallingConv(source->getCallingConv());
    declaration->setAttributes(source->getAttributes());
    return declaration;
}

std::expected<llvm::GlobalVariable*, std::string> prepare_retained_global_import(
    llvm::Module& output_module,
    llvm::Module& source_module,
    detail::RetainedGlobalImportPlan const& plan)
{
    auto* source = source_module.getNamedGlobal(plan.source_symbol);
    if (!source || source->isDeclaration() || !source->hasInitializer()
        || !source->isConstant()) {
        return std::unexpected(
            "selected retained LLVM global is not an immutable definition");
    }
    if (source->isThreadLocal() || source->getAddressSpace() != 0) {
        return std::unexpected(
            "selected retained LLVM global has an unsupported storage class");
    }
    auto const source_size = source_module.getDataLayout().getTypeAllocSize(
        source->getValueType());
    if (source_size.isScalable() || source_size.getFixedValue() != plan.size) {
        return std::unexpected(
            "selected retained LLVM global has the wrong finalized size");
    }

    auto const import_name = llvm::StringRef(
        plan.import_symbol.data(), plan.import_symbol.size());
    if (auto* collision = source_module.getNamedValue(import_name); collision != nullptr
        && collision != source) {
        return std::unexpected(
            "retained LLVM global import symbol collides with a package symbol");
    }
    if (output_module.getNamedValue(import_name)) {
        return std::unexpected(
            "retained LLVM global import symbol collides with a project symbol");
    }

    // Retained globals can have private/internal linkage just like package-local
    // callback wrappers. Promote only the selected immutable root under a
    // GraphJit-owned name; LinkOnlyNeeded then imports its initializer closure.
    source->setName(import_name);
    source->setLinkage(llvm::GlobalValue::ExternalLinkage);
    source->setVisibility(llvm::GlobalValue::DefaultVisibility);
    source->setDSOLocal(false);
    source->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    if (source->hasComdat()) source->setComdat(nullptr);

    auto* declaration = new llvm::GlobalVariable(
        output_module,
        source->getValueType(),
        true,
        llvm::GlobalValue::ExternalLinkage,
        nullptr,
        import_name,
        nullptr,
        llvm::GlobalValue::NotThreadLocal,
        source->getAddressSpace());
    declaration->setAlignment(source->getAlign());
    return declaration;
}

std::expected<void, std::string> emit_package_imports(
    LoweringInput& input,
    detail::PackageImportPlan const& plan,
    llvm::Module& output_module)
{
    auto* primitive_type = primitive_block_operation_type(output_module.getContext());
    for (auto const& package_plan : plan.packages) {
        if (package_plan.package_index >= input.packages.size()
            || !input.packages[package_plan.package_index].module) {
            return std::unexpected(
                "planned package LLVM is not available for lowering consumption");
        }

        // Package modules are parsed specifically for this GraphJit compilation.
        // Promote all selected callback/retained-global roots in place, then
        // consume the module once. LinkOnlyNeeded keeps only their transitive
        // closures without a full CloneModule copy.
        auto source_module =
            std::move(input.packages[package_plan.package_index].module);
        for (auto const& callback : package_plan.callbacks) {
            auto imported = prepare_primitive_callback_import(
                output_module,
                *source_module,
                callback.source_symbol,
                primitive_type,
                callback.import_symbol,
                callback.role);
            if (!imported) return std::unexpected(std::move(imported.error()));
        }
        for (auto const& global : package_plan.retained_globals) {
            auto imported = prepare_retained_global_import(
                output_module, *source_module, global);
            if (!imported) return std::unexpected(std::move(imported.error()));
        }

        if (llvm::Linker::linkModules(
                output_module,
                std::move(source_module),
                llvm::Linker::Flags::LinkOnlyNeeded)) {
            return std::unexpected(
                "failed to import selected package LLVM roots into the project module");
        }

        for (auto const& callback : package_plan.callbacks) {
            auto* imported = output_module.getFunction(callback.import_symbol);
            if (!imported || imported->isDeclaration()) {
                return std::unexpected(
                    "selected primitive " + callback.role
                    + " closure was not imported into the project module");
            }
        }
        for (auto const& global : package_plan.retained_globals) {
            auto* imported = output_module.getNamedGlobal(global.import_symbol);
            if (!imported || imported->isDeclaration() || !imported->hasInitializer()
                || !imported->isConstant()) {
                return std::unexpected(
                    "selected retained LLVM global was not imported into the project module");
            }
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
            if (!global || global->isDeclaration()) {
                return std::unexpected(
                    "planned node configuration relocation references an unmaterialized retained global");
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
        emitted.push_back(EmittedNodeConfiguration{
            .node_config = *node_config,
            .tick_context_template = immutable_bytes_global(
                output_module,
                &node.tick_context_template,
                sizeof(node.tick_context_template),
                alignof(ReflectedNodeTickContext),
                node.tick_context_global_symbol),
        });
    }
    return emitted;
}

void store_context_span(
    llvm::IRBuilder<>& builder,
    llvm::Value* context_storage,
    std::size_t data_field_offset,
    std::size_t size_field_offset,
    llvm::Value* storage_base,
    std::size_t storage_offset,
    std::size_t span_size)
{
    auto& llvm_context = builder.getContext();
    auto* byte_type = llvm::Type::getInt8Ty(llvm_context);
    auto* size_type = llvm::IntegerType::get(
        llvm_context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto offset = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };

    auto* data_slot = builder.CreateInBoundsGEP(
        byte_type, context_storage, offset(data_field_offset), "span.data.slot");
    auto* data = builder.CreateInBoundsGEP(
        byte_type, storage_base, offset(storage_offset), "span.data");
    builder.CreateStore(data, data_slot);

    auto* size_slot = builder.CreateInBoundsGEP(
        byte_type, context_storage, offset(size_field_offset), "span.size.slot");
    builder.CreateStore(offset(span_size), size_slot);
}

void emit_primitive_call(
    llvm::IRBuilder<>& builder,
    llvm::Function* primitive_callback,
    EmittedNodeConfiguration const& configuration,
    detail::PrimitiveStoragePlan const& storage,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* context_storage = builder.CreateAlloca(
        llvm::Type::getInt8Ty(builder.getContext()),
        llvm::ConstantInt::get(size_type, sizeof(ReflectedNodeTickContext)),
        "tick_context");
    context_storage->setAlignment(llvm::Align(alignof(ReflectedNodeTickContext)));
    builder.CreateMemCpy(
        context_storage,
        llvm::Align(alignof(ReflectedNodeTickContext)),
        configuration.tick_context_template,
        llvm::Align(alignof(ReflectedNodeTickContext)),
        sizeof(ReflectedNodeTickContext));

    auto const offsets = reflected_context_byte_offsets();
    if (storage.has_compiled_state) {
        store_context_span(
            builder,
            context_storage,
            offsets.compiled_state_data,
            offsets.compiled_state_size,
            storage_base,
            storage.compiled_state_offset,
            storage.compiled_state_size);
    }
    if (storage.has_state) {
        store_context_span(
            builder,
            context_storage,
            offsets.state_data,
            offsets.state_size,
            storage_base,
            storage.state_offset,
            storage.state_size);
    }

    auto* call = builder.CreateCall(
        primitive_callback,
        {configuration.node_config, context_storage, sample_index, block_size});
    call->setCallingConv(primitive_callback->getCallingConv());
}

void emit_sliced_primitive_calls(
    llvm::IRBuilder<>& builder,
    llvm::Function* primitive_callback,
    EmittedNodeConfiguration const& configuration,
    detail::PrimitiveStoragePlan const& storage,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    std::size_t maximum_block_size)
{
    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* zero = llvm::ConstantInt::get(size_type, 0);
    auto* maximum = llvm::ConstantInt::get(size_type, maximum_block_size);

    auto* preheader = builder.GetInsertBlock();
    auto* loop = llvm::BasicBlock::Create(context, "primitive.slice", function);
    auto* exit = llvm::BasicBlock::Create(context, "primitive.slice.end", function);
    auto* nonempty = builder.CreateICmpNE(block_size, zero, "primitive.slice.nonempty");
    builder.CreateCondBr(nonempty, loop, exit);

    builder.SetInsertPoint(loop);
    auto* offset = builder.CreatePHI(size_type, 2, "primitive.slice.offset");
    offset->addIncoming(zero, preheader);
    auto* remaining = builder.CreateSub(
        block_size, offset, "primitive.slice.remaining");
    auto* use_remaining = builder.CreateICmpULT(
        remaining, maximum, "primitive.slice.tail");
    auto* slice_size = builder.CreateSelect(
        use_remaining, remaining, maximum, "primitive.slice.size");
    auto* slice_index = builder.CreateAdd(
        sample_index, offset, "primitive.slice.index");

    emit_primitive_call(
        builder,
        primitive_callback,
        configuration,
        storage,
        storage_base,
        slice_index,
        slice_size);

    auto* next_offset = builder.CreateAdd(
        offset, slice_size, "primitive.slice.next");
    auto* done = builder.CreateICmpUGE(
        next_offset, block_size, "primitive.slice.done");
    builder.CreateCondBr(done, exit, loop);
    offset->addIncoming(next_offset, loop);

    builder.SetInsertPoint(exit);
}

std::expected<llvm::Function*, std::string> define_root_operation(
    llvm::Module& module,
    std::string_view symbol,
    detail::LoweringPlan const& plan,
    std::vector<EmittedNodeConfiguration> const& configurations,
    bool skip)
{
    auto* root_type = root_block_operation_type(module.getContext());
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

    auto* entry = llvm::BasicBlock::Create(module.getContext(), "entry", function);
    llvm::IRBuilder<> builder(entry);
    for (auto const& step : plan.execution.primitive_steps) {
        if (step.configuration_index >= configurations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing node configuration");
        }
        if (step.storage_index >= plan.declarations.primitive_storage.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing canonical storage plan");
        }

        auto const& callback_symbol =
            skip ? step.skip_callback_symbol : step.tick_callback_symbol;
        if (callback_symbol.empty()) {
            return std::unexpected(
                "GraphJit execution plan references a missing primitive callback");
        }
        auto* primitive_callback = module.getFunction(callback_symbol);
        if (!primitive_callback || primitive_callback->isDeclaration()) {
            return std::unexpected(
                "GraphJit execution plan references an unmaterialized primitive callback");
        }
        if (step.maximum_block_size == 0) {
            return std::unexpected(
                "GraphJit execution plan contains an invalid primitive maximum block size");
        }
        if (step.maximum_block_size < plan.declarations.node_layout.max_block_size) {
            emit_sliced_primitive_calls(
                builder,
                primitive_callback,
                configurations[step.configuration_index],
                plan.declarations.primitive_storage[step.storage_index],
                storage_base,
                sample_index,
                block_size,
                step.maximum_block_size);
        } else {
            emit_primitive_call(
                builder,
                primitive_callback,
                configurations[step.configuration_index],
                plan.declarations.primitive_storage[step.storage_index],
                storage_base,
                sample_index,
                block_size);
        }
    }
    builder.CreateRetVoid();
    return function;
}

std::expected<LoweringOutput, std::string> emit_lowering_plan(
    LoweringInput& input,
    detail::LoweringPlan&& plan,
    llvm::Module& output_module)
{
    auto imported = emit_package_imports(input, plan.imports, output_module);
    if (!imported) return std::unexpected(std::move(imported.error()));

    auto configurations =
        emit_node_configurations(plan.configurations, output_module);
    if (!configurations) {
        return std::unexpected(std::move(configurations.error()));
    }

    auto tick = define_root_operation(
        output_module,
        root_tick_block_symbol,
        plan,
        *configurations,
        false);
    if (!tick) return std::unexpected(std::move(tick.error()));

    std::string skip_symbol;
    if (plan.execution.root_skippable) {
        auto skip = define_root_operation(
            output_module,
            root_skip_block_symbol,
            plan,
            *configurations,
            true);
        if (!skip) return std::unexpected(std::move(skip.error()));
        skip_symbol = std::string(root_skip_block_symbol);
    }

    return LoweringOutput{
        .node_layout = std::move(plan.declarations.node_layout),
        .root_symbols = {
            .tick_block = std::string(root_tick_block_symbol),
            .skip_block = std::move(skip_symbol),
        },
    };
}
} // namespace

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput& input,
    llvm::Module& output_module)
{
    if (output_module.getNamedValue(root_tick_block_symbol)
        || output_module.getNamedValue(root_skip_block_symbol)) {
        return std::unexpected(
            "ConfiguredGraph -> LLVM IR lowering output module already contains reserved root symbols");
    }

    // Host-side graph/declaration/import/configuration/execution planning must
    // succeed before output LLVM is mutated or a compile-local package module is
    // consumed. The emitter then realizes that immutable plan in one direction.
    auto plan = detail::build_lowering_plan(input);
    if (!plan) return std::unexpected(std::move(plan.error()));

    return emit_lowering_plan(input, std::move(*plan), output_module);
}
} // namespace iv::graph_jit
