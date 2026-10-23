#include <intravenous/graph_jit/lowering.h>
#include <intravenous/graph_jit/activity_runtime.h>
#include <intravenous/graph_jit/event_conversion_runtime.h>
#include <intravenous/graph_jit/event_retention_runtime.h>
#include <intravenous/graph_jit/lowering_plan.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/Alignment.h>

#include <algorithm>
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
constexpr std::string_view root_tick_block_symbol = "__iv_graph_root_tick_block";
constexpr std::string_view root_transition_tick_block_symbol =
    "__iv_graph_root_transition_tick_block";
constexpr std::string_view root_background_forward_symbol =
    "__iv_graph_root_background_forward";
constexpr std::string_view root_background_reverse_symbol =
    "__iv_graph_root_background_reverse";
constexpr std::string_view root_background_evaluate_symbol =
    "__iv_graph_root_background_evaluate";

struct ReflectedContextByteOffsets {
    std::size_t continuous_input_bindings_data = 0;
    std::size_t continuous_input_bindings_size = 0;
    std::size_t continuous_output_bindings_data = 0;
    std::size_t continuous_output_bindings_size = 0;
    std::size_t event_input_bindings_data = 0;
    std::size_t event_input_bindings_size = 0;
    std::size_t event_output_bindings_data = 0;
    std::size_t event_output_bindings_size = 0;
    std::size_t random_access_inputs_data = 0;
    std::size_t random_access_inputs_size = 0;
    std::size_t random_access_event_inputs_data = 0;
    std::size_t random_access_event_inputs_size = 0;
    std::size_t state_data = 0;
    std::size_t state_size = 0;
};

struct TickInvocationByteOffsets {
    std::size_t sequential_sample_inputs_data = 0;
    std::size_t sequential_sample_inputs_size = 0;
    std::size_t sequential_event_inputs_data = 0;
    std::size_t sequential_event_inputs_size = 0;
    std::size_t random_access_sample_inputs_data = 0;
    std::size_t random_access_sample_inputs_size = 0;
    std::size_t random_access_event_inputs_data = 0;
    std::size_t random_access_event_inputs_size = 0;
    std::size_t sample_captures_data = 0;
    std::size_t sample_captures_size = 0;
    std::size_t event_captures_data = 0;
    std::size_t event_captures_size = 0;
};

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
    llvm::GlobalVariable* tick_context_template = nullptr;
};

struct EmittedPrimitiveSamplePorts {
    llvm::Value* input_bindings = nullptr;
    std::size_t input_count = 0;
    llvm::Value* output_bindings = nullptr;
    std::size_t output_count = 0;
};

struct EmittedSamplePortBindings {
    std::vector<EmittedPrimitiveSamplePorts> primitives{};
};

struct EmittedPrimitiveEventPorts {
    llvm::Value* input_bindings = nullptr;
    std::size_t input_count = 0;
    llvm::Value* output_bindings = nullptr;
    std::size_t output_count = 0;
};

struct EmittedEventPortBindings {
    std::vector<EmittedPrimitiveEventPorts> primitives{};
};

struct EmittedRealtimeStorage {
    llvm::Value* persistent_base = nullptr;
    llvm::Value* sample_transient_base = nullptr;
    llvm::Value* event_transient_base = nullptr;
    std::vector<llvm::Value*> sample_representations{};
    std::vector<llvm::Value*> producer_authored_extents{};
    std::vector<llvm::Value*> sequential_pace_cursors{};
    std::vector<llvm::Value*> event_representations{};
    std::vector<llvm::Value*> event_overflow_counts{};
};

struct EmittedContinuousActivitySummary {
    std::size_t frontier = 0;
    std::size_t representation_epoch = 0;
    llvm::Value* sample_index = nullptr;
    llvm::Value* block_size = nullptr;
    llvm::Value* flags = nullptr;
};

struct EmittedEventActivitySummary {
    std::size_t representation = 0;
    std::size_t representation_epoch = 0;
    llvm::Value* sample_index = nullptr;
    llvm::Value* block_size = nullptr;
    llvm::Value* empty = nullptr;
};

struct ActivityEmissionCache {
    // These are compiler-emission epochs, not runtime counters. A write to one
    // sample representation advances its epoch so a later consumer cannot
    // reuse a fact that was emitted before that write.
    std::vector<std::size_t> representation_epochs{};
    std::vector<EmittedContinuousActivitySummary> continuous{};
    std::vector<std::size_t> event_representation_epochs{};
    std::vector<EmittedEventActivitySummary> events{};
};

constexpr ReflectedContextByteOffsets reflected_context_byte_offsets() noexcept
{
    return {
        .continuous_input_bindings_data =
            offsetof(ReflectedNodeTickContext, continuous_input_bindings)
            + offsetof(ReflectedSpan<ReflectedContinuousInputPortBinding const>, pointer),
        .continuous_input_bindings_size =
            offsetof(ReflectedNodeTickContext, continuous_input_bindings)
            + offsetof(ReflectedSpan<ReflectedContinuousInputPortBinding const>, extent),
        .continuous_output_bindings_data =
            offsetof(ReflectedNodeTickContext, continuous_output_bindings)
            + offsetof(ReflectedSpan<ReflectedContinuousOutputPortBinding const>, pointer),
        .continuous_output_bindings_size =
            offsetof(ReflectedNodeTickContext, continuous_output_bindings)
            + offsetof(ReflectedSpan<ReflectedContinuousOutputPortBinding const>, extent),
        .event_input_bindings_data =
            offsetof(ReflectedNodeTickContext, event_input_bindings)
            + offsetof(ReflectedSpan<ReflectedEventInputPortBinding const>, pointer),
        .event_input_bindings_size =
            offsetof(ReflectedNodeTickContext, event_input_bindings)
            + offsetof(ReflectedSpan<ReflectedEventInputPortBinding const>, extent),
        .event_output_bindings_data =
            offsetof(ReflectedNodeTickContext, event_output_bindings)
            + offsetof(ReflectedSpan<ReflectedEventOutputPortBinding const>, pointer),
        .event_output_bindings_size =
            offsetof(ReflectedNodeTickContext, event_output_bindings)
            + offsetof(ReflectedSpan<ReflectedEventOutputPortBinding const>, extent),
        .random_access_inputs_data =
            offsetof(ReflectedNodeTickContext, random_access_inputs)
            + offsetof(ReflectedSpan<RandomAccessSampleInputPort const>, pointer),
        .random_access_inputs_size =
            offsetof(ReflectedNodeTickContext, random_access_inputs)
            + offsetof(ReflectedSpan<RandomAccessSampleInputPort const>, extent),
        .random_access_event_inputs_data =
            offsetof(ReflectedNodeTickContext, random_access_event_inputs)
            + offsetof(ReflectedSpan<RandomAccessEventInputPort const>, pointer),
        .random_access_event_inputs_size =
            offsetof(ReflectedNodeTickContext, random_access_event_inputs)
            + offsetof(ReflectedSpan<RandomAccessEventInputPort const>, extent),
        .state_data = offsetof(ReflectedNodeTickContext, state)
            + offsetof(ReflectedSpan<std::byte>, pointer),
        .state_size = offsetof(ReflectedNodeTickContext, state)
            + offsetof(ReflectedSpan<std::byte>, extent),
    };
}

constexpr TickInvocationByteOffsets tick_invocation_byte_offsets() noexcept
{
    return {
        .sequential_sample_inputs_data =
            offsetof(TickInvocationCall, sequential_sample_inputs)
            + offsetof(
                ReflectedSpan<ReflectedContinuousInputPortBinding const>, pointer),
        .sequential_sample_inputs_size =
            offsetof(TickInvocationCall, sequential_sample_inputs)
            + offsetof(
                ReflectedSpan<ReflectedContinuousInputPortBinding const>, extent),
        .sequential_event_inputs_data =
            offsetof(TickInvocationCall, sequential_event_inputs)
            + offsetof(
                ReflectedSpan<ReflectedEventInputPortBinding const>, pointer),
        .sequential_event_inputs_size =
            offsetof(TickInvocationCall, sequential_event_inputs)
            + offsetof(
                ReflectedSpan<ReflectedEventInputPortBinding const>, extent),
        .random_access_sample_inputs_data =
            offsetof(TickInvocationCall, random_access_sample_inputs)
            + offsetof(ReflectedSpan<RandomAccessSampleInputPort const>, pointer),
        .random_access_sample_inputs_size =
            offsetof(TickInvocationCall, random_access_sample_inputs)
            + offsetof(ReflectedSpan<RandomAccessSampleInputPort const>, extent),
        .random_access_event_inputs_data =
            offsetof(TickInvocationCall, random_access_event_inputs)
            + offsetof(ReflectedSpan<RandomAccessEventInputPort const>, pointer),
        .random_access_event_inputs_size =
            offsetof(TickInvocationCall, random_access_event_inputs)
            + offsetof(ReflectedSpan<RandomAccessEventInputPort const>, extent),
        .sample_captures_data = offsetof(TickInvocationCall, sample_captures)
            + offsetof(
                ReflectedSpan<TickSampleCaptureOperation const>, pointer),
        .sample_captures_size = offsetof(TickInvocationCall, sample_captures)
            + offsetof(
                ReflectedSpan<TickSampleCaptureOperation const>, extent),
        .event_captures_data = offsetof(TickInvocationCall, event_captures)
            + offsetof(
                ReflectedSpan<TickEventCaptureOperation const>, pointer),
        .event_captures_size = offsetof(TickInvocationCall, event_captures)
            + offsetof(
                ReflectedSpan<TickEventCaptureOperation const>, extent),
    };
}

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

llvm::FunctionType* root_block_operation_type(llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {pointer, pointer, size_type, size_type},
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

llvm::FunctionType* background_operation_type(llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {pointer, pointer}, false);
}

llvm::FunctionType* batch_operation_type(llvm::LLVMContext& context)
{
    auto* pointer = llvm::PointerType::getUnqual(context);
    return llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {pointer}, false);
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

std::expected<llvm::Function*, std::string> import_primitive_callback(
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

std::expected<llvm::GlobalVariable*, std::string> import_retained_global(
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
    auto* block_type = primitive_block_operation_type(output_module.getContext());
    auto* background_type = background_operation_type(output_module.getContext());
    auto* batch_type = batch_operation_type(output_module.getContext());
    auto* size_type = llvm::IntegerType::get(
        output_module.getContext(),
        static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* sequential_combine_type = llvm::FunctionType::get(
        llvm::Type::getInt1Ty(output_module.getContext()),
        {size_type,
         llvm::Type::getInt8Ty(output_module.getContext()),
         llvm::PointerType::getUnqual(output_module.getContext()),
         llvm::PointerType::getUnqual(output_module.getContext()),
         size_type,
         size_type},
        false);
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
            llvm::FunctionType* callback_type = nullptr;
            switch (callback.abi) {
            case detail::CallbackImportAbi::block:
                callback_type = block_type;
                break;
            case detail::CallbackImportAbi::background:
                callback_type = background_type;
                break;
            case detail::CallbackImportAbi::batch:
                callback_type = batch_type;
                break;
            case detail::CallbackImportAbi::sequential_combine:
                callback_type = sequential_combine_type;
                break;
            }
            IV_ASSERT(callback_type != nullptr,
                "unknown primitive callback import ABI");
            auto imported = import_primitive_callback(
                output_module,
                *source_module,
                callback.source_symbol,
                callback_type,
                callback.import_symbol,
                callback.role);
            if (!imported) return std::unexpected(std::move(imported.error()));
        }
        for (auto const& global : package_plan.retained_globals) {
            auto imported = import_retained_global(
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

std::expected<ReflectedContinuousPortStorageBinding, std::string>
sample_storage_binding(
    detail::SampleStoragePlan const& plan,
    std::size_t representation_index)
{
    if (representation_index >= plan.representations.size()) {
        return std::unexpected(
            "GraphJit sample port references missing storage");
    }
    auto const& storage = plan.representations[representation_index];

    if (storage.constant_value) {
        if (storage.transient_allocation
                != detail::no_sample_transient_allocation
            || storage.persistent_allocation
                != detail::no_sample_persistent_allocation) {
            return std::unexpected(
                "GraphJit constant sample input unexpectedly owns writable storage");
        }
    } else if (storage.transient_allocation
               != detail::no_sample_transient_allocation) {
        if (storage.transient_allocation >= plan.transient_allocations.size()) {
            return std::unexpected(
                "GraphJit sample storage references a missing transient allocation");
        }
        auto const& allocation =
            plan.transient_allocations[storage.transient_allocation];
        if (allocation.representation_index != representation_index) {
            return std::unexpected(
                "GraphJit sample transient allocation points at the wrong storage");
        }
    } else if (storage.persistent_allocation
               != detail::no_sample_persistent_allocation) {
        if (storage.persistent_allocation >= plan.persistent_allocations.size()) {
            return std::unexpected(
                "GraphJit sample storage references a missing persistent allocation");
        }
        auto const& allocation =
            plan.persistent_allocations[storage.persistent_allocation];
        if (allocation.representation_index != representation_index) {
            return std::unexpected(
                "GraphJit sample persistent allocation points at the wrong storage");
        }
        if (allocation.kind != detail::SamplePersistentStorageKind::ring) {
            return std::unexpected(
                "GraphJit compact sample carry must bind through its transient working storage");
        }
    } else {
        return std::unexpected(
            "GraphJit sample storage has no realized storage");
    }
    ReflectedContinuousPortStorageBinding binding{
        .geometry = storage.geometry,
        .value_capacity = storage.frame_capacity,
        .storage_latency = 0,
        .channel_layout = storage.channel_layout,
    };
    auto const channels = channel_count(storage.channel_layout);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        binding.channels[channel].value_capacity = storage.frame_capacity;
        binding.channels[channel].value_stride_bytes =
            storage.channel_layout.sample_layout
                    == SampleStreamLayout::planar
                ? storage.geometry.value_size_bytes
                : channels * storage.geometry.value_size_bytes;
    }
    return binding;
}

std::expected<ReflectedContinuousPortStorageBinding, std::string>
sample_input_storage_binding(
    detail::SampleStoragePlan const& storage,
    detail::PrimitiveSampleInputBindingPlan const& input)
{
    auto const target_channels = channel_count(input.channel_layout);
    if (input.channels.size() != target_channels || target_channels == 0) {
        return std::unexpected(
            "GraphJit sample input storage has an invalid channel count");
    }

    ReflectedContinuousPortStorageBinding binding{
        .storage_latency = 0,
        .channel_layout = input.channel_layout,
    };
    for (std::size_t target_channel = 0;
         target_channel < target_channels; ++target_channel) {
        auto const& channel = input.channels[target_channel];
        if (channel.storage == detail::no_sample_representation) {
            return std::unexpected(
                "GraphJit sample input channel has no storage");
        }
        auto source = sample_storage_binding(
            storage, channel.storage);
        if (!source) return std::unexpected(std::move(source.error()));
        auto const source_channel_count = channel_count(source->channel_layout);
        if (channel.representation_channel >= source_channel_count) {
            return std::unexpected(
                "GraphJit sample input channel is outside its storage");
        }
        binding.channels[target_channel] =
            source->channels[channel.representation_channel];
        binding.channels[target_channel].value_delay = channel.frame_delay;
        if (target_channel == 0) {
            binding.geometry = source->geometry;
            binding.value_capacity =
                binding.channels[target_channel].value_capacity;
        } else if (binding.geometry != source->geometry) {
            return std::unexpected(
                "GraphJit continuous input channels disagree on value geometry");
        }
    }
    return binding;
}

llvm::Value* runtime_bytes_copy(
    llvm::IRBuilder<>& builder,
    llvm::Module& module,
    void const* data,
    std::size_t size,
    std::size_t alignment,
    llvm::Twine const& name)
{
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* result = builder.CreateAlloca(
        llvm::Type::getInt8Ty(builder.getContext()),
        llvm::ConstantInt::get(size_type, size),
        name + ".runtime");
    result->setAlignment(llvm::Align(alignment));
    auto* initial = immutable_bytes_global(
        module, data, size, alignment, name + ".template");
    builder.CreateMemCpy(
        result, llvm::Align(alignment), initial, llvm::Align(alignment), size);
    return result;
}

void store_runtime_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* bytes,
    std::size_t offset,
    llvm::Value* value)
{
    builder.CreateStore(
        value,
        byte_offset_pointer(builder, bytes, offset, "binding.pointer.index"));
}

void store_sample_binding_channel_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* binding_bytes,
    std::size_t storage_binding_offset,
    std::size_t target_channel,
    detail::SampleRepresentationPlan const& storage,
    std::size_t representation_channel,
    llvm::Value* representation_base)
{
    auto const channels = channel_count(storage.channel_layout);
    IV_ASSERT(
        representation_channel < channels,
        "sample source channel is outside its storage");
    auto const value_offset = storage.channel_layout.sample_layout
            == SampleStreamLayout::planar
        ? representation_channel * storage.frame_capacity
        : representation_channel;
    auto* channel_base = value_offset == 0
        ? representation_base
        : byte_offset_pointer(
            builder,
            representation_base,
            value_offset * storage.geometry.value_size_bytes,
            "continuous.binding.channel");
    store_runtime_pointer(
        builder,
        binding_bytes,
        storage_binding_offset
            + offsetof(ReflectedContinuousPortStorageBinding, channels)
            + target_channel * sizeof(ReflectedContinuousChannelStorageBinding)
            + offsetof(ReflectedContinuousChannelStorageBinding, storage),
        channel_base);
}

void store_sample_binding_channel_pointers(
    llvm::IRBuilder<>& builder,
    llvm::Value* binding_bytes,
    std::size_t storage_binding_offset,
    detail::SampleRepresentationPlan const& storage,
    llvm::Value* representation_base)
{
    auto const channels = channel_count(storage.channel_layout);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        store_sample_binding_channel_pointer(
            builder,
            binding_bytes,
            storage_binding_offset,
            channel,
            storage,
            channel,
            representation_base);
    }
}

std::expected<EmittedSamplePortBindings, std::string> emit_sample_port_bindings(
    llvm::IRBuilder<>& builder,
    llvm::Module& module,
    detail::SamplePortBindingPlan const& plan,
    EmittedRealtimeStorage const& realtime_storage)
{
    EmittedSamplePortBindings emitted;
    emitted.primitives.resize(plan.primitives.size());

    for (std::size_t primitive_index = 0;
         primitive_index < plan.primitives.size();
         ++primitive_index) {
        auto const& primitive = plan.primitives[primitive_index];
        auto& result = emitted.primitives[primitive_index];
        result.input_count = primitive.inputs.size();
        result.output_count = std::ranges::count_if(
            primitive.outputs,
            [](auto const& output) { return output.realtime; });

        if (!primitive.inputs.empty()) {
            std::vector<ReflectedContinuousInputPortBinding> bindings;
            bindings.reserve(primitive.inputs.size());
            for (auto const& input : primitive.inputs) {
                // The TickInvocationCall slot replaces an invocation-supplied
                // binding except for its compiler-owned scratch below.
                auto storage = input.invocation_supplied
                        && !input.combines_invocation_supplied
                    ? std::expected<
                        ReflectedContinuousPortStorageBinding,
                        std::string>{ReflectedContinuousPortStorageBinding{
                            .geometry = input.geometry,
                            .channel_layout = input.channel_layout,
                        }}
                    : sample_input_storage_binding(plan.storage, input);
                if (!storage) return std::unexpected(std::move(storage.error()));
                bindings.push_back(ReflectedContinuousInputPortBinding{
                    .storage = *storage,
                    .history = input.history,
                    .read_latency = input.read_latency,
                    .absolute_pace_exponent = input.absolute_pace_exponent,
                    .contiguous_value_capacity =
                        input.contiguous_value_capacity,
                });
            }
            result.input_bindings = runtime_bytes_copy(
                builder, module,
                bindings.data(),
                bindings.size() * sizeof(ReflectedContinuousInputPortBinding),
                alignof(ReflectedContinuousInputPortBinding),
                "__iv_graph_sample_inputs_" + std::to_string(primitive_index));
            for (std::size_t i = 0; i < primitive.inputs.size(); ++i) {
                auto const& input = primitive.inputs[i];
                for (std::size_t target_channel = 0;
                     target_channel < channel_count(input.channel_layout);
                     ++target_channel) {
                    if (!input.invocation_supplied
                        || input.combines_invocation_supplied) {
                        auto const& channel = input.channels[target_channel];
                        auto const storage = channel.storage;
                        if (storage
                                >= realtime_storage.sample_representations.size()
                            || storage >= plan.storage.representations.size()) {
                            return std::unexpected(
                                "GraphJit sample input lost its resolved storage");
                        }
                        store_sample_binding_channel_pointer(
                            builder,
                            result.input_bindings,
                            i * sizeof(ReflectedContinuousInputPortBinding)
                                + offsetof(
                                    ReflectedContinuousInputPortBinding,
                                    storage),
                            target_channel,
                            plan.storage.representations[storage],
                            channel.representation_channel,
                            realtime_storage.sample_representations[storage]);
                    }
                    if (input.has_contiguous_scratch) {
                        if (realtime_storage.sample_transient_base == nullptr
                            || input.contiguous_value_capacity
                                > std::numeric_limits<std::size_t>::max()
                                    / input.geometry.value_size_bytes) {
                            return std::unexpected(
                                "GraphJit continuous input scratch lost its root arena");
                        }
                        auto const channel_bytes =
                            input.contiguous_storage_capacity
                            * input.geometry.value_size_bytes;
                        auto* scratch = byte_offset_pointer(
                            builder,
                            realtime_storage.sample_transient_base,
                            input.contiguous_scratch_offset
                                + target_channel * channel_bytes,
                            "continuous.input.scratch");
                        store_runtime_pointer(
                            builder,
                            result.input_bindings,
                            i * sizeof(ReflectedContinuousInputPortBinding)
                                + offsetof(
                                    ReflectedContinuousInputPortBinding,
                                    storage)
                                + offsetof(
                                    ReflectedContinuousPortStorageBinding,
                                    channels)
                                + target_channel
                                    * sizeof(
                                        ReflectedContinuousChannelStorageBinding)
                                + offsetof(
                                    ReflectedContinuousChannelStorageBinding,
                                    contiguous_scratch),
                            scratch);
                    }
                }
            }
        }

        if (result.output_count != 0) {
            std::vector<ReflectedContinuousOutputPortBinding> bindings;
            bindings.reserve(result.output_count);
            for (auto const& output : primitive.outputs) {
                if (!output.realtime) continue;
                if (!output.storage) {
                    return std::unexpected(
                        "GraphJit sample output has no storage");
                }
                auto storage = sample_storage_binding(
                    plan.storage, *output.storage);
                if (!storage) return std::unexpected(std::move(storage.error()));
                bindings.push_back(ReflectedContinuousOutputPortBinding{
                    .storage = *storage,
                    .history = output.history,
                    .latency = output.latency,
                    .absolute_pace_exponent = output.absolute_pace_exponent,
                });
            }
            result.output_bindings = runtime_bytes_copy(
                builder, module,
                bindings.data(),
                bindings.size() * sizeof(ReflectedContinuousOutputPortBinding),
                alignof(ReflectedContinuousOutputPortBinding),
                "__iv_graph_sample_outputs_" + std::to_string(primitive_index));
            std::size_t binding_index = 0;
            for (auto const& output : primitive.outputs) {
                if (!output.realtime) continue;
                auto const storage = *output.storage;
                if (storage >= realtime_storage.sample_representations.size()) {
                    return std::unexpected(
                        "GraphJit sample output lost its resolved storage");
                }
                store_sample_binding_channel_pointers(
                    builder,
                    result.output_bindings,
                    binding_index * sizeof(ReflectedContinuousOutputPortBinding)
                        + offsetof(ReflectedContinuousOutputPortBinding, storage),
                    plan.storage.representations[storage],
                    realtime_storage.sample_representations[storage]);
                ++binding_index;
            }
        }
    }
    return emitted;
}

std::expected<EmittedEventPortBindings, std::string> emit_event_port_bindings(
    llvm::IRBuilder<>& builder,
    llvm::Module& module,
    detail::EventPortBindingPlan const& plan,
    EmittedRealtimeStorage const& realtime_storage)
{
    EmittedEventPortBindings emitted;
    emitted.primitives.resize(plan.primitives.size());

    auto storage_binding = [&](std::size_t representation_index)
        -> std::expected<ReflectedEventPortStorageBinding, std::string> {
        if (representation_index >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit event port references missing storage");
        }
        auto const& storage = plan.ports[representation_index];
        return ReflectedEventPortStorageBinding{
            .storage = nullptr,
            .count_offset = storage.count_relative_offset,
            .read_index_offset = storage.read_index_relative_offset,
            .write_index_offset = storage.write_index_relative_offset,
            .events_offset = storage.events_relative_offset,
            .event_capacity = storage.event_capacity,
            .type = storage.type,
            .persistent_ring = storage.persistent_ring,
        };
    };

    for (std::size_t primitive_index = 0;
         primitive_index < plan.primitives.size();
         ++primitive_index) {
        auto const& primitive = plan.primitives[primitive_index];
        auto& result = emitted.primitives[primitive_index];
        result.input_count = primitive.inputs.size();
        result.output_count = std::ranges::count_if(
            primitive.outputs,
            [](auto const& output) { return output.realtime; });

        if (!primitive.inputs.empty()) {
            std::vector<ReflectedEventInputPortBinding> bindings;
            bindings.reserve(primitive.inputs.size());
            for (auto const& input : primitive.inputs) {
                // A background-only slot replaces the whole binding. Mixed
                // delivery retains this realtime binding for the final merge.
                if (input.invocation_supplied
                    && !input.combines_invocation_supplied) {
                    bindings.emplace_back();
                    continue;
                }
                if (!input.storage) {
                    return std::unexpected(
                        "GraphJit event input has no storage");
                }
                auto storage = storage_binding(*input.storage);
                if (!storage) return std::unexpected(std::move(storage.error()));
                bindings.push_back(ReflectedEventInputPortBinding{
                    .storage = *storage,
                });
            }
            result.input_bindings = runtime_bytes_copy(
                builder, module,
                bindings.data(),
                bindings.size() * sizeof(ReflectedEventInputPortBinding),
                alignof(ReflectedEventInputPortBinding),
                "__iv_graph_event_inputs_" + std::to_string(primitive_index));
            for (std::size_t i = 0; i < primitive.inputs.size(); ++i) {
                if (primitive.inputs[i].invocation_supplied
                    && !primitive.inputs[i].combines_invocation_supplied) {
                    continue;
                }
                auto const storage = *primitive.inputs[i].storage;
                if (storage >= realtime_storage.event_representations.size()) {
                    return std::unexpected(
                        "GraphJit event input lost its resolved storage");
                }
                store_runtime_pointer(
                    builder,
                    result.input_bindings,
                    i * sizeof(ReflectedEventInputPortBinding)
                        + offsetof(ReflectedEventInputPortBinding, storage)
                        + offsetof(ReflectedEventPortStorageBinding, storage),
                    realtime_storage.event_representations[storage]);
                auto const& representation = plan.ports[storage];
                if (representation.has_source_indices) {
                    store_runtime_pointer(
                        builder,
                        result.input_bindings,
                        i * sizeof(ReflectedEventInputPortBinding)
                            + offsetof(
                                ReflectedEventInputPortBinding,
                                semantic_source_indices),
                        byte_offset_pointer(
                            builder,
                            realtime_storage.event_representations[storage],
                            representation.source_indices_relative_offset,
                            "event.input.source.indices"));
                }
            }
        }

        if (result.output_count != 0) {
            std::vector<ReflectedEventOutputPortBinding> bindings;
            bindings.reserve(result.output_count);
            for (auto const& output : primitive.outputs) {
                if (!output.realtime) continue;
                if (!output.storage) {
                    return std::unexpected(
                        "GraphJit event output has no storage");
                }
                auto binding = storage_binding(*output.storage);
                if (!binding) return std::unexpected(std::move(binding.error()));
                auto const& port_storage =
                    plan.ports[*output.storage];
                if (!port_storage.has_producer_overflow_counter) {
                    return std::unexpected(
                        "GraphJit event output storage lost producer telemetry");
                }
                bindings.push_back(ReflectedEventOutputPortBinding{
                    .storage = *binding,
                    .overflow_count = nullptr,
                    .source_type = output.source_type,
                    .history = output.history,
                    .latency = output.latency,
                    .append_existing = output.append_existing,
                });
            }
            result.output_bindings = runtime_bytes_copy(
                builder, module,
                bindings.data(),
                bindings.size() * sizeof(ReflectedEventOutputPortBinding),
                alignof(ReflectedEventOutputPortBinding),
                "__iv_graph_event_outputs_" + std::to_string(primitive_index));
            std::size_t binding_index = 0;
            for (auto const& output : primitive.outputs) {
                if (!output.realtime) continue;
                auto const storage = *output.storage;
                if (storage >= realtime_storage.event_representations.size()
                    || storage
                        >= realtime_storage.event_overflow_counts.size()
                    || realtime_storage.event_overflow_counts[storage]
                        == nullptr) {
                    return std::unexpected(
                        "GraphJit event output lost its resolved storage or telemetry");
                }
                store_runtime_pointer(
                    builder,
                    result.output_bindings,
                    binding_index * sizeof(ReflectedEventOutputPortBinding)
                        + offsetof(ReflectedEventOutputPortBinding, storage)
                        + offsetof(ReflectedEventPortStorageBinding, storage),
                    realtime_storage.event_representations[storage]);
                store_runtime_pointer(
                    builder,
                    result.output_bindings,
                    binding_index * sizeof(ReflectedEventOutputPortBinding)
                        + offsetof(ReflectedEventOutputPortBinding, overflow_count),
                    realtime_storage.event_overflow_counts[storage]);
                ++binding_index;
            }
        }
    }
    return emitted;
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

void store_tick_invocation_span(
    llvm::IRBuilder<>& builder,
    llvm::Value* context_storage,
    std::size_t context_data_offset,
    std::size_t context_size_offset,
    llvm::Value* invocation,
    std::size_t invocation_data_offset,
    std::size_t invocation_size_offset,
    std::size_t begin,
    std::size_t count,
    std::size_t element_size,
    llvm::Twine const& name)
{
    auto& context = builder.getContext();
    auto* byte_type = llvm::Type::getInt8Ty(context);
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };

    auto* source_data_address = byte_offset_pointer(
        builder, invocation, invocation_data_offset, name + ".source.data");
    auto* source_size_address = byte_offset_pointer(
        builder, invocation, invocation_size_offset, name + ".source.size");
    auto* source_data = builder.CreateLoad(
        pointer_type, source_data_address, name + ".data");
    auto* source_size = builder.CreateLoad(
        size_type, source_size_address, name + ".size");
    auto* present = builder.CreateAnd(
        builder.CreateIsNotNull(source_data),
        builder.CreateICmpUGE(source_size, constant(begin + count)),
        name + ".present");
    auto* selected_data = begin == 0
        ? source_data
        : builder.CreateInBoundsGEP(
            byte_type,
            source_data,
            constant(begin * element_size),
            name + ".begin");
    selected_data = builder.CreateSelect(
        present,
        selected_data,
        llvm::ConstantPointerNull::get(pointer_type),
        name + ".selected.data");
    auto* selected_size = builder.CreateSelect(
        present,
        constant(count),
        constant(0),
        name + ".selected.size");

    builder.CreateStore(
        selected_data,
        byte_offset_pointer(
            builder, context_storage, context_data_offset, name + ".target.data"));
    builder.CreateStore(
        selected_size,
        byte_offset_pointer(
            builder, context_storage, context_size_offset, name + ".target.size"));
}

void emit_tick_capture_call(
    llvm::IRBuilder<>& builder,
    llvm::Value* invocation,
    std::size_t invocation_data_offset,
    std::size_t invocation_size_offset,
    std::size_t operation_index,
    std::size_t operation_size,
    std::size_t operation_context_offset,
    std::size_t operation_callback_offset,
    llvm::Value* output_binding,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    llvm::Twine const& name)
{
    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* byte_type = llvm::Type::getInt8Ty(context);
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };

    auto* source_data = builder.CreateLoad(
        pointer_type,
        byte_offset_pointer(
            builder, invocation, invocation_data_offset, name + ".span.data"),
        name + ".data");
    auto* source_size = builder.CreateLoad(
        size_type,
        byte_offset_pointer(
            builder, invocation, invocation_size_offset, name + ".span.size"),
        name + ".size");
    auto* operation_present = builder.CreateAnd(
        builder.CreateIsNotNull(source_data),
        builder.CreateICmpUGT(
            source_size, constant(operation_index), name + ".index.present"),
        name + ".present");

    auto* operation_block = llvm::BasicBlock::Create(
        context, name + ".operation", function);
    auto* call_block = llvm::BasicBlock::Create(
        context, name + ".call", function);
    auto* continuation = llvm::BasicBlock::Create(
        context, name + ".end", function);
    builder.CreateCondBr(operation_present, operation_block, continuation);

    builder.SetInsertPoint(operation_block);
    auto* operation = builder.CreateInBoundsGEP(
        byte_type,
        source_data,
        constant(operation_index * operation_size),
        name + ".operation.ptr");
    auto* opaque_context = builder.CreateLoad(
        pointer_type,
        byte_offset_pointer(
            builder,
            operation,
            operation_context_offset,
            name + ".context.ptr"),
        name + ".context");
    auto* callback = builder.CreateLoad(
        pointer_type,
        byte_offset_pointer(
            builder,
            operation,
            operation_callback_offset,
            name + ".callback.ptr"),
        name + ".callback");
    auto* callable = builder.CreateAnd(
        builder.CreateIsNotNull(opaque_context),
        builder.CreateIsNotNull(callback),
        name + ".callable");
    builder.CreateCondBr(callable, call_block, continuation);

    builder.SetInsertPoint(call_block);
    auto* callback_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {pointer_type, pointer_type, size_type, size_type},
        false);
    builder.CreateCall(
        callback_type,
        callback,
        {opaque_context, output_binding, sample_index, block_size});
    builder.CreateBr(continuation);

    builder.SetInsertPoint(continuation);
}

llvm::Value* overlay_tick_sequential_bindings(
    llvm::IRBuilder<>& builder,
    llvm::Value* static_bindings,
    std::size_t static_binding_count,
    llvm::Value* invocation,
    std::size_t invocation_data_offset,
    std::span<TickSequentialBindingPlan const> dynamic_bindings,
    std::size_t dynamic_begin,
    std::size_t dynamic_count,
    BackgroundEvaluationPlan const& background,
    std::span<detail::PrimitiveSampleInputBindingPlan const> sample_inputs,
    std::span<detail::PrimitiveEventInputBindingPlan const> event_inputs,
    std::size_t element_size,
    std::size_t element_alignment,
    llvm::Twine const& name)
{
    auto& context = builder.getContext();
    auto* byte_type = llvm::Type::getInt8Ty(context);
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };
    auto const alignment = llvm::Align(element_alignment);

    auto* overlaid = builder.CreateAlloca(
        byte_type,
        constant(static_binding_count * element_size),
        name + ".storage");
    overlaid->setAlignment(alignment);
    builder.CreateMemCpy(
        overlaid,
        alignment,
        static_bindings,
        alignment,
        static_binding_count * element_size);

    // The TickInvocationCall built from this graph's TickInvocationWorkspace
    // always carries every slot; an invocation-supplied input has no other
    // binding.
    auto* source_data_address = byte_offset_pointer(
        builder,
        invocation,
        invocation_data_offset,
        name + ".source.data");
    auto* source_data = builder.CreateLoad(
        pointer_type, source_data_address, name + ".data");

    for (std::size_t slot = 0; slot < dynamic_count; ++slot) {
        auto const& binding = dynamic_bindings[dynamic_begin + slot];
        auto const target = background.ports[binding.port]
                                .configured_port.port_index;
        if (target < sample_inputs.size()
            && sample_inputs[target].combines_invocation_supplied) {
            continue;
        }
        if (target < event_inputs.size()
            && event_inputs[target].combines_invocation_supplied) {
            continue;
        }
        auto* invocation_source = builder.CreateInBoundsGEP(
            byte_type,
            source_data,
            constant((dynamic_begin + slot) * element_size),
            name + ".invocation.binding");
        auto* target_binding = builder.CreateInBoundsGEP(
            byte_type,
            overlaid,
            constant(target * element_size),
            name + ".target.binding");
        builder.CreateMemCpy(
            target_binding,
            alignment,
            invocation_source,
            alignment,
            element_size);
    }
    return overlaid;
}

void restore_continuous_input_scratch_bindings(
    llvm::IRBuilder<>& builder,
    llvm::Value* target_bindings,
    llvm::Value* static_bindings,
    std::size_t binding_count)
{
    auto copy_field = [&](std::size_t binding,
                          std::size_t field_offset,
                          std::size_t field_size,
                          std::size_t field_alignment,
                          llvm::Twine const& name) {
        auto const offset = binding
            * sizeof(ReflectedContinuousInputPortBinding) + field_offset;
        builder.CreateMemCpy(
            byte_offset_pointer(builder, target_bindings, offset, name + ".target"),
            llvm::Align(field_alignment),
            byte_offset_pointer(builder, static_bindings, offset, name + ".source"),
            llvm::Align(field_alignment),
            field_size);
    };

    for (std::size_t binding = 0; binding < binding_count; ++binding) {
        copy_field(
            binding,
            offsetof(
                ReflectedContinuousInputPortBinding,
                contiguous_value_capacity),
            sizeof(std::size_t),
            alignof(std::size_t),
            "continuous.scratch.capacity");
        for (std::size_t channel = 0;
             channel < maximum_supported_channel_count; ++channel) {
            copy_field(
                binding,
                offsetof(ReflectedContinuousInputPortBinding, storage)
                    + offsetof(ReflectedContinuousPortStorageBinding, channels)
                    + channel
                        * sizeof(ReflectedContinuousChannelStorageBinding)
                    + offsetof(
                        ReflectedContinuousChannelStorageBinding,
                        contiguous_scratch),
                sizeof(std::byte*),
                alignof(std::byte*),
                "continuous.scratch.pointer");
        }
    }
}

llvm::Value* emit_sequential_begin_ordinal(
    llvm::IRBuilder<>& builder,
    detail::SampleStoragePlan const& storage,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* callback_index,
    std::int32_t pace_exponent,
    llvm::Twine const& name);

struct EmittedLatticeProjection {
    llvm::Value* begin_ordinal = nullptr;
    llvm::Value* value_count = nullptr;
};

EmittedLatticeProjection emit_lattice_projection(
    llvm::IRBuilder<>& builder,
    llvm::Value* project_index,
    llvm::Value* project_count,
    std::int32_t pace_exponent,
    llvm::Twine const& name);

void combine_tick_sequential_sample_bindings(
    llvm::IRBuilder<>& builder,
    llvm::Value* target_bindings,
    llvm::Value* static_bindings,
    llvm::Value* invocation,
    std::size_t invocation_data_offset,
    std::span<TickSequentialBindingPlan const> dynamic_bindings,
    std::size_t dynamic_begin,
    std::size_t dynamic_count,
    BackgroundEvaluationPlan const& background,
    std::span<detail::PrimitiveSampleInputBindingPlan const> inputs,
    detail::SampleStoragePlan const& sample_storage,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    std::string_view combine_symbol)
{
    auto& context = builder.getContext();
    auto* module = builder.GetInsertBlock()->getModule();
    llvm::Function* combine = nullptr;
    auto* byte_type = llvm::Type::getInt8Ty(context);
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };
    auto binding_pointer = [&](llvm::Value* base,
                               std::size_t input,
                               llvm::Twine const& name) {
        return builder.CreateInBoundsGEP(
            byte_type,
            base,
            constant(input * sizeof(ReflectedContinuousInputPortBinding)),
            name);
    };
    auto load_size = [&](llvm::Value* base,
                         std::size_t offset,
                         llvm::Twine const& name) {
        return builder.CreateLoad(
            size_type, byte_offset_pointer(builder, base, offset, name), name);
    };
    auto load_pointer = [&](llvm::Value* base,
                            std::size_t offset,
                            llvm::Twine const& name) {
        return builder.CreateLoad(
            pointer_type, byte_offset_pointer(builder, base, offset, name), name);
    };
    auto const channel_storage_offset = [](std::size_t channel) {
        return offsetof(ReflectedContinuousInputPortBinding, storage)
            + offsetof(ReflectedContinuousPortStorageBinding, channels)
            + channel * sizeof(ReflectedContinuousChannelStorageBinding);
    };
    auto* invocation_data = builder.CreateLoad(
        pointer_type,
        byte_offset_pointer(
            builder, invocation, invocation_data_offset,
            "tick.mixed.sample.invocation.data.ptr"),
        "tick.mixed.sample.invocation.data");

    for (std::size_t slot = 0; slot < dynamic_count; ++slot) {
        auto const& dynamic = dynamic_bindings[dynamic_begin + slot];
        IV_ASSERT(dynamic.port < background.ports.size(),
            "GraphJit mixed sample binding references a missing port");
        auto const input_index = background.ports[dynamic.port]
                                     .configured_port.port_index;
        if (input_index >= inputs.size()
            || !inputs[input_index].combines_invocation_supplied) {
            continue;
        }
        if (combine == nullptr) {
            combine = module->getFunction(
                llvm::StringRef(combine_symbol.data(), combine_symbol.size()));
            IV_ASSERT(combine && !combine->isDeclaration(),
                "GraphJit mixed sample input has no materialized combination callback");
        }
        auto const& planned = inputs[input_index];
        IV_ASSERT(planned.has_contiguous_scratch
                && planned.contiguous_storage_capacity != 0
                && std::has_single_bit(planned.contiguous_storage_capacity)
                && planned.geometry.value_size_bytes != 0,
            "GraphJit mixed sample input has no bounded combination scratch");
        auto* ordinary = binding_pointer(
            static_bindings, input_index, "tick.mixed.sample.ordinary");
        auto* target = binding_pointer(
            target_bindings, input_index, "tick.mixed.sample.target");
        auto* supplied = builder.CreateInBoundsGEP(
            byte_type,
            invocation_data,
            constant((dynamic_begin + slot)
                * sizeof(ReflectedContinuousInputPortBinding)),
            "tick.mixed.sample.supplied");
        auto const projection = emit_lattice_projection(
            builder,
            sample_index,
            block_size,
            planned.absolute_pace_exponent,
            "tick.mixed.sample.projection");
        auto* value_count = builder.CreateAdd(
            projection.value_count,
            constant(planned.history),
            "tick.mixed.sample.value.count");
        auto* ordinary_begin = emit_sequential_begin_ordinal(
            builder,
            sample_storage,
            realtime_storage,
            root_index,
            sample_index,
            planned.absolute_pace_exponent,
            "tick.mixed.sample.ordinary.ordinal");
        auto* supplied_begin = load_size(
            supplied,
            offsetof(ReflectedContinuousInputPortBinding,
                     sequential_begin_ordinal),
            "tick.mixed.sample.supplied.ordinal");
        auto* supplied_latency = load_size(
            supplied,
            offsetof(ReflectedContinuousInputPortBinding, read_latency),
            "tick.mixed.sample.supplied.latency");

        for (std::size_t channel = 0;
             channel < channel_count(planned.channel_layout);
             ++channel) {
            auto const ordinary_channel = channel_storage_offset(channel);
            auto const supplied_channel = channel_storage_offset(channel);
            auto* destination = load_pointer(
                target,
                ordinary_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding,
                               contiguous_scratch),
                "tick.mixed.sample.destination");
            auto* ordinary_storage = load_pointer(
                ordinary,
                ordinary_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding, storage),
                "tick.mixed.sample.ordinary.storage");
            auto* ordinary_capacity = load_size(
                ordinary,
                ordinary_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding,
                               value_capacity),
                "tick.mixed.sample.ordinary.capacity");
            auto* ordinary_stride = load_size(
                ordinary,
                ordinary_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding,
                               value_stride_bytes),
                "tick.mixed.sample.ordinary.stride");
            auto* ordinary_delay = load_size(
                ordinary,
                ordinary_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding, value_delay),
                "tick.mixed.sample.ordinary.delay");
            auto* supplied_storage = load_pointer(
                supplied,
                supplied_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding, storage),
                "tick.mixed.sample.supplied.storage");
            auto* supplied_capacity = load_size(
                supplied,
                supplied_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding,
                               value_capacity),
                "tick.mixed.sample.supplied.capacity");
            auto* supplied_stride = load_size(
                supplied,
                supplied_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding,
                               value_stride_bytes),
                "tick.mixed.sample.supplied.stride");
            auto* supplied_delay = load_size(
                supplied,
                supplied_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding, value_delay),
                "tick.mixed.sample.supplied.delay");

            auto* function = builder.GetInsertBlock()->getParent();
            auto* loop = llvm::BasicBlock::Create(
                context, "tick.mixed.sample.loop", function);
            auto* body = llvm::BasicBlock::Create(
                context, "tick.mixed.sample.body", function);
            auto* done = llvm::BasicBlock::Create(
                context, "tick.mixed.sample.done", function);
            auto* rejected = llvm::BasicBlock::Create(
                context, "tick.mixed.sample.rejected", function);
            auto* preheader = builder.GetInsertBlock();
            builder.CreateBr(loop);
            builder.SetInsertPoint(loop);
            auto* index = builder.CreatePHI(size_type, 2,
                "tick.mixed.sample.index");
            index->addIncoming(constant(0), preheader);
            auto* in_range = builder.CreateICmpULT(index, value_count);
            builder.CreateCondBr(in_range, body, done);

            builder.SetInsertPoint(body);
            auto source_pointer = [&](llvm::Value* storage,
                                      llvm::Value* capacity,
                                      llvm::Value* stride,
                                      llvm::Value* delay,
                                      llvm::Value* begin,
                                      llvm::Value* latency,
                                      llvm::Twine const& name) {
                auto* ordinal = builder.CreateAdd(
                    builder.CreateSub(
                        builder.CreateSub(begin, latency),
                        constant(planned.history)),
                    index);
                ordinal = builder.CreateSub(ordinal, delay);
                auto* slot_index = builder.CreateAnd(
                    ordinal,
                    builder.CreateSub(capacity, constant(1)));
                auto* byte_index = builder.CreateMul(slot_index, stride);
                return builder.CreateInBoundsGEP(
                    byte_type, storage, byte_index, name);
            };
            auto* ordinary_source = source_pointer(
                ordinary_storage,
                ordinary_capacity,
                ordinary_stride,
                ordinary_delay,
                ordinary_begin,
                constant(planned.read_latency),
                "tick.mixed.sample.ordinary.value");
            auto* supplied_source = source_pointer(
                supplied_storage,
                supplied_capacity,
                supplied_stride,
                supplied_delay,
                supplied_begin,
                supplied_latency,
                "tick.mixed.sample.supplied.value");
            auto* destination_value = builder.CreateInBoundsGEP(
                byte_type,
                destination,
                builder.CreateMul(
                    index, constant(planned.geometry.value_size_bytes)),
                "tick.mixed.sample.destination.value");
            builder.CreateMemCpy(
                destination_value,
                llvm::Align(planned.geometry.element_alignment),
                ordinary_source,
                llvm::Align(planned.geometry.element_alignment),
                planned.geometry.value_size_bytes);
            auto* combined = builder.CreateCall(
                combine,
                {constant(input_index),
                 llvm::ConstantInt::get(
                     llvm::Type::getInt8Ty(context),
                     static_cast<std::uint8_t>(planned.geometry.value_type)),
                 destination_value,
                 supplied_source,
                 constant(planned.geometry.value_size_bytes),
                 constant(planned.geometry.elements_per_value)},
                "tick.mixed.sample.combined");
            auto* next = builder.CreateAdd(index, constant(1));
            auto* body_block = builder.GetInsertBlock();
            builder.CreateCondBr(combined, loop, rejected);
            index->addIncoming(next, body_block);

            builder.SetInsertPoint(rejected);
            auto* trap = llvm::Intrinsic::getDeclaration(
                module, llvm::Intrinsic::trap);
            builder.CreateCall(trap);
            builder.CreateUnreachable();

            builder.SetInsertPoint(done);
            store_runtime_pointer(
                builder,
                target,
                ordinary_channel
                    + offsetof(ReflectedContinuousChannelStorageBinding, storage),
                destination);
            builder.CreateStore(
                constant(planned.contiguous_storage_capacity),
                byte_offset_pointer(
                    builder,
                    target,
                    ordinary_channel
                        + offsetof(ReflectedContinuousChannelStorageBinding,
                                   value_capacity),
                    "tick.mixed.sample.target.capacity"));
            builder.CreateStore(
                constant(planned.geometry.value_size_bytes),
                byte_offset_pointer(
                    builder,
                    target,
                    ordinary_channel
                        + offsetof(ReflectedContinuousChannelStorageBinding,
                                   value_stride_bytes),
                    "tick.mixed.sample.target.stride"));
            builder.CreateStore(
                constant(0),
                byte_offset_pointer(
                    builder,
                    target,
                    ordinary_channel
                        + offsetof(ReflectedContinuousChannelStorageBinding,
                                   value_delay),
                    "tick.mixed.sample.target.delay"));
        }
        builder.CreateStore(
            constant(planned.contiguous_storage_capacity),
            byte_offset_pointer(
                builder,
                target,
                offsetof(ReflectedContinuousInputPortBinding, storage)
                    + offsetof(ReflectedContinuousPortStorageBinding,
                               value_capacity),
                "tick.mixed.sample.target.storage.capacity"));
        builder.CreateStore(
            constant(0),
            byte_offset_pointer(
                builder,
                target,
                offsetof(ReflectedContinuousInputPortBinding, storage)
                    + offsetof(ReflectedContinuousPortStorageBinding,
                               storage_latency),
                "tick.mixed.sample.target.storage.latency"));
        builder.CreateStore(
            constant(0),
            byte_offset_pointer(
                builder,
                target,
                offsetof(ReflectedContinuousInputPortBinding, read_latency),
                "tick.mixed.sample.target.read.latency"));
        builder.CreateStore(
            constant(planned.history),
            byte_offset_pointer(
                builder,
                target,
                offsetof(ReflectedContinuousInputPortBinding,
                         sequential_begin_ordinal),
                "tick.mixed.sample.target.ordinal"));
    }
}

void combine_tick_sequential_event_bindings(
    llvm::IRBuilder<>& builder,
    llvm::Value* target_bindings,
    llvm::Value* invocation,
    std::size_t invocation_data_offset,
    std::span<TickSequentialBindingPlan const> dynamic_bindings,
    std::size_t dynamic_begin,
    std::size_t dynamic_count,
    BackgroundEvaluationPlan const& background,
    std::span<detail::PrimitiveEventInputBindingPlan const> inputs,
    detail::EventPortBindingPlan const& event_ports,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* sample_index)
{
    auto& context = builder.getContext();
    auto* module = builder.GetInsertBlock()->getModule();
    auto* byte_type = llvm::Type::getInt8Ty(context);
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* byte_bool_type = llvm::Type::getInt8Ty(context);
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };
    auto binding_pointer = [&](llvm::Value* base,
                               std::size_t input,
                               llvm::Twine const& name) {
        return builder.CreateInBoundsGEP(
            byte_type,
            base,
            constant(input * sizeof(ReflectedEventInputPortBinding)),
            name);
    };
    auto load_size = [&](llvm::Value* base,
                         std::size_t offset,
                         llvm::Twine const& name) {
        return builder.CreateLoad(
            size_type, byte_offset_pointer(builder, base, offset, name), name);
    };
    auto load_pointer = [&](llvm::Value* base,
                            std::size_t offset,
                            llvm::Twine const& name) {
        return builder.CreateLoad(
            pointer_type, byte_offset_pointer(builder, base, offset, name), name);
    };
    struct Sequence {
        llvm::Value* events = nullptr;
        llvm::Value* source_indices = nullptr;
        llvm::Value* capacity = nullptr;
        llvm::Value* read_index = nullptr;
        llvm::Value* write_index = nullptr;
    };
    auto load_sequence = [&](llvm::Value* binding,
                             llvm::Twine const& name) {
        auto const storage_offset = offsetof(
            ReflectedEventInputPortBinding, storage);
        auto* base = load_pointer(
            binding,
            storage_offset
                + offsetof(ReflectedEventPortStorageBinding, storage),
            name + ".storage");
        auto* count_offset = load_size(
            binding,
            storage_offset
                + offsetof(ReflectedEventPortStorageBinding, count_offset),
            name + ".count.offset");
        auto* read_offset = load_size(
            binding,
            storage_offset
                + offsetof(ReflectedEventPortStorageBinding, read_index_offset),
            name + ".read.offset");
        auto* write_offset = load_size(
            binding,
            storage_offset
                + offsetof(ReflectedEventPortStorageBinding, write_index_offset),
            name + ".write.offset");
        auto* events_offset = load_size(
            binding,
            storage_offset
                + offsetof(ReflectedEventPortStorageBinding, events_offset),
            name + ".events.offset");
        auto* capacity = load_size(
            binding,
            storage_offset
                + offsetof(ReflectedEventPortStorageBinding, event_capacity),
            name + ".capacity");
        auto* persistent = builder.CreateLoad(
            byte_bool_type,
            byte_offset_pointer(
                builder,
                binding,
                storage_offset
                    + offsetof(
                        ReflectedEventPortStorageBinding, persistent_ring),
                name + ".persistent.ptr"),
            name + ".persistent");
        auto* count = builder.CreateLoad(
            size_type,
            builder.CreateInBoundsGEP(
                byte_type, base, count_offset, name + ".count.ptr"),
            name + ".count");
        auto* ring_read = builder.CreateLoad(
            size_type,
            builder.CreateInBoundsGEP(
                byte_type, base, read_offset, name + ".read.ptr"),
            name + ".read");
        auto* ring_write = builder.CreateLoad(
            size_type,
            builder.CreateInBoundsGEP(
                byte_type, base, write_offset, name + ".write.ptr"),
            name + ".write");
        auto* is_persistent = builder.CreateICmpNE(
            persistent,
            llvm::ConstantInt::get(byte_bool_type, 0),
            name + ".is.persistent");
        return Sequence{
            .events = builder.CreateInBoundsGEP(
                byte_type, base, events_offset, name + ".events"),
            .source_indices = load_pointer(
                binding,
                offsetof(
                    ReflectedEventInputPortBinding,
                    semantic_source_indices),
                name + ".source.indices"),
            .capacity = capacity,
            .read_index = builder.CreateSelect(
                is_persistent, ring_read, constant(0), name + ".selected.read"),
            .write_index = builder.CreateSelect(
                is_persistent, ring_write, count, name + ".selected.write"),
        };
    };

    auto* invocation_data = builder.CreateLoad(
        pointer_type,
        byte_offset_pointer(
            builder,
            invocation,
            invocation_data_offset,
            "tick.mixed.event.invocation.data.ptr"),
        "tick.mixed.event.invocation.data");
    auto* helper_type = llvm::FunctionType::get(
        size_type,
        {pointer_type, size_type,
         pointer_type, pointer_type, size_type, size_type, size_type, size_type,
         pointer_type, pointer_type, size_type, size_type, size_type, size_type,
         sample_index->getType()},
        false);
    auto helper = module->getOrInsertFunction(
        detail::mixed_event_sequence_merge_symbol, helper_type);

    for (std::size_t slot = 0; slot < dynamic_count; ++slot) {
        auto const& dynamic = dynamic_bindings[dynamic_begin + slot];
        IV_ASSERT(dynamic.port < background.ports.size(),
            "GraphJit mixed event binding references a missing port");
        auto const input_index = background.ports[dynamic.port]
                                     .configured_port.port_index;
        if (input_index >= inputs.size()
            || !inputs[input_index].combines_invocation_supplied) {
            continue;
        }
        auto const& planned = inputs[input_index];
        IV_ASSERT(planned.storage
                && planned.combined_storage
                && *planned.storage < event_ports.ports.size()
                && *planned.combined_storage < event_ports.ports.size()
                && *planned.combined_storage
                    < realtime_storage.event_representations.size(),
            "GraphJit mixed event input lost its planned storage");
        auto const& combined = event_ports.ports[*planned.combined_storage];
        auto* target_binding = binding_pointer(
            target_bindings, input_index, "tick.mixed.event.target.binding");
        auto* supplied_binding = builder.CreateInBoundsGEP(
            byte_type,
            invocation_data,
            constant((dynamic_begin + slot)
                * sizeof(ReflectedEventInputPortBinding)),
            "tick.mixed.event.supplied.binding");
        auto const realtime_sequence = load_sequence(
            target_binding, "tick.mixed.event.realtime");
        auto const supplied_sequence = load_sequence(
            supplied_binding, "tick.mixed.event.supplied");
        auto* combined_base = realtime_storage.event_representations[
            *planned.combined_storage];
        auto* combined_events = byte_offset_pointer(
            builder,
            combined_base,
            combined.events_relative_offset,
            "tick.mixed.event.combined.events");
        auto* merged_count = builder.CreateCall(
            helper,
            {combined_events,
             constant(combined.event_capacity),
             realtime_sequence.events,
             realtime_sequence.source_indices,
             constant(planned.realtime_semantic_source_index.value_or(
                 std::numeric_limits<std::size_t>::max())),
             realtime_sequence.capacity,
             realtime_sequence.read_index,
             realtime_sequence.write_index,
             supplied_sequence.events,
             supplied_sequence.source_indices,
             constant(std::numeric_limits<std::size_t>::max()),
             supplied_sequence.capacity,
             supplied_sequence.read_index,
             supplied_sequence.write_index,
             sample_index},
            "tick.mixed.event.count");

        auto* function = builder.GetInsertBlock()->getParent();
        auto* accepted = llvm::BasicBlock::Create(
            context, "tick.mixed.event.accepted", function);
        auto* rejected = llvm::BasicBlock::Create(
            context, "tick.mixed.event.rejected", function);
        builder.CreateCondBr(
            builder.CreateICmpNE(
                merged_count,
                constant(std::numeric_limits<std::size_t>::max())),
            accepted,
            rejected);
        builder.SetInsertPoint(rejected);
        auto* trap = llvm::Intrinsic::getDeclaration(module, llvm::Intrinsic::trap);
        builder.CreateCall(trap);
        builder.CreateUnreachable();
        builder.SetInsertPoint(accepted);
        builder.CreateStore(
            merged_count,
            byte_offset_pointer(
                builder,
                combined_base,
                combined.count_relative_offset,
                "tick.mixed.event.combined.count"));

        ReflectedEventInputPortBinding combined_binding{
            .storage = ReflectedEventPortStorageBinding{
                .storage = nullptr,
                .count_offset = combined.count_relative_offset,
                .read_index_offset = combined.read_index_relative_offset,
                .write_index_offset = combined.write_index_relative_offset,
                .events_offset = combined.events_relative_offset,
                .event_capacity = combined.event_capacity,
                .type = combined.type,
                .persistent_ring = combined.persistent_ring,
            },
        };
        auto* binding = runtime_bytes_copy(
            builder,
            *module,
            &combined_binding,
            sizeof(combined_binding),
            alignof(ReflectedEventInputPortBinding),
            "tick.mixed.event.combined.binding."
                + std::to_string(input_index));
        store_runtime_pointer(
            builder,
            binding,
            offsetof(ReflectedEventInputPortBinding, storage)
                + offsetof(ReflectedEventPortStorageBinding, storage),
            combined_base);
        builder.CreateMemCpy(
            target_binding,
            llvm::Align(alignof(ReflectedEventInputPortBinding)),
            binding,
            llvm::Align(alignof(ReflectedEventInputPortBinding)),
            sizeof(ReflectedEventInputPortBinding));
    }
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
    auto* data = byte_offset_pointer(
        builder, storage_base, storage_offset, "span.data");
    store_context_span_pointer(
        builder,
        context_storage,
        data_field_offset,
        size_field_offset,
        data,
        span_size);
}

void store_sequential_begin_ordinal(
    llvm::IRBuilder<>& builder,
    llvm::Value* bindings,
    std::size_t binding_index,
    std::size_t binding_size,
    std::size_t field_offset,
    llvm::Value* ordinal,
    llvm::Twine const& name)
{
    auto* pointer = byte_offset_pointer(
        builder,
        bindings,
        binding_index * binding_size + field_offset,
        name + ".pointer");
    builder.CreateStore(ordinal, pointer);
}

struct EmittedPrimitiveTickContext {
    llvm::Value* storage = nullptr;
    llvm::Value* continuous_input_bindings = nullptr;
    llvm::Value* event_input_bindings = nullptr;
};

EmittedPrimitiveTickContext emit_primitive_tick_context(
    llvm::IRBuilder<>& builder,
    EmittedNodeConfiguration const& configuration,
    detail::PrimitiveStoragePlan const& storage,
    EmittedPrimitiveSamplePorts const& sample_ports,
    EmittedPrimitiveEventPorts const& event_ports,
    detail::PrimitiveSamplePortPlan const& sample_plan,
    detail::SampleStoragePlan const& sample_storage,
    detail::PrimitiveEventPortPlan const& event_plan,
    detail::EventPortBindingPlan const& event_storage,
    BackgroundEvaluationPlan const* tick_background,
    TickNodeInvocationPlan const* tick_invocation,
    llvm::Value* invocation,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    std::string_view combine_sequential_input_values_symbol)
{
    auto* storage_base = realtime_storage.persistent_base;
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
    llvm::Value* continuous_input_bindings = sample_ports.input_bindings;
    llvm::Value* event_input_bindings = event_ports.input_bindings;
    if (sample_ports.input_count != 0) {
        auto* bindings = continuous_input_bindings;
        if (tick_background != nullptr && tick_invocation != nullptr
            && tick_invocation->sequential_sample_count != 0) {
            auto const call_offsets = tick_invocation_byte_offsets();
            bindings = overlay_tick_sequential_bindings(
                builder,
                bindings,
                sample_ports.input_count,
                invocation,
                call_offsets.sequential_sample_inputs_data,
                tick_background->tick_runtime.sequential_sample_inputs,
                tick_invocation->sequential_sample_begin,
                tick_invocation->sequential_sample_count,
                *tick_background,
                sample_plan.inputs,
                {},
                sizeof(ReflectedContinuousInputPortBinding),
                alignof(ReflectedContinuousInputPortBinding),
                "tick.sequential.sample");
            restore_continuous_input_scratch_bindings(
                builder,
                bindings,
                sample_ports.input_bindings,
                sample_ports.input_count);
            combine_tick_sequential_sample_bindings(
                builder,
                bindings,
                sample_ports.input_bindings,
                invocation,
                call_offsets.sequential_sample_inputs_data,
                tick_background->tick_runtime.sequential_sample_inputs,
                tick_invocation->sequential_sample_begin,
                tick_invocation->sequential_sample_count,
                *tick_background,
                sample_plan.inputs,
                sample_storage,
                realtime_storage,
                root_index,
                sample_index,
                block_size,
                combine_sequential_input_values_symbol);
        }
        for (std::size_t input = 0; input < sample_plan.inputs.size(); ++input) {
            auto const external = tick_background != nullptr
                && tick_invocation != nullptr
                && std::ranges::any_of(
                    std::span{
                        tick_background->tick_runtime.sequential_sample_inputs}
                        .subspan(
                            tick_invocation->sequential_sample_begin,
                            tick_invocation->sequential_sample_count),
                    [&, input](auto const& selected) {
                        auto const& port = tick_background->ports[selected.port];
                        return port.configured_port.port_index == input;
                    });
            if (external) continue;
            auto* ordinal = emit_sequential_begin_ordinal(
                builder,
                sample_storage,
                realtime_storage,
                root_index,
                sample_index,
                sample_plan.inputs[input].absolute_pace_exponent,
                "continuous.input.sequential");
            store_sequential_begin_ordinal(
                builder,
                bindings,
                input,
                sizeof(ReflectedContinuousInputPortBinding),
                offsetof(
                    ReflectedContinuousInputPortBinding,
                    sequential_begin_ordinal),
                ordinal,
                "continuous.input.sequential");
        }
        store_context_span_pointer(
            builder,
            context_storage,
            offsets.continuous_input_bindings_data,
            offsets.continuous_input_bindings_size,
            bindings,
            sample_ports.input_count);
        continuous_input_bindings = bindings;
    }
    if (sample_ports.output_count != 0) {
        for (std::size_t output = 0;
             output < sample_plan.outputs.size(); ++output) {
            if (!sample_plan.outputs[output].realtime) continue;
            auto* ordinal = emit_sequential_begin_ordinal(
                builder,
                sample_storage,
                realtime_storage,
                root_index,
                sample_index,
                sample_plan.outputs[output].absolute_pace_exponent,
                "continuous.output.sequential");
            auto const compact_output = static_cast<std::size_t>(
                std::ranges::count_if(
                    std::span{sample_plan.outputs}.first(output),
                    [](auto const& candidate) { return candidate.realtime; }));
            store_sequential_begin_ordinal(
                builder,
                sample_ports.output_bindings,
                compact_output,
                sizeof(ReflectedContinuousOutputPortBinding),
                offsetof(
                    ReflectedContinuousOutputPortBinding,
                    sequential_begin_ordinal),
                ordinal,
                "continuous.output.sequential");
        }
        store_context_span_pointer(
            builder,
            context_storage,
            offsets.continuous_output_bindings_data,
            offsets.continuous_output_bindings_size,
            sample_ports.output_bindings,
            sample_ports.output_count);
    }
    if (event_ports.input_count != 0) {
        auto* bindings = event_input_bindings;
        if (tick_background != nullptr && tick_invocation != nullptr
            && tick_invocation->sequential_event_count != 0) {
            auto const call_offsets = tick_invocation_byte_offsets();
            bindings = overlay_tick_sequential_bindings(
                builder,
                bindings,
                event_ports.input_count,
                invocation,
                call_offsets.sequential_event_inputs_data,
                tick_background->tick_runtime.sequential_event_inputs,
                tick_invocation->sequential_event_begin,
                tick_invocation->sequential_event_count,
                *tick_background,
                {},
                event_plan.inputs,
                sizeof(ReflectedEventInputPortBinding),
                alignof(ReflectedEventInputPortBinding),
                "tick.sequential.event");
            combine_tick_sequential_event_bindings(
                builder,
                bindings,
                invocation,
                call_offsets.sequential_event_inputs_data,
                tick_background->tick_runtime.sequential_event_inputs,
                tick_invocation->sequential_event_begin,
                tick_invocation->sequential_event_count,
                *tick_background,
                event_plan.inputs,
                event_storage,
                realtime_storage,
                sample_index);
        }
        store_context_span_pointer(
            builder,
            context_storage,
            offsets.event_input_bindings_data,
            offsets.event_input_bindings_size,
            bindings,
            event_ports.input_count);
        event_input_bindings = bindings;
    }
    if (event_ports.output_count != 0) {
        store_context_span_pointer(
            builder,
            context_storage,
            offsets.event_output_bindings_data,
            offsets.event_output_bindings_size,
            event_ports.output_bindings,
            event_ports.output_count);
    }
    if (tick_invocation != nullptr) {
        auto const call_offsets = tick_invocation_byte_offsets();
        if (tick_invocation->random_access_sample_count != 0) {
            store_tick_invocation_span(
                builder,
                context_storage,
                offsets.random_access_inputs_data,
                offsets.random_access_inputs_size,
                invocation,
                call_offsets.random_access_sample_inputs_data,
                call_offsets.random_access_sample_inputs_size,
                tick_invocation->random_access_sample_begin,
                tick_invocation->random_access_sample_count,
                sizeof(RandomAccessSampleInputPort),
                "tick.random_access.sample");
        }
        if (tick_invocation->random_access_event_count != 0) {
            store_tick_invocation_span(
                builder,
                context_storage,
                offsets.random_access_event_inputs_data,
                offsets.random_access_event_inputs_size,
                invocation,
                call_offsets.random_access_event_inputs_data,
                call_offsets.random_access_event_inputs_size,
                tick_invocation->random_access_event_begin,
                tick_invocation->random_access_event_count,
                sizeof(RandomAccessEventInputPort),
                "tick.random_access.event");
        }
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

    return {
        .storage = context_storage,
        .continuous_input_bindings = continuous_input_bindings,
        .event_input_bindings = event_input_bindings,
    };
}

// Values computed before a primitive's Tick/skip choice. A ticking primitive
// passes them to emit_primitive_activity_update() after its callback has
// written the current outputs; a skipped primitive leaves its activity state
// unchanged.
struct EmittedPrimitiveActivity {
    llvm::Value* dormant_pointer = nullptr;
    llvm::Value* silent_samples_pointer = nullptr;
    llvm::Value* inputs_constant = nullptr;
    llvm::Value* inputs_unchanged = nullptr;
    llvm::Value* should_skip = nullptr;
};

EmittedPrimitiveActivity emit_primitive_activity_decision(
    llvm::IRBuilder<>& builder,
    detail::DeclarationPlan const& declarations,
    ActivityEmissionCache& activity_cache,
    detail::PrimitiveActivityPlan const& activity,
    EmittedPrimitiveEventPorts const& event_ports,
    detail::PrimitiveSamplePortPlan const& sample_plan,
    detail::PrimitiveEventPortPlan const& event_plan,
    BackgroundEvaluationPlan const* tick_background,
    TickNodeInvocationPlan const* tick_invocation,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* continuous_input_bindings,
    llvm::Value* event_input_bindings,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    auto& llvm_context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        llvm_context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* byte_type = llvm::Type::getInt8Ty(llvm_context);
    auto* bool_type = llvm::Type::getInt1Ty(llvm_context);
    auto* pointer_type = llvm::PointerType::get(llvm_context, 0);
    auto* true_value = llvm::ConstantInt::getTrue(llvm_context);
    auto size_constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };
    auto binding_pointer = [&](llvm::Value* base,
                               std::size_t index,
                               std::size_t stride,
                               llvm::Twine const& name) {
        return builder.CreateInBoundsGEP(
            byte_type, base, size_constant(index * stride), name);
    };
    auto* int32_type = llvm::Type::getInt32Ty(llvm_context);
    auto* storage_base = realtime_storage.persistent_base;
    auto* activity_state = byte_offset_pointer(
        builder,
        storage_base,
        activity.storage_offset,
        "activity.state");
    auto* dormant_pointer = byte_offset_pointer(
        builder,
        activity_state,
        offsetof(detail::PrimitiveActivityState, dormant),
        "activity.dormant.pointer");
    auto* silent_samples_pointer = byte_offset_pointer(
        builder,
        activity_state,
        offsetof(detail::PrimitiveActivityState, silent_samples),
        "activity.silent.pointer");

    llvm::Value* inputs_constant = true_value;
    llvm::Value* inputs_unchanged = true_value;
    auto input_activity_type = llvm::FunctionType::get(
        int32_type,
        {pointer_type, size_type, size_type, pointer_type, pointer_type},
        false);
    auto input_activity = builder.GetInsertBlock()->getModule()
        ->getOrInsertFunction(
            detail::continuous_input_activity_symbol,
            input_activity_type);
    IV_ASSERT(
        activity.continuous_inputs.size() == sample_plan.inputs.size(),
        "GraphJit activity input plan must match the primitive binding plan");
    for (std::size_t input_index = 0;
         input_index < activity.continuous_inputs.size(); ++input_index) {
        auto const& input_activity_plan =
            activity.continuous_inputs[input_index];
        if (input_activity_plan.static_constant) continue;
        auto const& binding_plan = sample_plan.inputs[input_index];
        IV_ASSERT(
            input_activity_plan.channels.size()
                == (binding_plan.invocation_supplied
                    ? channel_count(binding_plan.channel_layout)
                    : binding_plan.channels.size()),
            "GraphJit activity channel plan must match the primitive binding");
        auto const projection = emit_lattice_projection(
            builder,
            sample_index,
            block_size,
            binding_plan.absolute_pace_exponent,
            "activity.input.projection");
        auto* binding = binding_pointer(
            continuous_input_bindings,
            input_index,
            sizeof(ReflectedContinuousInputPortBinding),
            "activity.input.binding");
        for (std::size_t channel = 0;
             channel < input_activity_plan.channels.size(); ++channel) {
            auto const& channel_plan = input_activity_plan.channels[channel];
            IV_ASSERT(
                channel_plan.frontier
                    < declarations.activity_frontiers.size(),
                "GraphJit activity channel must reference a frontier");
            auto const& frontier = declarations.activity_frontiers[
                channel_plan.frontier];
            // An invocation-supplied frontier has no realtime representation
            // and therefore no realtime writer that could invalidate it.
            IV_ASSERT(
                frontier.representation == detail::no_sample_representation
                    || frontier.representation
                        < activity_cache.representation_epochs.size(),
                "GraphJit activity frontier must reference sample storage");
            auto const epoch =
                frontier.representation == detail::no_sample_representation
                ? std::size_t{0}
                : activity_cache.representation_epochs[
                    frontier.representation];
            auto summary = std::ranges::find_if(
                activity_cache.continuous,
                [&](EmittedContinuousActivitySummary const& candidate) {
                    return candidate.frontier == channel_plan.frontier
                        && candidate.representation_epoch == epoch
                        && candidate.sample_index == sample_index
                        && candidate.block_size == block_size;
                });
            llvm::Value* flags = nullptr;
            if (summary != activity_cache.continuous.end()) {
                flags = summary->flags;
            } else {
                auto* valid = byte_offset_pointer(
                    builder,
                    storage_base,
                    frontier.remembered_valid_storage_offset,
                    "activity.input.valid");
                auto* remembered = byte_offset_pointer(
                    builder,
                    storage_base,
                    frontier.remembered_value_storage_offset,
                    "activity.input.remembered");
                flags = builder.CreateCall(
                    input_activity,
                    {binding,
                     size_constant(channel),
                     projection.value_count,
                     valid,
                     remembered},
                    "activity.input.flags");
                activity_cache.continuous.push_back(
                    EmittedContinuousActivitySummary{
                        .frontier = channel_plan.frontier,
                        .representation_epoch = epoch,
                        .sample_index = sample_index,
                        .block_size = block_size,
                        .flags = flags,
                    });
            }
            auto* constant = builder.CreateICmpNE(
                builder.CreateAnd(
                    flags,
                    llvm::ConstantInt::get(
                        int32_type,
                        detail::continuous_input_constant)),
                llvm::ConstantInt::get(int32_type, 0),
                "activity.input.constant");
            auto* unchanged = builder.CreateICmpNE(
                builder.CreateAnd(
                    flags,
                    llvm::ConstantInt::get(
                        int32_type,
                        detail::continuous_input_unchanged)),
                llvm::ConstantInt::get(int32_type, 0),
                "activity.input.unchanged");
            inputs_constant = builder.CreateAnd(
                inputs_constant, constant, "activity.inputs.constant");
            inputs_unchanged = builder.CreateAnd(
                inputs_unchanged, unchanged, "activity.inputs.unchanged");
        }
    }

    auto event_input_type = llvm::FunctionType::get(
        bool_type,
        {pointer_type, size_type, size_type},
        false);
    auto event_input_empty = builder.GetInsertBlock()->getModule()
        ->getOrInsertFunction(
            detail::event_input_activity_symbol,
            event_input_type);
    IV_ASSERT(
        event_plan.inputs.size() == event_ports.input_count,
        "GraphJit activity event plan must match the primitive binding plan");
    for (std::size_t input = 0; input < event_ports.input_count; ++input) {
        auto* binding = binding_pointer(
            event_input_bindings,
            input,
            sizeof(ReflectedEventInputPortBinding),
            "activity.event.input.binding");
        auto const externally_bound = tick_background != nullptr
            && tick_invocation != nullptr
            && std::ranges::any_of(
                std::span{tick_background->tick_runtime
                              .sequential_event_inputs}
                    .subspan(
                        tick_invocation->sequential_event_begin,
                        tick_invocation->sequential_event_count),
                [&, input](auto const& selected) {
                    auto const& port = tick_background->ports[selected.port];
                    return port.kind == PortKind::event
                        && port.configured_port.port_index == input;
                });
        auto const storage = event_plan.inputs[input].storage;
        llvm::Value* empty = nullptr;
        if (!externally_bound && storage
            && *storage
                < activity_cache.event_representation_epochs.size()) {
            auto const epoch = activity_cache.event_representation_epochs[
                *storage];
            auto summary = std::ranges::find_if(
                activity_cache.events,
                [&](EmittedEventActivitySummary const& candidate) {
                    return candidate.representation == *storage
                        && candidate.representation_epoch == epoch
                        && candidate.sample_index == sample_index
                        && candidate.block_size == block_size;
                });
            if (summary != activity_cache.events.end()) {
                empty = summary->empty;
            } else {
                empty = builder.CreateCall(
                    event_input_empty,
                    {binding, sample_index, block_size},
                    "activity.event.input.empty");
                activity_cache.events.push_back(
                    EmittedEventActivitySummary{
                        .representation = *storage,
                        .representation_epoch = epoch,
                        .sample_index = sample_index,
                        .block_size = block_size,
                        .empty = empty,
                    });
            }
        } else {
            empty = builder.CreateCall(
                event_input_empty,
                {binding, sample_index, block_size},
                "activity.event.input.empty");
        }
        inputs_constant = builder.CreateAnd(
            inputs_constant, empty, "activity.inputs.quiescent");
        inputs_unchanged = builder.CreateAnd(
            inputs_unchanged, empty, "activity.inputs.unchanged");
    }

    auto* dormant = builder.CreateICmpNE(
        builder.CreateLoad(byte_type, dormant_pointer, "activity.dormant"),
        llvm::ConstantInt::get(byte_type, 0),
        "activity.is.dormant");
    auto* should_skip = builder.CreateAnd(
        dormant, inputs_unchanged, "activity.should.skip");
    return {
        .dormant_pointer = dormant_pointer,
        .silent_samples_pointer = silent_samples_pointer,
        .inputs_constant = inputs_constant,
        .inputs_unchanged = inputs_unchanged,
        .should_skip = should_skip,
    };
}

void emit_primitive_activity_update(
    llvm::IRBuilder<>& builder,
    detail::PrimitiveActivityPlan const& activity,
    EmittedPrimitiveActivity const& decision,
    EmittedPrimitiveSamplePorts const& sample_ports,
    EmittedPrimitiveEventPorts const& event_ports,
    detail::PrimitiveSamplePortPlan const& sample_plan,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    auto& llvm_context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        llvm_context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* byte_type = llvm::Type::getInt8Ty(llvm_context);
    auto* bool_type = llvm::Type::getInt1Ty(llvm_context);
    auto* pointer_type = llvm::PointerType::get(llvm_context, 0);
    auto* true_value = llvm::ConstantInt::getTrue(llvm_context);
    auto size_constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };
    auto binding_pointer = [&](llvm::Value* base,
                               std::size_t index,
                               std::size_t stride,
                               llvm::Twine const& name) {
        return builder.CreateInBoundsGEP(
            byte_type, base, size_constant(index * stride), name);
    };
    auto* false_value = llvm::ConstantInt::getFalse(llvm_context);
    auto* dormant_pointer = decision.dormant_pointer;
    auto* silent_samples_pointer = decision.silent_samples_pointer;
    auto* inputs_constant = decision.inputs_constant;
    auto* inputs_unchanged = decision.inputs_unchanged;

    auto const has_output = sample_ports.output_count != 0
        || event_ports.output_count != 0;
    llvm::Value* outputs_clear = has_output ? true_value : false_value;
    auto output_clear_type = llvm::FunctionType::get(
        bool_type,
        {pointer_type, size_type, size_type},
        false);
    auto output_is_clear = builder.GetInsertBlock()->getModule()
        ->getOrInsertFunction(
            detail::continuous_output_clear_symbol,
            output_clear_type);
    std::size_t compact_output = 0;
    for (auto const& output_plan : sample_plan.outputs) {
        if (!output_plan.realtime) continue;
        IV_ASSERT(
            compact_output
                < activity.continuous_output_channel_counts.size(),
            "GraphJit activity output plan must match the primitive binding");
        auto const projection = emit_lattice_projection(
            builder,
            sample_index,
            block_size,
            output_plan.absolute_pace_exponent,
            "activity.output.projection");
        auto* binding = binding_pointer(
            sample_ports.output_bindings,
            compact_output,
            sizeof(ReflectedContinuousOutputPortBinding),
            "activity.output.binding");
        auto const channels =
            activity.continuous_output_channel_counts[compact_output++];
        for (std::size_t channel = 0; channel < channels; ++channel) {
            auto* clear = builder.CreateCall(
                output_is_clear,
                {binding, size_constant(channel), projection.value_count},
                "activity.output.clear");
            outputs_clear = builder.CreateAnd(
                outputs_clear, clear, "activity.outputs.clear");
        }
    }

    auto event_output_type = llvm::FunctionType::get(
        bool_type,
        {pointer_type, size_type, size_type},
        false);
    auto event_output_empty = builder.GetInsertBlock()->getModule()
        ->getOrInsertFunction(
            detail::event_output_activity_symbol,
            event_output_type);
    for (std::size_t output = 0; output < event_ports.output_count; ++output) {
        auto* binding = binding_pointer(
            event_ports.output_bindings,
            output,
            sizeof(ReflectedEventOutputPortBinding),
            "activity.event.output.binding");
        auto* empty = builder.CreateCall(
            event_output_empty,
            {binding, sample_index, block_size},
            "activity.event.output.empty");
        outputs_clear = builder.CreateAnd(
            outputs_clear, empty, "activity.outputs.clear");
    }

    auto* quiescent = builder.CreateAnd(
        inputs_constant, outputs_clear, "activity.quiescent");
    auto* accumulated = builder.CreateLoad(
        size_type, silent_samples_pointer, "activity.silent.samples");
    // A changed constant input starts a new quiet interval. In particular, a
    // dormant primitive must not reuse the interval which preceded its wake.
    // The current callback block may still be the first quiet block when it
    // authors only cleared outputs.
    auto* accumulation_base = builder.CreateSelect(
        inputs_unchanged,
        accumulated,
        llvm::ConstantInt::get(size_type, 0),
        "activity.silent.base");
    auto* sum = builder.CreateAdd(
        accumulation_base, block_size, "activity.silent.sum");
    auto* overflow = builder.CreateICmpULT(
        sum, accumulation_base, "activity.silent.overflow");
    auto* saturated = builder.CreateSelect(
        overflow,
        llvm::ConstantInt::get(
            size_type, std::numeric_limits<std::size_t>::max()),
        sum,
        "activity.silent.saturated");
    auto* next_silent = builder.CreateSelect(
        quiescent,
        saturated,
        llvm::ConstantInt::get(size_type, 0),
        "activity.silent.next");
    builder.CreateStore(next_silent, silent_samples_pointer);
    auto* ttl_elapsed = builder.CreateICmpUGE(
        next_silent,
        size_constant(activity.ttl_samples),
        "activity.ttl.elapsed");
    auto* next_dormant = builder.CreateAnd(
        quiescent, ttl_elapsed, "activity.dormant.next");
    builder.CreateStore(
        builder.CreateZExt(next_dormant, byte_type), dormant_pointer);
}

void emit_primitive_call(
    llvm::IRBuilder<>& builder,
    llvm::Function* tick_callback,
    llvm::Function* skip_callback,
    detail::DeclarationPlan const& declarations,
    ActivityEmissionCache& activity_cache,
    detail::PrimitiveActivityPlan const* activity,
    bool force_skip,
    EmittedNodeConfiguration const& configuration,
    detail::PrimitiveStoragePlan const& storage,
    EmittedPrimitiveSamplePorts const& sample_ports,
    EmittedPrimitiveEventPorts const& event_ports,
    detail::PrimitiveSamplePortPlan const& sample_plan,
    detail::SampleStoragePlan const& sample_storage,
    detail::PrimitiveEventPortPlan const& event_plan,
    detail::EventPortBindingPlan const& event_storage,
    BackgroundEvaluationPlan const* tick_background,
    TickNodeInvocationPlan const* tick_invocation,
    llvm::Value* invocation,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    std::string_view combine_sequential_input_values_symbol)
{
    auto const emitted_context = emit_primitive_tick_context(
        builder,
        configuration,
        storage,
        sample_ports,
        event_ports,
        sample_plan,
        sample_storage,
        event_plan,
        event_storage,
        tick_background,
        tick_invocation,
        invocation,
        realtime_storage,
        root_index,
        sample_index,
        block_size,
        combine_sequential_input_values_symbol);
    auto* context_storage = emitted_context.storage;
    auto* continuous_input_bindings =
        emitted_context.continuous_input_bindings;
    auto* event_input_bindings = emitted_context.event_input_bindings;

    auto invoke = [&](llvm::Function* callback) {
        auto* call = builder.CreateCall(
            callback,
            {configuration.node_config,
             context_storage,
             sample_index,
             block_size});
        call->setCallingConv(callback->getCallingConv());
    };
    if (force_skip) {
        invoke(skip_callback);
        return;
    }
    if (activity == nullptr) {
        invoke(tick_callback);
        return;
    }

    auto& llvm_context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto const decision = emit_primitive_activity_decision(
        builder,
        declarations,
        activity_cache,
        *activity,
        event_ports,
        sample_plan,
        event_plan,
        tick_background,
        tick_invocation,
        realtime_storage,
        continuous_input_bindings,
        event_input_bindings,
        sample_index,
        block_size);
    auto* tick_block = llvm::BasicBlock::Create(
        llvm_context, "activity.tick", function);
    auto* skip_block = llvm::BasicBlock::Create(
        llvm_context, "activity.skip", function);
    auto* merge_block = llvm::BasicBlock::Create(
        llvm_context, "activity.end", function);
    builder.CreateCondBr(decision.should_skip, skip_block, tick_block);

    builder.SetInsertPoint(skip_block);
    invoke(skip_callback);
    builder.CreateBr(merge_block);

    builder.SetInsertPoint(tick_block);
    builder.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_context), 0),
        decision.dormant_pointer);
    invoke(tick_callback);
    emit_primitive_activity_update(
        builder,
        *activity,
        decision,
        sample_ports,
        event_ports,
        sample_plan,
        sample_index,
        block_size);
    builder.CreateBr(merge_block);

    builder.SetInsertPoint(merge_block);
}

void emit_sliced_primitive_calls(
    llvm::IRBuilder<>& builder,
    llvm::Function* tick_callback,
    llvm::Function* skip_callback,
    detail::DeclarationPlan const& declarations,
    ActivityEmissionCache& activity_cache,
    detail::PrimitiveActivityPlan const* activity,
    bool force_skip,
    EmittedNodeConfiguration const& configuration,
    detail::PrimitiveStoragePlan const& storage,
    EmittedPrimitiveSamplePorts const& sample_ports,
    EmittedPrimitiveEventPorts const& event_ports,
    detail::PrimitiveSamplePortPlan const& sample_plan,
    detail::SampleStoragePlan const& sample_storage,
    detail::PrimitiveEventPortPlan const& event_plan,
    detail::EventPortBindingPlan const& event_storage,
    BackgroundEvaluationPlan const* tick_background,
    TickNodeInvocationPlan const* tick_invocation,
    llvm::Value* invocation,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    std::size_t maximum_block_size,
    std::string_view combine_sequential_input_values_symbol)
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
        tick_callback,
        skip_callback,
        declarations,
        activity_cache,
        activity,
        force_skip,
        configuration,
        storage,
        sample_ports,
        event_ports,
        sample_plan,
        sample_storage,
        event_plan,
        event_storage,
        tick_background,
        tick_invocation,
        invocation,
        realtime_storage,
        root_index,
        slice_index,
        slice_size,
        combine_sequential_input_values_symbol);

    auto* next_offset = builder.CreateAdd(
        offset, slice_size, "primitive.slice.next");
    auto* done = builder.CreateICmpUGE(
        next_offset, block_size, "primitive.slice.done");
    builder.CreateCondBr(done, exit, loop);
    offset->addIncoming(next_offset, builder.GetInsertBlock());

    builder.SetInsertPoint(exit);
}


bool supports_linear_audio_channel_conversion(
    ContinuousValueStorageGeometry const& geometry) noexcept
{
    return geometry.channel_capability == ContinuousValueChannelCapability::audio
        && geometry.element_alignment >= alignof(float)
        && geometry.channel_scalar_lane_count != 0
        && geometry.value_size_bytes
            == geometry.channel_scalar_lane_count * sizeof(float);
}

EmittedLatticeProjection emit_lattice_projection(
    llvm::IRBuilder<>& builder,
    llvm::Value* project_index,
    llvm::Value* project_count,
    std::int32_t pace_exponent,
    llvm::Twine const& name)
{
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    if (pace_exponent >= 0) {
        auto const shift = static_cast<unsigned>(pace_exponent);
        IV_ASSERT(shift < sizeof(std::size_t) * 8,
            "continuous pace cannot fit a root-block value count");
        return {
            .begin_ordinal = shift == 0
                ? project_index
                : builder.CreateShl(
                    project_index,
                    llvm::ConstantInt::get(size_type, shift),
                    name + ".begin"),
            .value_count = shift == 0
                ? project_count
                : builder.CreateShl(
                    project_count,
                    llvm::ConstantInt::get(size_type, shift),
                    name + ".count"),
        };
    }

    auto const shift = static_cast<unsigned>(-pace_exponent);
    IV_ASSERT(shift <= sizeof(std::size_t) * 8,
        "continuous pace is outside the Q64.64 lattice");
    llvm::Value* offset = nullptr;
    llvm::Value* first = nullptr;
    llvm::Value* ordinal = nullptr;
    if (shift == sizeof(std::size_t) * 8) {
        offset = builder.CreateSub(
            llvm::ConstantInt::get(size_type, 0),
            project_index,
            name + ".offset");
        first = builder.CreateAdd(project_index, offset, name + ".first");
        ordinal = llvm::ConstantInt::get(size_type, 0);
    } else {
        auto const spacing = std::size_t{1} << shift;
        auto const mask = spacing - 1;
        auto* remainder = builder.CreateAnd(
            project_index,
            llvm::ConstantInt::get(size_type, mask),
            name + ".remainder");
        offset = builder.CreateAnd(
            builder.CreateSub(
                llvm::ConstantInt::get(size_type, spacing),
                remainder,
                name + ".offset.raw"),
            llvm::ConstantInt::get(size_type, mask),
            name + ".offset");
        first = builder.CreateAdd(project_index, offset, name + ".first");
        ordinal = builder.CreateLShr(
            first,
            llvm::ConstantInt::get(size_type, shift),
            name + ".begin");
    }

    auto* has_value = builder.CreateICmpULT(
        offset, project_count, name + ".nonempty");
    auto* after_first = builder.CreateSub(
        builder.CreateSub(project_count, offset, name + ".after.offset"),
        llvm::ConstantInt::get(size_type, 1),
        name + ".after.first");
    llvm::Value* additional = shift == sizeof(std::size_t) * 8
        ? static_cast<llvm::Value*>(llvm::ConstantInt::get(size_type, 0))
        : builder.CreateLShr(
            after_first,
            llvm::ConstantInt::get(size_type, shift),
            name + ".additional");
    auto* count = builder.CreateSelect(
        has_value,
        builder.CreateAdd(
            additional,
            llvm::ConstantInt::get(size_type, 1),
            name + ".nonempty.count"),
        llvm::ConstantInt::get(size_type, 0),
        name + ".count");
    return {.begin_ordinal = ordinal, .value_count = count};
}

llvm::Value* emit_sequential_begin_ordinal(
    llvm::IRBuilder<>& builder,
    detail::SampleStoragePlan const& storage,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* callback_index,
    std::int32_t pace_exponent,
    llvm::Twine const& name)
{
    auto const cursor = std::ranges::find(
        storage.sequential_pace_cursors,
        pace_exponent,
        &detail::SequentialPaceCursorPlan::absolute_pace_exponent);
    IV_ASSERT(cursor != storage.sequential_pace_cursors.end(),
        "continuous storage has no cursor for its resolved pace");
    auto const cursor_index = static_cast<std::size_t>(
        std::distance(storage.sequential_pace_cursors.begin(), cursor));
    IV_ASSERT(cursor_index < realtime_storage.sequential_pace_cursors.size(),
        "continuous Sequential cursor has no root value");
    auto* root_ordinal = realtime_storage.sequential_pace_cursors[cursor_index];
    auto* prefix_extent = builder.CreateSub(
        callback_index, root_index, name + ".prefix.extent");
    auto const prefix = emit_lattice_projection(
        builder,
        root_index,
        prefix_extent,
        pace_exponent,
        name + ".prefix");
    return builder.CreateAdd(
        root_ordinal, prefix.value_count, name + ".begin");
}

llvm::Value* emit_unsigned_min(
    llvm::IRBuilder<>& builder,
    llvm::Value* left,
    llvm::Value* right,
    llvm::Twine const& name)
{
    return builder.CreateSelect(
        builder.CreateICmpULT(left, right, name + ".left"),
        left,
        right,
        name);
}

llvm::Value* emit_available_producer_revision(
    llvm::IRBuilder<>& builder,
    detail::SampleStoragePlan const& storage,
    EmittedRealtimeStorage const& realtime_storage,
    std::size_t source_representation,
    std::size_t requested_revision_frames,
    llvm::Value* root_index,
    llvm::Value* callback_index,
    llvm::Twine const& name)
{
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    if (requested_revision_frames == 0) {
        return llvm::ConstantInt::get(size_type, 0);
    }
    auto const extent = std::ranges::find(
        storage.producer_authored_extents,
        source_representation,
        &detail::SampleProducerAuthoredExtentPlan::source_representation);
    IV_ASSERT(extent != storage.producer_authored_extents.end(),
        "detached continuous revision has no producer authored extent");
    IV_ASSERT(requested_revision_frames <= extent->maximum_revision_frames,
        "detached continuous revision exceeds its producer authored extent");
    auto* maximum = llvm::ConstantInt::get(
        size_type, extent->maximum_revision_frames);
    auto* requested = llvm::ConstantInt::get(
        size_type, requested_revision_frames);
    auto const extent_index = static_cast<std::size_t>(
        std::distance(storage.producer_authored_extents.begin(), extent));
    IV_ASSERT(extent_index < realtime_storage.producer_authored_extents.size(),
        "detached continuous revision has no root extent value");
    auto* prior = emit_unsigned_min(
        builder,
        realtime_storage.producer_authored_extents[extent_index],
        maximum,
        name + ".prior");
    auto* prefix_extent = builder.CreateSub(
        callback_index, root_index, name + ".prefix.extent");
    auto const prefix = emit_lattice_projection(
        builder,
        root_index,
        prefix_extent,
        extent->absolute_pace_exponent,
        name + ".prefix");
    auto* remaining = builder.CreateSub(
        maximum, prior, name + ".remaining");
    auto* prefix_increment = emit_unsigned_min(
        builder,
        prefix.value_count,
        remaining,
        name + ".prefix.increment");
    auto* available = builder.CreateAdd(
        prior, prefix_increment, name + ".available");
    return emit_unsigned_min(
        builder, available, requested, name + ".requested");
}

llvm::Value* continuous_value_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* representation_base,
    ReflectedContinuousPortStorageBinding const& binding,
    llvm::Value* absolute_value,
    std::size_t channel,
    llvm::Twine const& name)
{
    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto const channels = channel_count(binding.channel_layout);
    auto* value = builder.CreateAnd(
        absolute_value,
        llvm::ConstantInt::get(size_type, binding.value_capacity - 1),
        name + ".value");
    llvm::Value* value_offset = nullptr;
    if (binding.channel_layout.sample_layout == SampleStreamLayout::planar) {
        value_offset = builder.CreateAdd(
            value,
            llvm::ConstantInt::get(
                size_type, channel * binding.value_capacity),
            name + ".value.offset");
    } else {
        value_offset = builder.CreateAdd(
            builder.CreateMul(
                value,
                llvm::ConstantInt::get(size_type, channels),
                name + ".value.base"),
            llvm::ConstantInt::get(size_type, channel),
            name + ".value.offset");
    }
    auto* byte_offset = builder.CreateMul(
        value_offset,
        llvm::ConstantInt::get(size_type, binding.geometry.value_size_bytes),
        name + ".byte.offset");
    return builder.CreateInBoundsGEP(
        llvm::Type::getInt8Ty(context),
        representation_base,
        byte_offset,
        name + ".value.ptr");
}

llvm::Value* continuous_element_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* representation_base,
    ReflectedContinuousPortStorageBinding const& binding,
    llvm::Value* absolute_value,
    std::size_t channel,
    std::size_t element_index,
    llvm::Twine const& name)
{
    auto* value_pointer = continuous_value_pointer(
        builder,
        representation_base,
        binding,
        absolute_value,
        channel,
        name);
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    return builder.CreateInBoundsGEP(
        llvm::Type::getFloatTy(builder.getContext()),
        value_pointer,
        llvm::ConstantInt::get(size_type, element_index),
        name + ".element.ptr");
}

std::expected<std::vector<llvm::Value*>, std::string>
emit_channel_conversion(
    llvm::IRBuilder<>& builder,
    ChannelTypeId source_type,
    ChannelTypeId target_type,
    detail::SampleChannelTransformPlan const* transform,
    std::span<llvm::Value* const> source_values,
    std::string_view name)
{
    auto const source_count = transform && !transform->empty()
        ? transform->source_channel_count
        : channel_count(source_type);
    auto const target_count = transform && !transform->empty()
        ? transform->target_channel_count
        : channel_count(target_type);
    if (!is_registered_channel_type(source_type)
        || !is_registered_channel_type(target_type)
        || source_values.size() != source_count
        || (transform && !transform->empty()
            && transform->coefficients.size() != source_count * target_count)) {
        return std::unexpected(
            "GraphJit channel conversion has an invalid registered type or source value count");
    }

    auto* sample_type = llvm::Type::getFloatTy(builder.getContext());
    std::vector<llvm::Value*> result;
    result.reserve(target_count);
    for (std::size_t target_channel = 0;
         target_channel < target_count;
         ++target_channel) {
        llvm::Value* value = nullptr;
        for (std::size_t source_channel = 0;
             source_channel < source_values.size();
             ++source_channel) {
            auto const coefficient = transform && !transform->empty()
                ? transform->coefficients[
                    target_channel * source_count + source_channel]
                : channel_conversion_coefficient(
                    source_type, target_type, source_channel, target_channel);
            if (coefficient == 0.0f) continue;
            llvm::Value* term = source_values[source_channel];
            if (coefficient != 1.0f) {
                term = builder.CreateFMul(
                    term,
                    llvm::ConstantFP::get(sample_type, coefficient),
                    std::string(name) + ".scale."
                        + std::to_string(target_channel) + "."
                        + std::to_string(source_channel));
            }
            if (value) {
                value = builder.CreateFAdd(
                    value,
                    term,
                    std::string(name) + ".sum."
                        + std::to_string(target_channel) + "."
                        + std::to_string(source_channel));
            } else {
                value = term;
            }
        }
        if (!value) value = llvm::ConstantFP::get(sample_type, 0.0);
        result.push_back(value);
    }
    return result;
}

std::expected<void, std::string> emit_sample_materialization(
    llvm::IRBuilder<>& builder,
    detail::SampleStoragePlan const& storage,
    detail::SampleMaterializationPlan const& materialization,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    if (materialization.source_representation >= storage.representations.size()
        || materialization.target_representation >= storage.representations.size()) {
        return std::unexpected(
            "GraphJit sample materialization references a missing storage");
    }
    auto const& source_representation =
        storage.representations[materialization.source_representation];
    auto const& target_representation =
        storage.representations[materialization.target_representation];
    if (source_representation.channel_layout != materialization.source_layout
        || target_representation.channel_layout != materialization.target_layout
        || source_representation.geometry != target_representation.geometry
        || !supports_linear_audio_channel_conversion(
            source_representation.geometry)) {
        return std::unexpected(
            "GraphJit continuous materialization has incompatible layout or value geometry");
    }
    auto const projection = emit_lattice_projection(
        builder,
        sample_index,
        block_size,
        target_representation.absolute_pace_exponent,
        "continuous.materialize.lattice");
    sample_index = emit_sequential_begin_ordinal(
        builder,
        storage,
        realtime_storage,
        root_index,
        sample_index,
        target_representation.absolute_pace_exponent,
        "continuous.materialize.sequential");
    block_size = projection.value_count;
    auto source = sample_storage_binding(
        storage, materialization.source_representation);
    if (!source) return std::unexpected(std::move(source.error()));
    auto target = sample_storage_binding(
        storage, materialization.target_representation);
    if (!target) return std::unexpected(std::move(target.error()));
    if (materialization.source_representation
            >= realtime_storage.sample_representations.size()
        || materialization.target_representation
            >= realtime_storage.sample_representations.size()) {
        return std::unexpected(
            "GraphJit sample materialization lost resolved storage");
    }
    auto* source_base = realtime_storage.sample_representations[
        materialization.source_representation];
    auto* target_base = realtime_storage.sample_representations[
        materialization.target_representation];
    if (source->value_capacity == 0 || target->value_capacity == 0
        || !is_power_of_2(source->value_capacity)
        || !is_power_of_2(target->value_capacity)) {
        return std::unexpected(
            "GraphJit sample materialization requires bounded power-of-two frame capacities");
    }

    // Validate against the semantic conversion registry, then emit the tiny
    // mono/stereo conversion directly so no host function pointer or runtime
    // converter object enters the audio-thread ABI.
    try {
        (void)ChannelConversionRegistry::plan(
            materialization.source_layout, materialization.target_layout);
    } catch (std::exception const& e) {
        return std::unexpected(
            "GraphJit sample materialization conversion is unsupported: "
            + std::string(e.what()));
    }

    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* sample_type = llvm::Type::getFloatTy(context);
    auto* zero = llvm::ConstantInt::get(size_type, 0);
    auto* preheader = builder.GetInsertBlock();
    auto* loop = llvm::BasicBlock::Create(
        context, "sample.materialize", function);
    auto* exit = llvm::BasicBlock::Create(
        context, "sample.materialize.end", function);
    builder.CreateCondBr(
        builder.CreateICmpNE(block_size, zero, "sample.materialize.nonempty"),
        loop,
        exit);

    builder.SetInsertPoint(loop);
    auto* frame_offset = builder.CreatePHI(
        size_type, 2, "sample.materialize.frame");
    frame_offset->addIncoming(zero, preheader);
    if (materialization.latest_read_latency > materialization.retained_before) {
        return std::unexpected(
            "GraphJit sample materialization has an invalid consumer window");
    }
    auto* first_frame = builder.CreateSub(
        sample_index,
        llvm::ConstantInt::get(size_type, materialization.retained_before),
        "sample.materialize.first");
    auto* absolute_frame = builder.CreateAdd(
        first_frame, frame_offset, "sample.materialize.absolute");

    auto load_source = [&](std::size_t channel,
                           std::size_t element,
                           llvm::Twine const& name) {
        return builder.CreateLoad(
            sample_type,
            continuous_element_pointer(
                builder,
                source_base,
                *source,
                absolute_frame,
                channel,
                element,
                name),
            name + ".value");
    };
    auto store_target = [&](std::size_t channel,
                            std::size_t element,
                            llvm::Value* value,
                            llvm::Twine const& name) {
        builder.CreateStore(
            value,
            continuous_element_pointer(
                builder,
                target_base,
                *target,
                absolute_frame,
                channel,
                element,
                name));
    };

    for (std::size_t element = 0;
         element < source_representation.geometry.channel_scalar_lane_count;
         ++element) {
        std::vector<llvm::Value*> source_values;
        source_values.reserve(channel_count(materialization.source_layout));
        for (std::size_t channel = 0;
             channel < channel_count(materialization.source_layout);
             ++channel) {
            source_values.push_back(load_source(
                channel,
                element,
                "continuous.materialize.source." + std::to_string(channel)
                    + "." + std::to_string(element)));
        }
        auto converted = emit_channel_conversion(
            builder,
            materialization.source_layout.channel_type,
            materialization.target_layout.channel_type,
            materialization.transform.empty()
                ? nullptr
                : &materialization.transform,
            source_values,
            "continuous.materialize.convert." + std::to_string(element));
        if (!converted) return std::unexpected(std::move(converted.error()));
        for (std::size_t channel = 0; channel < converted->size(); ++channel) {
            store_target(
                channel,
                element,
                (*converted)[channel],
                "continuous.materialize.target." + std::to_string(channel)
                    + "." + std::to_string(element));
        }
    }

    auto* next = builder.CreateAdd(
        frame_offset,
        llvm::ConstantInt::get(size_type, 1),
        "sample.materialize.next");
    auto const materialized_prefix = materialization.retained_before
        - materialization.latest_read_latency;
    auto* materialize_count = builder.CreateAdd(
        block_size,
        llvm::ConstantInt::get(size_type, materialized_prefix),
        "sample.materialize.count");
    auto* done = builder.CreateICmpUGE(
        next, materialize_count, "sample.materialize.done");
    builder.CreateCondBr(done, exit, loop);
    frame_offset->addIncoming(next, loop);
    builder.SetInsertPoint(exit);
    return {};
}


std::expected<void, std::string> emit_sample_composition_write(
    llvm::IRBuilder<>& builder,
    detail::SampleStoragePlan const& storage,
    std::vector<detail::SampleCompositionContributionPlan> const& contribution_plans,
    std::size_t target_representation_index,
    ChannelLayout target_layout,
    std::size_t target_history,
    bool shift_writes_by_read_latency,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    if (target_representation_index >= storage.representations.size()) {
        return std::unexpected(
            "GraphJit sample composition references a missing target storage");
    }
    auto const& target_representation =
        storage.representations[target_representation_index];
    if (target_representation.channel_layout != target_layout
        || !supports_linear_audio_channel_conversion(
            target_representation.geometry)) {
        return std::unexpected(
            "GraphJit continuous composition target has incompatible layout or value geometry");
    }
    auto const projection = emit_lattice_projection(
        builder,
        sample_index,
        block_size,
        target_representation.absolute_pace_exponent,
        "continuous.compose.lattice");
    auto* callback_index = sample_index;
    sample_index = emit_sequential_begin_ordinal(
        builder,
        storage,
        realtime_storage,
        root_index,
        sample_index,
        target_representation.absolute_pace_exponent,
        "continuous.compose.sequential");
    block_size = projection.value_count;
    if (contribution_plans.empty()) {
        return std::unexpected(
            "GraphJit sample composition contains no semantic contributions");
    }
    if (shift_writes_by_read_latency
        && (target_representation.storage
                == RealtimeBufferStorageKind::transient_stack
            || target_representation.persistent_allocation
                == detail::no_sample_persistent_allocation)) {
        return std::unexpected(
            "GraphJit shifted sample composition requires retained feedback storage");
    }

    auto target = sample_storage_binding(
        storage, target_representation_index);
    if (!target) return std::unexpected(std::move(target.error()));
    if (target_representation_index
        >= realtime_storage.sample_representations.size()) {
        return std::unexpected(
            "GraphJit sample composition lost resolved target storage");
    }
    auto* target_base = realtime_storage.sample_representations[
        target_representation_index];
    if (target->value_capacity == 0
        || !is_power_of_2(target->value_capacity)) {
        return std::unexpected(
            "GraphJit sample composition requires bounded power-of-two target storage");
    }

    struct SourceBinding {
        ReflectedContinuousPortStorageBinding storage{};
        llvm::Value* base = nullptr;
        std::size_t source_representation = detail::no_sample_representation;
        std::size_t source_channel = 0;
        std::size_t read_latency = 0;
    };
    struct ContributionBinding {
        ChannelLayout source_layout{};
        ChannelLayout converted_layout{};
        detail::SampleChannelTransformPlan transform{};
        std::vector<SourceBinding> sources{};
        std::vector<std::size_t> target_channels{};
        std::vector<std::size_t> shifted_write_latencies{};
        std::optional<ReflectedContinuousPortStorageBinding> feedback_alignment_storage{};
        llvm::Value* feedback_alignment_base = nullptr;
        std::size_t feedback_alignment_write_latency = 0;
    };

    std::vector<ContributionBinding> contributions;
    contributions.reserve(contribution_plans.size());
    std::vector<bool> populated_targets(channel_count(target_layout), false);
    for (auto const& contribution_plan : contribution_plans) {
        auto const source_channel_count = contribution_plan.transform.empty()
            ? channel_count(contribution_plan.source_layout)
            : contribution_plan.transform.source_channel_count;
        auto const target_channel_count = contribution_plan.transform.empty()
            ? channel_count(contribution_plan.converted_layout)
            : contribution_plan.transform.target_channel_count;
        if (contribution_plan.sources.size() != source_channel_count
            || contribution_plan.target_channels.size() != target_channel_count
            || (!contribution_plan.transform.empty()
                && contribution_plan.transform.coefficients.size()
                    != source_channel_count * target_channel_count)) {
            return std::unexpected(
                "GraphJit sample composition contribution has inconsistent semantic channel counts");
        }
        if (contribution_plan.transform.empty()) {
            try {
                (void)ChannelConversionRegistry::plan(
                    contribution_plan.source_layout,
                    contribution_plan.converted_layout);
            } catch (std::exception const& e) {
                return std::unexpected(
                    "GraphJit sample composition conversion is unsupported: "
                    + std::string(e.what()));
            }
        }

        ContributionBinding contribution{
            .source_layout = contribution_plan.source_layout,
            .converted_layout = contribution_plan.converted_layout,
            .transform = contribution_plan.transform,
            .target_channels = contribution_plan.target_channels,
        };
        contribution.sources.reserve(contribution_plan.sources.size());
        for (auto const& source_plan : contribution_plan.sources) {
            if (source_plan.source_representation
                >= storage.representations.size()) {
                return std::unexpected(
                    "GraphJit sample composition references a missing source storage");
            }
            auto source = sample_storage_binding(
                storage, source_plan.source_representation);
            if (!source) return std::unexpected(std::move(source.error()));
            if (source->value_capacity == 0
                || !is_power_of_2(source->value_capacity)
                || source->geometry != target->geometry
                || source_plan.source_channel
                    >= channel_count(source->channel_layout)) {
                return std::unexpected(
                    "GraphJit sample composition contains an invalid source channel");
            }
            contribution.sources.push_back(SourceBinding{
                .storage = *source,
                .base = realtime_storage.sample_representations[
                    source_plan.source_representation],
                .source_representation = source_plan.source_representation,
                .source_channel = source_plan.source_channel,
                .read_latency = source_plan.read_latency,
            });
        }
        for (auto const target_channel : contribution.target_channels) {
            if (target_channel >= populated_targets.size()
                || populated_targets[target_channel]) {
                return std::unexpected(
                    "GraphJit sample composition contains an invalid target projection");
            }
            populated_targets[target_channel] = true;
        }

        if (!shift_writes_by_read_latency
            && (contribution_plan.feedback_alignment_representation
                    != detail::no_sample_representation
                || contribution_plan.feedback_alignment_write_latency != 0)) {
            return std::unexpected(
                "GraphJit feed-forward sample composition unexpectedly owns feedback alignment storage");
        }
        if (shift_writes_by_read_latency) {
            auto const source_type = contribution.source_layout.channel_type;
            auto const converted_type = contribution.converted_layout.channel_type;
            contribution.shifted_write_latencies.resize(
                target_channel_count);
            auto const [minimum_source, maximum_source] =
                std::ranges::minmax_element(
                    contribution.sources,
                    {},
                    &SourceBinding::read_latency);
            auto const minimum_latency = minimum_source->read_latency;
            auto const maximum_latency = maximum_source->read_latency;
            auto coefficient = [&](std::size_t source_channel,
                                   std::size_t target_channel) {
                return contribution_plan.transform.empty()
                    ? channel_conversion_coefficient(
                        source_type,
                        converted_type,
                        source_channel,
                        target_channel)
                    : contribution_plan.transform.coefficients[
                        target_channel * source_channel_count + source_channel];
            };
            auto const transform_mixes_sources = [&] {
                for (std::size_t target_channel = 0;
                     target_channel < target_channel_count;
                     ++target_channel) {
                    std::size_t contributors = 0;
                    for (std::size_t source_channel = 0;
                         source_channel < source_channel_count;
                         ++source_channel) {
                        contributors += coefficient(source_channel, target_channel)
                            != 0.0f;
                    }
                    if (contributors > 1) return true;
                }
                return false;
            }();
            auto const needs_alignment = transform_mixes_sources
                && minimum_latency != maximum_latency;

            if (needs_alignment) {
                if (contribution_plan.feedback_alignment_representation
                        >= storage.representations.size()
                    || contribution_plan.feedback_alignment_write_latency
                        != minimum_latency) {
                    return std::unexpected(
                        "GraphJit unequal-latency feedback mixing lost its alignment plan");
                }
                auto alignment = sample_storage_binding(
                    storage,
                    contribution_plan.feedback_alignment_representation);
                if (!alignment) {
                    return std::unexpected(std::move(alignment.error()));
                }
                if (alignment->channel_layout != contribution.source_layout
                    || alignment->geometry != target->geometry
                    || alignment->value_capacity == 0
                    || !is_power_of_2(alignment->value_capacity)) {
                    return std::unexpected(
                        "GraphJit unequal-latency feedback mixing has invalid alignment storage");
                }
                contribution.feedback_alignment_storage = *alignment;
                contribution.feedback_alignment_base =
                    realtime_storage.sample_representations[
                        contribution_plan.feedback_alignment_representation];
                contribution.feedback_alignment_write_latency = minimum_latency;
                std::ranges::fill(
                    contribution.shifted_write_latencies, minimum_latency);
            } else {
                if (contribution_plan.feedback_alignment_representation
                    != detail::no_sample_representation) {
                    return std::unexpected(
                        "GraphJit feedback conversion unexpectedly owns alignment storage");
                }
                for (std::size_t target_channel = 0;
                     target_channel < contribution.shifted_write_latencies.size();
                     ++target_channel) {
                    std::optional<std::size_t> latency;
                    for (std::size_t source_channel = 0;
                         source_channel < contribution.sources.size();
                         ++source_channel) {
                        if (coefficient(source_channel, target_channel) == 0.0f) {
                            continue;
                        }
                        auto const candidate =
                            contribution.sources[source_channel].read_latency;
                        if (latency && *latency != candidate) {
                            return std::unexpected(
                                "GraphJit feedback conversion mixes unaligned source latencies");
                        }
                        latency = candidate;
                    }
                    if (!latency) {
                        return std::unexpected(
                            "GraphJit feedback conversion has a target channel with no source contribution");
                    }
                    contribution.shifted_write_latencies[target_channel] = *latency;
                }
            }
        }
        contributions.push_back(std::move(contribution));
    }
    if (!std::ranges::all_of(
            populated_targets, [](bool value) { return value; })) {
        return std::unexpected(
            "GraphJit sample composition does not populate every target channel");
    }

    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* sample_type = llvm::Type::getFloatTy(context);
    auto* zero = llvm::ConstantInt::get(size_type, 0);
    llvm::Value* history = llvm::ConstantInt::get(size_type, target_history);
    std::vector<llvm::Value*> contribution_histories;
    contribution_histories.reserve(contributions.size());
    for (std::size_t contribution_index = 0;
         contribution_index < contributions.size(); ++contribution_index) {
        llvm::Value* available = history;
        if (shift_writes_by_read_latency && target_history != 0) {
            for (std::size_t source_index = 0;
                 source_index < contributions[contribution_index].sources.size();
                 ++source_index) {
                auto const& source =
                    contributions[contribution_index].sources[source_index];
                auto* source_available = emit_available_producer_revision(
                    builder,
                    storage,
                    realtime_storage,
                    source.source_representation,
                    target_history,
                    root_index,
                    callback_index,
                    "sample.compose.revision."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index));
                available = emit_unsigned_min(
                    builder,
                    available,
                    source_available,
                    "sample.compose.available."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index));
            }
        }
        contribution_histories.push_back(available);
    }
    auto* compose_count = builder.CreateAdd(
        block_size, history, "sample.compose.count");
    auto emit_loop = [&](
                         llvm::BasicBlock* preheader,
                         llvm::BasicBlock* exit)
        -> std::expected<void, std::string> {
        builder.SetInsertPoint(preheader);
        auto* has_frames = builder.CreateICmpNE(
            compose_count, zero, "sample.compose.nonempty");
        auto* loop = llvm::BasicBlock::Create(
            context, "sample.compose.loop", function);
        builder.CreateCondBr(has_frames, loop, exit);

        builder.SetInsertPoint(loop);
    auto* frame_offset = builder.CreatePHI(
        size_type, 2, "sample.compose.frame");
    frame_offset->addIncoming(zero, preheader);
    auto* first_frame = builder.CreateSub(
        sample_index, history, "sample.compose.first");
    auto* target_frame = builder.CreateAdd(
        first_frame, frame_offset, "sample.compose.absolute");

    for (std::size_t contribution_index = 0;
         contribution_index < contributions.size(); ++contribution_index) {
        auto const& contribution = contributions[contribution_index];
        auto* first_authored_offset = builder.CreateSub(
            history,
            contribution_histories[contribution_index],
            "sample.compose.first.authored."
                + std::to_string(contribution_index));
        auto* contribution_is_authored = builder.CreateICmpUGE(
            frame_offset,
            first_authored_offset,
            "sample.compose.authored."
                + std::to_string(contribution_index));
        for (std::size_t element = 0;
             element < target->geometry.channel_scalar_lane_count;
             ++element) {
            std::vector<llvm::Value*> source_values;
            source_values.reserve(contribution.sources.size());
            for (std::size_t source_index = 0;
                 source_index < contribution.sources.size(); ++source_index) {
                auto const& source = contribution.sources[source_index];
                llvm::Value* source_frame = target_frame;
                if (!shift_writes_by_read_latency) {
                    source_frame = builder.CreateSub(
                        target_frame,
                        llvm::ConstantInt::get(size_type, source.read_latency),
                        "continuous.compose.source.value."
                            + std::to_string(contribution_index) + "."
                            + std::to_string(source_index));
                }
                auto* source_pointer = continuous_element_pointer(
                    builder,
                    source.base,
                    source.storage,
                    source_frame,
                    source.source_channel,
                    element,
                    "continuous.compose.source."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index) + "."
                        + std::to_string(element));
                source_values.push_back(builder.CreateLoad(
                    sample_type,
                    source_pointer,
                    "continuous.compose.value."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index) + "."
                        + std::to_string(element)));
            }

            if (contribution.feedback_alignment_storage) {
                for (std::size_t source_index = 0;
                     source_index < contribution.sources.size(); ++source_index) {
                    auto* alignment_pointer = continuous_element_pointer(
                        builder,
                        contribution.feedback_alignment_base,
                        *contribution.feedback_alignment_storage,
                        target_frame,
                        source_index,
                        element,
                        "continuous.compose.align.write."
                            + std::to_string(contribution_index) + "."
                            + std::to_string(source_index) + "."
                            + std::to_string(element));
                    auto* retained_alignment_value = builder.CreateLoad(
                        sample_type,
                        alignment_pointer,
                        "continuous.compose.align.retained."
                            + std::to_string(contribution_index) + "."
                            + std::to_string(source_index) + "."
                            + std::to_string(element));
                    builder.CreateStore(
                        builder.CreateSelect(
                            contribution_is_authored,
                            source_values[source_index],
                            retained_alignment_value,
                            "continuous.compose.align.selected."
                                + std::to_string(contribution_index) + "."
                                + std::to_string(source_index) + "."
                                + std::to_string(element)),
                        alignment_pointer);
                }

                for (std::size_t source_index = 0;
                     source_index < contribution.sources.size(); ++source_index) {
                    auto const latency_delta =
                        contribution.sources[source_index].read_latency
                        - contribution.feedback_alignment_write_latency;
                    auto* aligned_frame = builder.CreateSub(
                        target_frame,
                        llvm::ConstantInt::get(size_type, latency_delta),
                        "continuous.compose.align.value."
                            + std::to_string(contribution_index) + "."
                            + std::to_string(source_index));
                    auto* alignment_pointer = continuous_element_pointer(
                        builder,
                        contribution.feedback_alignment_base,
                        *contribution.feedback_alignment_storage,
                        aligned_frame,
                        source_index,
                        element,
                        "continuous.compose.align.read."
                            + std::to_string(contribution_index) + "."
                            + std::to_string(source_index) + "."
                            + std::to_string(element));
                    source_values[source_index] = builder.CreateLoad(
                        sample_type,
                        alignment_pointer,
                        "continuous.compose.align.value."
                            + std::to_string(contribution_index) + "."
                            + std::to_string(source_index) + "."
                            + std::to_string(element));
                }
            }

            auto converted = emit_channel_conversion(
                builder,
                contribution.source_layout.channel_type,
                contribution.converted_layout.channel_type,
                contribution.transform.empty()
                    ? nullptr
                    : &contribution.transform,
                source_values,
                "continuous.compose.convert."
                    + std::to_string(contribution_index) + "."
                    + std::to_string(element));
            if (!converted) return std::unexpected(std::move(converted.error()));
            auto& converted_values = *converted;

            if (converted_values.size() != contribution.target_channels.size()) {
                return std::unexpected(
                    "GraphJit continuous composition conversion produced an invalid channel count");
            }
            for (std::size_t converted_channel = 0;
                 converted_channel < converted_values.size(); ++converted_channel) {
                llvm::Value* target_storage_frame = target_frame;
                if (shift_writes_by_read_latency) {
                    target_storage_frame = builder.CreateAdd(
                        target_frame,
                        llvm::ConstantInt::get(
                            size_type,
                            contribution.shifted_write_latencies[converted_channel]),
                        "continuous.compose.target.value."
                            + std::to_string(contribution_index) + "."
                            + std::to_string(converted_channel));
                }
                auto* target_pointer = continuous_element_pointer(
                    builder,
                    target_base,
                    *target,
                    target_storage_frame,
                    contribution.target_channels[converted_channel],
                    element,
                    "continuous.compose.target."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(converted_channel) + "."
                        + std::to_string(element));
                if (shift_writes_by_read_latency) {
                    auto* retained_target_value = builder.CreateLoad(
                        sample_type,
                        target_pointer,
                        "continuous.compose.target.retained."
                            + std::to_string(contribution_index) + "."
                            + std::to_string(converted_channel) + "."
                            + std::to_string(element));
                    builder.CreateStore(
                        builder.CreateSelect(
                            contribution_is_authored,
                            converted_values[converted_channel],
                            retained_target_value,
                            "continuous.compose.target.selected."
                                + std::to_string(contribution_index) + "."
                                + std::to_string(converted_channel) + "."
                                + std::to_string(element)),
                        target_pointer);
                } else {
                    builder.CreateStore(
                        converted_values[converted_channel], target_pointer);
                }
            }
        }
    }

    auto* next = builder.CreateAdd(
        frame_offset,
        llvm::ConstantInt::get(size_type, 1),
        "sample.compose.next");
    auto* done = builder.CreateICmpUGE(
        next, compose_count, "sample.compose.done");
    builder.CreateCondBr(done, exit, loop);
    frame_offset->addIncoming(next, loop);
        return {};
    };

    auto* composition_exit = llvm::BasicBlock::Create(
        context, "sample.compose.end", function);
    auto* preheader = builder.GetInsertBlock();
    auto emitted = emit_loop(preheader, composition_exit);
    if (!emitted) return std::unexpected(std::move(emitted.error()));
    builder.SetInsertPoint(composition_exit);
    return {};
}

std::expected<void, std::string> emit_sample_composition(
    llvm::IRBuilder<>& builder,
    detail::SampleStoragePlan const& storage,
    detail::SampleCompositionPlan const& composition,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    return emit_sample_composition_write(
        builder,
        storage,
        composition.contributions,
        composition.target_representation,
        composition.target_layout,
        composition.target_history,
        false,
        realtime_storage,
        root_index,
        sample_index,
        block_size);
}

std::expected<void, std::string> emit_sample_feedback_timeline_write(
    llvm::IRBuilder<>& builder,
    detail::SampleStoragePlan const& storage,
    detail::SampleFeedbackTimelinePlan const& timeline,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    if (timeline.timeline_representation >= storage.representations.size()) {
        return std::unexpected(
            "GraphJit sample feedback timeline references a missing storage");
    }
    auto const& timeline_plan =
        storage.representations[timeline.timeline_representation];
    if (timeline_plan.storage == RealtimeBufferStorageKind::transient_stack
        || timeline_plan.persistent_allocation
            == detail::no_sample_persistent_allocation
        || timeline_plan.channel_layout != timeline.channel_layout
        || timeline.loop_extra_latency == 0
        || timeline.loop_extra_latency >= timeline_plan.frame_capacity) {
        return std::unexpected(
            "GraphJit sample feedback timeline is inconsistent with its retained storage");
    }
    if (timeline_plan.persistent_allocation
        >= storage.persistent_allocations.size()) {
        return std::unexpected(
            "GraphJit sample feedback timeline references a missing persistent allocation");
    }
    auto const& allocation =
        storage.persistent_allocations[timeline_plan.persistent_allocation];
    auto const expected_persistent_kind = timeline_plan.storage
            == RealtimeBufferStorageKind::stack_with_persistent_carry
        ? detail::SamplePersistentStorageKind::compact_carry
        : detail::SamplePersistentStorageKind::ring;
    if (allocation.kind != expected_persistent_kind
        || allocation.representation_index != timeline.timeline_representation
        || allocation.retained_frames < timeline.retained_frames
        || !allocation.initialize_value
        || *allocation.initialize_value != timeline.initial_value
        || allocation.geometry != timeline_plan.geometry) {
        return std::unexpected(
            "GraphJit sample feedback timeline lost its persistent initialization semantics");
    }

    switch (timeline.writer.kind) {
    case detail::SampleFeedbackTimelineWriterKind::producer_home:
        return std::unexpected(
            "GraphJit producer-home feedback timeline must not schedule a post-producer write");
    case detail::SampleFeedbackTimelineWriterKind::composition:
        if (timeline.writer.source_representation != detail::no_sample_representation
            || timeline.writer.composition_contributions.empty()) {
            return std::unexpected(
                "GraphJit composed sample feedback timeline has an invalid writer shape");
        }
        return emit_sample_composition_write(
            builder,
            storage,
            timeline.writer.composition_contributions,
            timeline.timeline_representation,
            timeline.channel_layout,
            timeline.writer.revision_frames,
            true,
            realtime_storage,
            root_index,
            sample_index,
            block_size);
    case detail::SampleFeedbackTimelineWriterKind::copy:
        break;
    }

    auto const projection = emit_lattice_projection(
        builder,
        sample_index,
        block_size,
        timeline_plan.absolute_pace_exponent,
        "continuous.feedback.lattice");
    auto* callback_index = sample_index;
    sample_index = emit_sequential_begin_ordinal(
        builder,
        storage,
        realtime_storage,
        root_index,
        sample_index,
        timeline_plan.absolute_pace_exponent,
        "continuous.feedback.sequential");
    block_size = projection.value_count;

    if (!timeline.writer.composition_contributions.empty()
        || timeline.writer.source_representation >= storage.representations.size()) {
        return std::unexpected(
            "GraphJit copied sample feedback timeline has an invalid writer shape");
    }
    auto const& source_plan =
        storage.representations[timeline.writer.source_representation];
    if (timeline_plan.channel_layout != source_plan.channel_layout
        || timeline_plan.geometry != source_plan.geometry) {
        return std::unexpected(
            "GraphJit sample feedback copy changed channel layout without composition");
    }

    auto source = sample_storage_binding(
        storage, timeline.writer.source_representation);
    if (!source) return std::unexpected(std::move(source.error()));
    auto ring = sample_storage_binding(
        storage, timeline.timeline_representation);
    if (!ring) return std::unexpected(std::move(ring.error()));
    if (timeline.writer.source_representation
            >= realtime_storage.sample_representations.size()
        || timeline.timeline_representation
            >= realtime_storage.sample_representations.size()) {
        return std::unexpected(
            "GraphJit sample feedback copy lost resolved storage");
    }
    auto* source_base = realtime_storage.sample_representations[
        timeline.writer.source_representation];
    auto* ring_base = realtime_storage.sample_representations[
        timeline.timeline_representation];

    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* zero = llvm::ConstantInt::get(size_type, 0);
    auto* revision_frames = emit_available_producer_revision(
        builder,
        storage,
        realtime_storage,
        timeline.writer.source_representation,
        timeline.writer.revision_frames,
        root_index,
        callback_index,
        "sample.feedback.copy.revision");
    auto* copy_count = builder.CreateAdd(
        block_size, revision_frames, "sample.feedback.copy.count");
    auto* first_frame = builder.CreateSub(
        sample_index, revision_frames, "sample.feedback.copy.first");
    auto* preheader = builder.GetInsertBlock();
    auto* loop = llvm::BasicBlock::Create(
        context, "sample.feedback.copy", function);
    auto* exit = llvm::BasicBlock::Create(
        context, "sample.feedback.copy.end", function);
    builder.CreateCondBr(
        builder.CreateICmpNE(
            copy_count, zero, "sample.feedback.copy.nonempty"),
        loop,
        exit);

    builder.SetInsertPoint(loop);
    auto* frame_offset = builder.CreatePHI(
        size_type, 2, "sample.feedback.copy.frame");
    frame_offset->addIncoming(zero, preheader);
    auto* absolute_frame = builder.CreateAdd(
        first_frame, frame_offset, "sample.feedback.copy.absolute");

    auto const channels = channel_count(source_plan.channel_layout);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        auto* source_pointer = continuous_value_pointer(
            builder,
            source_base,
            *source,
            absolute_frame,
            channel,
            "sample.feedback.source." + std::to_string(channel));
        auto* ring_pointer = continuous_value_pointer(
            builder,
            ring_base,
            *ring,
            absolute_frame,
            channel,
            "sample.feedback.ring." + std::to_string(channel));
        builder.CreateMemCpy(
            ring_pointer,
            llvm::Align(timeline_plan.geometry.element_alignment),
            source_pointer,
            llvm::Align(source_plan.geometry.element_alignment),
            timeline_plan.geometry.value_size_bytes);
    }

    auto* next = builder.CreateAdd(
        frame_offset,
        llvm::ConstantInt::get(size_type, 1),
        "sample.feedback.copy.next");
    auto* done = builder.CreateICmpUGE(
        next, copy_count, "sample.feedback.copy.done");
    builder.CreateCondBr(done, exit, loop);
    frame_offset->addIncoming(next, loop);
    builder.SetInsertPoint(exit);
    return {};
}

std::expected<detail::SamplePersistentAllocationPlan const*, std::string>
compact_carry_allocation(
    detail::SampleStoragePlan const& storage,
    detail::SampleCarryOperationPlan const& operation)
{
    if (operation.representation_index >= storage.representations.size()) {
        return std::unexpected(
            "GraphJit sample carry references a missing storage");
    }
    if (operation.persistent_allocation >= storage.persistent_allocations.size()) {
        return std::unexpected(
            "GraphJit sample carry references a missing persistent allocation");
    }
    auto const& representation =
        storage.representations[operation.representation_index];
    auto const& allocation =
        storage.persistent_allocations[operation.persistent_allocation];
    if (allocation.kind != detail::SamplePersistentStorageKind::compact_carry
        || allocation.representation_index != operation.representation_index
        || representation.persistent_allocation != operation.persistent_allocation
        || representation.transient_allocation
               == detail::no_sample_transient_allocation
        || allocation.retained_frames != operation.retained_frames
        || operation.retained_frames == 0
        || operation.future_frames > operation.retained_frames) {
        return std::unexpected(
            "GraphJit sample carry plan is inconsistent with its storage");
    }
    return &allocation;
}

llvm::Value* compact_carry_value_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* storage_base,
    detail::SamplePersistentAllocationPlan const& allocation,
    llvm::Value* carry_frame,
    std::size_t channel,
    llvm::Twine const& name)
{
    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto const channels = channel_count(allocation.channel_layout);
    llvm::Value* value = nullptr;
    if (allocation.channel_layout.sample_layout == SampleStreamLayout::planar) {
        value = builder.CreateAdd(
            carry_frame,
            llvm::ConstantInt::get(
                size_type, channel * allocation.retained_frames),
            name + ".value");
    } else {
        value = builder.CreateAdd(
            builder.CreateMul(
                carry_frame,
                llvm::ConstantInt::get(size_type, channels),
                name + ".frame.base"),
            llvm::ConstantInt::get(size_type, channel),
            name + ".value");
    }
    auto* base = byte_offset_pointer(
        builder, storage_base, allocation.storage_offset, name + ".base");
    auto* byte_offset = builder.CreateMul(
        value,
        llvm::ConstantInt::get(size_type, allocation.geometry.value_size_bytes),
        name + ".byte.offset");
    return builder.CreateInBoundsGEP(
        llvm::Type::getInt8Ty(context), base, byte_offset, name + ".ptr");
}

std::expected<void, std::string> emit_sample_carry_operation(
    llvm::IRBuilder<>& builder,
    detail::SampleStoragePlan const& storage,
    detail::SampleCarryOperationPlan const& operation,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    bool restore)
{
    auto allocation = compact_carry_allocation(storage, operation);
    if (!allocation) return std::unexpected(std::move(allocation.error()));
    auto working = sample_storage_binding(storage, operation.representation_index);
    if (!working) return std::unexpected(std::move(working.error()));
    if (operation.representation_index
        >= realtime_storage.sample_representations.size()) {
        return std::unexpected(
            "GraphJit sample carry lost resolved working storage");
    }
    auto* working_base = realtime_storage.sample_representations[
        operation.representation_index];
    if (working->value_capacity == 0
        || !is_power_of_2(working->value_capacity)
        || working->channel_layout != (*allocation)->channel_layout
        || working->geometry != (*allocation)->geometry) {
        return std::unexpected(
            "GraphJit sample carry working storage is invalid");
    }
    auto const& representation =
        storage.representations[operation.representation_index];
    auto const projection = emit_lattice_projection(
        builder,
        sample_index,
        block_size,
        representation.absolute_pace_exponent,
        "continuous.carry.lattice");
    sample_index = emit_sequential_begin_ordinal(
        builder,
        storage,
        realtime_storage,
        root_index,
        sample_index,
        representation.absolute_pace_exponent,
        "continuous.carry.sequential");
    block_size = projection.value_count;

    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* zero = llvm::ConstantInt::get(size_type, 0);
    auto* preheader = builder.GetInsertBlock();
    auto* loop = llvm::BasicBlock::Create(
        context,
        restore ? "sample.carry.restore" : "sample.carry.commit",
        function);
    auto* exit = llvm::BasicBlock::Create(
        context,
        restore ? "sample.carry.restore.end" : "sample.carry.commit.end",
        function);
    builder.CreateCondBr(
        builder.CreateICmpNE(
            block_size,
            zero,
            restore ? "sample.carry.restore.nonempty"
                    : "sample.carry.commit.nonempty"),
        loop,
        exit);

    builder.SetInsertPoint(loop);
    auto* carry_frame = builder.CreatePHI(
        size_type,
        2,
        restore ? "sample.carry.restore.frame" : "sample.carry.commit.frame");
    carry_frame->addIncoming(zero, preheader);
    auto* retained = llvm::ConstantInt::get(size_type, operation.retained_frames);
    auto* retained_before = llvm::ConstantInt::get(
        size_type, operation.retained_frames - operation.future_frames);
    llvm::Value* first_absolute = nullptr;
    if (restore) {
        first_absolute = builder.CreateSub(
            sample_index, retained_before, "sample.carry.restore.first");
    } else {
        first_absolute = builder.CreateSub(
            builder.CreateAdd(
                sample_index, block_size, "sample.carry.commit.end.index"),
            retained_before,
            "sample.carry.commit.first");
    }
    auto* absolute_frame = builder.CreateAdd(
        first_absolute,
        carry_frame,
        restore ? "sample.carry.restore.absolute"
                : "sample.carry.commit.absolute");

    auto const channels = channel_count((*allocation)->channel_layout);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        auto* carry_pointer = compact_carry_value_pointer(
            builder,
            realtime_storage.persistent_base,
            **allocation,
            carry_frame,
            channel,
            restore ? "sample.carry.restore.persist"
                    : "sample.carry.commit.persist");
        auto* working_pointer = continuous_value_pointer(
            builder,
            working_base,
            *working,
            absolute_frame,
            channel,
            restore ? "sample.carry.restore.working"
                    : "sample.carry.commit.working");
        if (restore) {
            builder.CreateMemCpy(
                working_pointer,
                llvm::Align(working->geometry.element_alignment),
                carry_pointer,
                llvm::Align((*allocation)->geometry.element_alignment),
                working->geometry.value_size_bytes);
        } else {
            builder.CreateMemCpy(
                carry_pointer,
                llvm::Align((*allocation)->geometry.element_alignment),
                working_pointer,
                llvm::Align(working->geometry.element_alignment),
                working->geometry.value_size_bytes);
        }
    }

    auto* next = builder.CreateAdd(
        carry_frame,
        llvm::ConstantInt::get(size_type, 1),
        restore ? "sample.carry.restore.next" : "sample.carry.commit.next");
    auto* done = builder.CreateICmpUGE(
        next,
        retained,
        restore ? "sample.carry.restore.done" : "sample.carry.commit.done");
    builder.CreateCondBr(done, exit, loop);
    carry_frame->addIncoming(next, loop);
    builder.SetInsertPoint(exit);
    return {};
}


std::expected<void, std::string> emit_event_carry_operation(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventCarryPlan const& carry,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    bool restore)
{
    if (carry.working_representation >= event_ports.ports.size()
        || carry.persistent_representation >= event_ports.ports.size()) {
        return std::unexpected(
            "GraphJit event carry references a missing storage");
    }
    auto const& working =
        event_ports.ports[carry.working_representation];
    auto const& persistent =
        event_ports.ports[carry.persistent_representation];
    if (working.persistent || !persistent.persistent
        || working.type != persistent.type
        || working.has_source_indices != persistent.has_source_indices
        || working.has_sequential_timeline
        || !persistent.has_sequential_timeline) {
        return std::unexpected(
            "GraphJit event carry has inconsistent storage ports");
    }

    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* working_base = realtime_storage.event_representations[
        carry.working_representation];
    auto* persistent_base = realtime_storage.event_representations[
        carry.persistent_representation];
    auto* working_count_pointer = byte_offset_pointer(
        builder,
        working_base,
        working.count_relative_offset,
        restore ? "event.carry.restore.working.count"
                : "event.carry.commit.working.count");
    auto* persistent_count_pointer = byte_offset_pointer(
        builder,
        persistent_base,
        persistent.count_relative_offset,
        restore ? "event.carry.restore.persist.count"
                : "event.carry.commit.persist.count");
    auto* working_events = byte_offset_pointer(
        builder,
        working_base,
        working.events_relative_offset,
        restore ? "event.carry.restore.working.events"
                : "event.carry.commit.working.events");
    auto* persistent_events = byte_offset_pointer(
        builder,
        persistent_base,
        persistent.events_relative_offset,
        restore ? "event.carry.restore.persist.events"
                : "event.carry.commit.persist.events");
    llvm::Value* working_source_indices = nullptr;
    llvm::Value* persistent_source_indices = nullptr;
    if (working.has_source_indices) {
        working_source_indices = byte_offset_pointer(
            builder,
            working_base,
            working.source_indices_relative_offset,
            restore ? "event.carry.restore.working.indices"
                    : "event.carry.commit.working.indices");
        persistent_source_indices = byte_offset_pointer(
            builder,
            persistent_base,
            persistent.source_indices_relative_offset,
            restore ? "event.carry.restore.persist.indices"
                    : "event.carry.commit.persist.indices");
    }

    if (restore) {
        auto* persistent_count = builder.CreateLoad(
            size_type,
            persistent_count_pointer,
            "event.carry.restore.persist.count.value");
        auto* module = builder.GetInsertBlock()->getModule();
        auto* initialized_pointer = byte_offset_pointer(
            builder,
            persistent_base,
            persistent.timeline_initialized_relative_offset,
            "event.carry.timeline.initialized");
        auto* expected_pointer = byte_offset_pointer(
            builder,
            persistent_base,
            persistent.expected_next_time_relative_offset,
            "event.carry.timeline.expected");
        auto* initialized = builder.CreateICmpNE(
            builder.CreateLoad(
                llvm::Type::getInt8Ty(context),
                initialized_pointer,
                "event.carry.timeline.initialized.value"),
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0),
            "event.carry.timeline.has.expected");
        auto* stored_expected = builder.CreateLoad(
            size_type,
            expected_pointer,
            "event.carry.timeline.expected.value");
        auto* expected = builder.CreateSelect(
            initialized,
            stored_expected,
            sample_index,
            "event.carry.timeline.rebase.origin");
        auto* rebase_type = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context),
            {pointer_type, size_type, size_type, size_type},
            false);
        auto rebase = module->getOrInsertFunction(
            detail::event_sequence_rebase_symbol, rebase_type);
        builder.CreateCall(
            rebase,
            {persistent_events, persistent_count, expected, sample_index});
        builder.CreateStore(
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 1),
            initialized_pointer);
        builder.CreateStore(
            builder.CreateAdd(sample_index, block_size), expected_pointer);
        llvm::Value* restored_count = nullptr;
        if (working.has_source_indices) {
            auto* helper_type = llvm::FunctionType::get(
                size_type,
                {pointer_type, pointer_type, size_type, pointer_type,
                 pointer_type, size_type},
                false);
            auto helper = module->getOrInsertFunction(
                detail::ordered_event_carry_restore_symbol, helper_type);
            restored_count = builder.CreateCall(
                helper,
                {persistent_events,
                 persistent_source_indices,
                 persistent_count,
                 working_events,
                 working_source_indices,
                 llvm::ConstantInt::get(size_type, working.event_capacity)},
                "event.carry.restore.count");
        } else {
            auto* helper_type = llvm::FunctionType::get(
                size_type,
                {pointer_type, size_type, pointer_type, size_type},
                false);
            auto helper = module->getOrInsertFunction(
                detail::event_carry_restore_symbol, helper_type);
            restored_count = builder.CreateCall(
                helper,
                {persistent_events,
                 persistent_count,
                 working_events,
                 llvm::ConstantInt::get(size_type, working.event_capacity)},
                "event.carry.restore.count");
        }
        builder.CreateStore(restored_count, working_count_pointer);
        return {};
    }

    auto* working_count = builder.CreateLoad(
        size_type,
        working_count_pointer,
        "event.carry.commit.working.count.value");
    auto* module = builder.GetInsertBlock()->getModule();
    llvm::Value* committed_count = nullptr;
    if (working.has_source_indices) {
        auto* helper_type = llvm::FunctionType::get(
            size_type,
            {pointer_type, pointer_type, size_type, size_type, size_type,
             size_type, size_type, pointer_type, pointer_type, size_type},
            false);
        auto helper = module->getOrInsertFunction(
            detail::ordered_event_carry_commit_symbol, helper_type);
        committed_count = builder.CreateCall(
            helper,
            {working_events,
             working_source_indices,
             working_count,
             sample_index,
             block_size,
             llvm::ConstantInt::get(
                 size_type, carry.retained_history_samples),
             llvm::ConstantInt::get(
                 size_type, carry.retained_latency_samples),
             persistent_events,
             persistent_source_indices,
             llvm::ConstantInt::get(size_type, persistent.event_capacity)},
            "event.carry.commit.count");
    } else {
        auto* helper_type = llvm::FunctionType::get(
            size_type,
            {pointer_type, size_type, size_type, size_type, size_type, size_type,
             pointer_type, size_type},
            false);
        auto helper = module->getOrInsertFunction(
            detail::event_carry_commit_symbol, helper_type);
        committed_count = builder.CreateCall(
            helper,
            {working_events,
             working_count,
             sample_index,
             block_size,
             llvm::ConstantInt::get(
                 size_type, carry.retained_history_samples),
             llvm::ConstantInt::get(
                 size_type, carry.retained_latency_samples),
             persistent_events,
             llvm::ConstantInt::get(size_type, persistent.event_capacity)},
            "event.carry.commit.count");
    }
    builder.CreateStore(committed_count, persistent_count_pointer);
    return {};
}

std::expected<void, std::string> initialize_event_feedback_cursors_after_carry_restore(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventCarryPlan const& carry,
    std::vector<llvm::Value*> const& event_feedback_cursors,
    EmittedRealtimeStorage const& realtime_storage)
{
    if (carry.working_representation >= event_ports.ports.size()) {
        return std::unexpected(
            "GraphJit event carry feedback cursor references a missing working storage");
    }
    auto const& working =
        event_ports.ports[carry.working_representation];
    if (working.persistent || working.persistent_ring) {
        return std::unexpected(
            "GraphJit event carry feedback cursor has invalid working storage");
    }

    bool needs_initialize = false;
    for (std::size_t feedback_index = 0;
         feedback_index < event_ports.feedback_operations.size();
         ++feedback_index) {
        if (event_ports.feedback_operations[feedback_index].source_representation
            != carry.working_representation) {
            continue;
        }
        if (event_ports.feedback_operations[feedback_index]
                .source_sequence_restarts_each_append) {
            continue;
        }
        if (feedback_index >= event_feedback_cursors.size()
            || event_feedback_cursors[feedback_index] == nullptr) {
            return std::unexpected(
                "GraphJit event carry feedback cursor is missing root-call storage");
        }
        needs_initialize = true;
    }
    if (!needs_initialize) return {};

    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* working_count_pointer = byte_offset_pointer(
        builder,
        realtime_storage.event_representations[carry.working_representation],
        working.count_relative_offset,
        "event.carry.feedback.initialize.count");
    auto* restored_count = builder.CreateLoad(
        size_type, working_count_pointer, "event.carry.feedback.initialize.value");
    for (std::size_t feedback_index = 0;
         feedback_index < event_ports.feedback_operations.size();
         ++feedback_index) {
        if (event_ports.feedback_operations[feedback_index].source_representation
                == carry.working_representation
            && !event_ports.feedback_operations[feedback_index]
                    .source_sequence_restarts_each_append) {
            builder.CreateStore(
                restored_count, event_feedback_cursors[feedback_index]);
        }
    }
    return {};
}

std::expected<void, std::string> emit_event_persistent_ring_prune(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventPersistentRingPlan const& ring,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    if (ring.storage >= event_ports.ports.size()) {
        return std::unexpected(
            "GraphJit persistent event ring references a missing storage");
    }
    auto const& storage = event_ports.ports[ring.storage];
    if (!storage.region.valid()
        || !storage.persistent
        || !storage.persistent_ring
        || !storage.has_sequential_timeline) {
        return std::unexpected(
            "GraphJit persistent event ring has inconsistent storage");
    }

    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* representation_base =
        realtime_storage.event_representations[ring.storage];
    auto* read_index_pointer = byte_offset_pointer(
        builder,
        representation_base,
        storage.read_index_relative_offset,
        "event.ring.prune.read");
    auto* write_index_pointer = byte_offset_pointer(
        builder,
        representation_base,
        storage.write_index_relative_offset,
        "event.ring.prune.write");
    auto* events = byte_offset_pointer(
        builder,
        representation_base,
        storage.events_relative_offset,
        "event.ring.prune.events");
    auto* read_index = builder.CreateLoad(
        size_type, read_index_pointer, "event.ring.prune.read.value");
    auto* write_index = builder.CreateLoad(
        size_type, write_index_pointer, "event.ring.prune.write.value");

    auto* initialized_pointer = byte_offset_pointer(
        builder,
        representation_base,
        storage.timeline_initialized_relative_offset,
        "event.ring.timeline.initialized");
    auto* expected_pointer = byte_offset_pointer(
        builder,
        representation_base,
        storage.expected_next_time_relative_offset,
        "event.ring.timeline.expected");
    auto* initialized = builder.CreateICmpNE(
        builder.CreateLoad(
            llvm::Type::getInt8Ty(context),
            initialized_pointer,
            "event.ring.timeline.initialized.value"),
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0),
        "event.ring.timeline.has.expected");
    auto* stored_expected = builder.CreateLoad(
        size_type, expected_pointer, "event.ring.timeline.expected.value");
    auto* expected = builder.CreateSelect(
        initialized,
        stored_expected,
        sample_index,
        "event.ring.timeline.rebase.origin");
    auto* rebase_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {pointer_type, size_type, size_type, size_type, size_type, size_type},
        false);
    auto* module = builder.GetInsertBlock()->getModule();
    auto rebase = module->getOrInsertFunction(
        detail::event_ring_rebase_symbol, rebase_type);
    builder.CreateCall(
        rebase,
        {events,
         llvm::ConstantInt::get(size_type, storage.event_capacity),
         read_index,
         write_index,
         expected,
         sample_index});
    builder.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 1),
        initialized_pointer);
    builder.CreateStore(
        builder.CreateAdd(sample_index, block_size), expected_pointer);

    auto* helper_type = llvm::FunctionType::get(
        size_type,
        {pointer_type, size_type, size_type, size_type, size_type, size_type},
        false);
    auto helper = module->getOrInsertFunction(
        detail::event_persistent_ring_prune_symbol, helper_type);
    auto* pruned_read_index = builder.CreateCall(
        helper,
        {events,
         llvm::ConstantInt::get(size_type, storage.event_capacity),
         read_index,
         write_index,
         sample_index,
         llvm::ConstantInt::get(size_type, ring.retained_history_samples)},
        "event.ring.prune.read.next");
    builder.CreateStore(pruned_read_index, read_index_pointer);
    return {};
}

std::expected<void, std::string> emit_event_feedback_append(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventFeedbackPlan const& feedback,
    llvm::Value* feedback_cursor_pointer,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* sample_index)
{
    if (feedback.source_representation >= event_ports.ports.size()
        || feedback.target_representation >= event_ports.ports.size()) {
        return std::unexpected(
            "GraphJit event feedback references a missing storage");
    }
    if (feedback_cursor_pointer == nullptr
        && !feedback.source_sequence_restarts_each_append) {
        return std::unexpected(
            "GraphJit event feedback has no root-call source cursor");
    }
    auto const& source =
        event_ports.ports[feedback.source_representation];
    auto const& target =
        event_ports.ports[feedback.target_representation];
    if (source.type != target.type) {
        return std::unexpected(
            "GraphJit exact-type event feedback changed event type");
    }
    if (feedback.storage_plan.kind
        == RealtimeBufferStorageKind::full_node_storage) {
        if (!target.persistent || !target.persistent_ring) {
            return std::unexpected(
                "GraphJit full event feedback has no persistent ring");
        }
    } else if (target.persistent || target.persistent_ring) {
        return std::unexpected(
            "GraphJit transient/carry event feedback has invalid working storage");
    }

    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* plan_word_type = llvm::Type::getInt32Ty(context);
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* ordering_origin = feedback.ordering_history_samples == 0
        ? sample_index
        : builder.CreateSub(
            sample_index,
            llvm::ConstantInt::get(
                sample_index->getType(),
                feedback.ordering_history_samples),
            "event.feedback.ordering.origin");
    llvm::Value* source_begin = nullptr;
    if (feedback.source_sequence_restarts_each_append) {
        source_begin = llvm::ConstantInt::get(size_type, 0);
    } else {
        source_begin = builder.CreateLoad(
            size_type,
            feedback_cursor_pointer,
            "event.feedback.source.cursor");
    }
    llvm::Value* source_end = nullptr;
    auto* source_base = realtime_storage.event_representations[
        feedback.source_representation];
    auto* target_base = realtime_storage.event_representations[
        feedback.target_representation];
    if (source.persistent_ring) {
        auto* source_write_pointer = byte_offset_pointer(
            builder,
            source_base,
            source.write_index_relative_offset,
            "event.feedback.source.write");
        source_end = builder.CreateLoad(
            size_type,
            source_write_pointer,
            "event.feedback.source.write.value");
    } else {
        auto* source_count_pointer = byte_offset_pointer(
            builder,
            source_base,
            source.count_relative_offset,
            "event.feedback.source.count");
        auto* source_count = builder.CreateLoad(
            size_type,
            source_count_pointer,
            "event.feedback.source.count.value");
        auto* source_capacity = llvm::ConstantInt::get(
            size_type, source.event_capacity);
        source_end = builder.CreateSelect(
            builder.CreateICmpULT(
                source_count,
                source_capacity,
                "event.feedback.source.count.in_range"),
            source_count,
            source_capacity,
            "event.feedback.source.count.bounded");
    }
    auto* source_events = byte_offset_pointer(
        builder,
        source_base,
        source.events_relative_offset,
        "event.feedback.source.events");
    llvm::Value* source_indices = llvm::ConstantPointerNull::get(pointer_type);
    if (feedback.source_index_filter) {
        if (!source.has_source_indices) {
            return std::unexpected(
                "GraphJit filtered event feedback lost its source-index sidecar");
        }
        source_indices = byte_offset_pointer(
            builder,
            source_base,
            source.source_indices_relative_offset,
            "event.feedback.source.indices");
    }
    auto* source_index_filter = llvm::ConstantInt::get(
        size_type, feedback.source_index_filter.value_or(0));
    auto* filter_source_index = llvm::ConstantInt::get(
        plan_word_type, feedback.source_index_filter.has_value());
    auto* target_events = byte_offset_pointer(
        builder,
        target_base,
        target.events_relative_offset,
        "event.feedback.target.events");

    // Most event slices produce no events. Keep the audio-thread fast path to a
    // source-count comparison and avoid the out-of-line feedback helper
    // entirely unless this producer supplied events.
    if (!feedback.source_sequence_restarts_each_append) {
        builder.CreateStore(source_end, feedback_cursor_pointer);
    }
    auto* has_new_events = builder.CreateICmpULT(
        source_begin, source_end, "event.feedback.has_new_events");
    auto* function = builder.GetInsertBlock()->getParent();
    auto* append_block = llvm::BasicBlock::Create(
        context, "event.feedback.append", function);
    auto* continue_block = llvm::BasicBlock::Create(
        context, "event.feedback.continue", function);
    builder.CreateCondBr(has_new_events, append_block, continue_block);

    builder.SetInsertPoint(append_block);
    auto* module = builder.GetInsertBlock()->getModule();
    if (target.persistent_ring) {
        auto* target_read_pointer = byte_offset_pointer(
            builder,
            target_base,
            target.read_index_relative_offset,
            "event.feedback.ring.read");
        auto* target_write_pointer = byte_offset_pointer(
            builder,
            target_base,
            target.write_index_relative_offset,
            "event.feedback.ring.write");
        if (source.persistent_ring) {
            auto* helper_type = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context),
                {pointer_type, pointer_type, size_type, plan_word_type,
                 size_type, size_type, size_type, size_type, size_type,
                 pointer_type, size_type, pointer_type, pointer_type},
                false);
            auto helper = module->getOrInsertFunction(
                detail::event_feedback_append_ring_source_symbol, helper_type);
            builder.CreateCall(
                helper,
                {source_events,
                 source_indices,
                 source_index_filter,
                 filter_source_index,
                 llvm::ConstantInt::get(size_type, source.event_capacity),
                 source_begin,
                 source_end,
                 ordering_origin,
                 llvm::ConstantInt::get(size_type, feedback.loop_extra_latency),
                 target_events,
                 llvm::ConstantInt::get(size_type, target.event_capacity),
                 target_read_pointer,
                 target_write_pointer});
        } else {
            auto* helper_type = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context),
                {pointer_type, pointer_type, size_type, plan_word_type,
                 size_type, size_type, size_type, size_type,
                 pointer_type, size_type, pointer_type, pointer_type},
                false);
            auto helper = module->getOrInsertFunction(
                detail::event_feedback_append_symbol, helper_type);
            builder.CreateCall(
                helper,
                {source_events,
                 source_indices,
                 source_index_filter,
                 filter_source_index,
                 source_begin,
                 source_end,
                 ordering_origin,
                 llvm::ConstantInt::get(size_type, feedback.loop_extra_latency),
                 target_events,
                 llvm::ConstantInt::get(size_type, target.event_capacity),
                 target_read_pointer,
                 target_write_pointer});
        }
    } else {
        auto* target_count_pointer = byte_offset_pointer(
            builder,
            target_base,
            target.count_relative_offset,
            "event.feedback.sequence.count");
        if (source.persistent_ring) {
            auto* helper_type = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context),
                {pointer_type, pointer_type, size_type, plan_word_type,
                 size_type, size_type, size_type, size_type, size_type,
                 pointer_type, size_type, pointer_type},
                false);
            auto helper = module->getOrInsertFunction(
                detail::event_feedback_append_sequence_ring_source_symbol,
                helper_type);
            builder.CreateCall(
                helper,
                {source_events,
                 source_indices,
                 source_index_filter,
                 filter_source_index,
                 llvm::ConstantInt::get(size_type, source.event_capacity),
                 source_begin,
                 source_end,
                 ordering_origin,
                 llvm::ConstantInt::get(size_type, feedback.loop_extra_latency),
                 target_events,
                 llvm::ConstantInt::get(size_type, target.event_capacity),
                 target_count_pointer});
        } else {
            auto* helper_type = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context),
                {pointer_type, pointer_type, size_type, plan_word_type,
                 size_type, size_type, size_type, size_type,
                 pointer_type, size_type, pointer_type},
                false);
            auto helper = module->getOrInsertFunction(
                detail::event_feedback_append_sequence_symbol, helper_type);
            builder.CreateCall(
                helper,
                {source_events,
                 source_indices,
                 source_index_filter,
                 filter_source_index,
                 source_begin,
                 source_end,
                 ordering_origin,
                 llvm::ConstantInt::get(size_type, feedback.loop_extra_latency),
                 target_events,
                 llvm::ConstantInt::get(size_type, target.event_capacity),
                 target_count_pointer});
        }
    }
    builder.CreateBr(continue_block);
    builder.SetInsertPoint(continue_block);
    return {};
}

std::expected<void, std::string> emit_event_sequence_reset(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    std::size_t representation_index,
    EmittedRealtimeStorage const& realtime_storage)
{
    if (representation_index >= event_ports.ports.size()) {
        return std::unexpected(
            "GraphJit event sequence reset references a missing storage");
    }
    auto const& storage = event_ports.ports[representation_index];
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* count = byte_offset_pointer(
        builder,
        realtime_storage.event_representations[representation_index],
        storage.count_relative_offset,
        "event.reset.count");
    builder.CreateStore(llvm::ConstantInt::get(size_type, 0), count);
    return {};
}

std::expected<void, std::string> emit_event_merge(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventMergePlan const& merge,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index)
{
    if (merge.target_representation >= event_ports.ports.size()
        || merge.source_representations.empty()) {
        return std::unexpected(
            "GraphJit event merge references a missing storage");
    }
    auto const& target =
        event_ports.ports[merge.target_representation];
    auto const ordered_merge = target.has_source_indices;
    if ((ordered_merge
            && merge.source_indices.size()
                != merge.source_representations.size())
        || (!ordered_merge && !merge.source_indices.empty())) {
        return std::unexpected(
            "GraphJit ordered event merge has inconsistent source indices");
    }
    if ((target.event_capacity != 0
            && !is_power_of_2(target.event_capacity))) {
        return std::unexpected(
            "GraphJit event merge target has invalid storage");
    }
    if (target.persistent_ring && !merge.preserve_existing_target) {
        return std::unexpected(
            "GraphJit persistent event merge must preserve retained target events");
    }
    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* merge_origin = merge.ordering_history_samples == 0
        ? root_index
        : builder.CreateSub(
            root_index,
            llvm::ConstantInt::get(
                root_index->getType(),
                merge.ordering_history_samples),
            "event.merge.origin");
    auto* target_base = realtime_storage.event_representations[
        merge.target_representation];
    auto* target_events = byte_offset_pointer(
        builder,
        target_base,
        target.events_relative_offset,
        "event.merge.target.events");
    llvm::Value* target_source_indices = nullptr;
    if (ordered_merge) {
        target_source_indices = byte_offset_pointer(
            builder,
            target_base,
            target.source_indices_relative_offset,
            "event.merge.target.indices");
    }

    llvm::Value* target_read = llvm::ConstantInt::get(size_type, 0);
    llvm::Value* target_write = llvm::ConstantInt::get(size_type, 0);
    llvm::Value* target_count_or_write_pointer = nullptr;
    if (target.persistent_ring) {
        auto* read_pointer = byte_offset_pointer(
            builder,
            target_base,
            target.read_index_relative_offset,
            "event.merge.target.read.ptr");
        auto* write_pointer = byte_offset_pointer(
            builder,
            target_base,
            target.write_index_relative_offset,
            "event.merge.target.write.ptr");
        target_read = builder.CreateLoad(
            size_type, read_pointer, "event.merge.target.read");
        target_write = builder.CreateLoad(
            size_type, write_pointer, "event.merge.target.write");
        target_count_or_write_pointer = write_pointer;
    } else {
        auto* count_pointer = byte_offset_pointer(
            builder,
            target_base,
            target.count_relative_offset,
            "event.merge.target.count.ptr");
        if (merge.preserve_existing_target) {
            target_write = builder.CreateLoad(
                size_type, count_pointer, "event.merge.target.count");
        }
        target_count_or_write_pointer = count_pointer;
    }

    auto validate_source = [&](std::size_t representation_index)
        -> std::expected<detail::EventRepresentationPlan const*, std::string> {
        if (representation_index >= event_ports.ports.size()) {
            return std::unexpected(
                "GraphJit event merge references a missing source storage");
        }
        auto const& source = event_ports.ports[representation_index];
        if (source.persistent_ring || source.type != target.type
            || source.has_source_indices) {
            return std::unexpected(
                "GraphJit event merge source has inconsistent storage");
        }
        return &source;
    };

    auto* module = builder.GetInsertBlock()->getModule();

    for (std::size_t source_index = 0;
         source_index < merge.source_representations.size(); ++source_index) {
        auto const representation_index =
            merge.source_representations[source_index];
        auto source = validate_source(representation_index);
        if (!source) return std::unexpected(std::move(source.error()));
        auto* source_count_pointer = byte_offset_pointer(
            builder,
            realtime_storage.event_representations[representation_index],
            (*source)->count_relative_offset,
            "event.merge.source.count.ptr." + std::to_string(source_index));
        auto* source_count = builder.CreateLoad(
            size_type,
            source_count_pointer,
            "event.merge.source.count." + std::to_string(source_index));
        auto* source_events = byte_offset_pointer(
            builder,
            realtime_storage.event_representations[representation_index],
            (*source)->events_relative_offset,
            "event.merge.source.events." + std::to_string(source_index));
        if (ordered_merge) {
            auto* helper_type = llvm::FunctionType::get(
                size_type,
                {pointer_type, pointer_type, size_type, size_type, size_type,
                 pointer_type, size_type, size_type, size_type, size_type},
                false);
            auto helper = module->getOrInsertFunction(
                detail::ordered_event_sequence_merge_symbol, helper_type);
            target_write = builder.CreateCall(
                helper,
                {target_events,
                 target_source_indices,
                 llvm::ConstantInt::get(size_type, target.event_capacity),
                 target_read,
                 target_write,
                 source_events,
                 llvm::ConstantInt::get(size_type, (*source)->event_capacity),
                 source_count,
                 llvm::ConstantInt::get(
                     size_type, merge.source_indices[source_index]),
                 merge_origin},
                "event.merge.ordered.write." + std::to_string(source_index));
        } else {
            auto* helper_type = llvm::FunctionType::get(
                size_type,
                {pointer_type, size_type, size_type, size_type,
                 pointer_type, size_type, size_type, size_type},
                false);
            auto helper = module->getOrInsertFunction(
                detail::event_sequence_merge_symbol, helper_type);
            target_write = builder.CreateCall(
                helper,
                {target_events,
                 llvm::ConstantInt::get(size_type, target.event_capacity),
                 target_read,
                 target_write,
                 source_events,
                 llvm::ConstantInt::get(size_type, (*source)->event_capacity),
                 source_count,
                 merge_origin},
                "event.merge.write." + std::to_string(source_index));
        }
    }
    builder.CreateStore(target_write, target_count_or_write_pointer);
    return {};
}

std::expected<void, std::string> emit_event_materialization(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventMaterializationPlan const& materialization,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    if (materialization.source_representation >= event_ports.ports.size()
        || materialization.target_representation >= event_ports.ports.size()) {
        return std::unexpected(
            "GraphJit event materialization references a missing storage");
    }
    auto const& source =
        event_ports.ports[materialization.source_representation];
    auto const& target =
        event_ports.ports[materialization.target_representation];
    if (materialization.conversion.source_type != source.type
        || materialization.conversion.target_type != target.type
        || materialization.conversion.step_count
            > EventConversionPlan::max_steps) {
        return std::unexpected(
            "GraphJit transient event materialization has an invalid conversion plan");
    }

    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* source_base = realtime_storage.event_representations[
        materialization.source_representation];
    auto* target_base = realtime_storage.event_representations[
        materialization.target_representation];
    auto* target_count_pointer = byte_offset_pointer(
        builder,
        target_base,
        target.count_relative_offset,
        "event.materialize.target.count");
    auto* source_events = byte_offset_pointer(
        builder,
        source_base,
        source.events_relative_offset,
        "event.materialize.source.events");
    llvm::Value* source_indices = llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(context));
    if (materialization.source_index_filter
        || materialization.preserve_source_indices) {
        if (!source.has_source_indices) {
            return std::unexpected(
                "GraphJit event source selection lost its source-index sidecar");
        }
        source_indices = byte_offset_pointer(
            builder,
            source_base,
            source.source_indices_relative_offset,
            "event.materialize.source.indices");
    }
    auto* target_events = byte_offset_pointer(
        builder,
        target_base,
        target.events_relative_offset,
        "event.materialize.target.events");
    llvm::Value* target_indices = llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(context));
    if (materialization.preserve_source_indices) {
        if (!target.has_source_indices) {
            return std::unexpected(
                "GraphJit event materialization target lost its source-index sidecar");
        }
        target_indices = byte_offset_pointer(
            builder,
            target_base,
            target.source_indices_relative_offset,
            "event.materialize.target.indices");
    }

    if (materialization.source_index_filter
        || materialization.select_invocation_window
        || source.persistent_ring) {
        llvm::Value* source_read_index = llvm::ConstantInt::get(size_type, 0);
        llvm::Value* source_write_index = nullptr;
        if (source.persistent_ring) {
            auto* read_index_pointer = byte_offset_pointer(
                builder,
                source_base,
                source.read_index_relative_offset,
                "event.materialize.source.read.index");
            auto* write_index_pointer = byte_offset_pointer(
                builder,
                source_base,
                source.write_index_relative_offset,
                "event.materialize.source.write.index");
            source_read_index = builder.CreateLoad(
                size_type,
                read_index_pointer,
                "event.materialize.source.read");
            source_write_index = builder.CreateLoad(
                size_type,
                write_index_pointer,
                "event.materialize.source.write");
        } else {
            auto* source_count_pointer = byte_offset_pointer(
                builder,
                source_base,
                source.count_relative_offset,
                "event.materialize.source.count");
            source_write_index = builder.CreateLoad(
                size_type,
                source_count_pointer,
                "event.materialize.source.write");
        }

        auto* plan_word_type = llvm::Type::getInt32Ty(context);
        auto* pointer_type = llvm::PointerType::getUnqual(context);
        auto* helper_type = llvm::FunctionType::get(
            size_type,
            {plan_word_type, plan_word_type, plan_word_type, plan_word_type,
             plan_word_type, size_type, pointer_type, pointer_type, size_type,
             plan_word_type, size_type, size_type, size_type,
             sample_index->getType(), size_type, size_type, pointer_type,
             size_type, pointer_type},
            false);
        auto* module = builder.GetInsertBlock()->getModule();
        auto helper = module->getOrInsertFunction(
            detail::event_sequence_materialization_symbol, helper_type);
        auto step = [&](std::size_t index) -> llvm::Constant* {
            auto const value = index < materialization.conversion.step_count
                ? static_cast<std::underlying_type_t<EventConversionStepId>>(
                      materialization.conversion.steps[index])
                : 0;
            return llvm::ConstantInt::get(plan_word_type, value);
        };
        auto* materialized_count = builder.CreateCall(
            helper,
            {llvm::ConstantInt::get(
                 plan_word_type,
                 static_cast<std::underlying_type_t<EventTypeId>>(source.type)),
             llvm::ConstantInt::get(
                 plan_word_type,
                 static_cast<std::underlying_type_t<EventTypeId>>(target.type)),
             step(0), step(1), step(2),
             llvm::ConstantInt::get(
                 size_type, materialization.conversion.step_count),
             source_events,
             source_indices,
             llvm::ConstantInt::get(
                 size_type,
                 materialization.source_index_filter.value_or(0)),
             llvm::ConstantInt::get(
                 plan_word_type,
                 materialization.source_index_filter.has_value()),
             llvm::ConstantInt::get(size_type, source.event_capacity),
             source_read_index,
             source_write_index,
             sample_index,
             block_size,
             llvm::ConstantInt::get(size_type, materialization.history_samples),
             target_events,
             llvm::ConstantInt::get(size_type, target.event_capacity),
             target_indices},
            "event.materialize.windowed.count");
        builder.CreateStore(materialized_count, target_count_pointer);
        return {};
    }

    auto* source_count_pointer = byte_offset_pointer(
        builder,
        source_base,
        source.count_relative_offset,
        "event.materialize.source.count");
    auto* count = builder.CreateLoad(
        size_type, source_count_pointer, "event.materialize.count");
    auto* source_capacity = llvm::ConstantInt::get(
        size_type, source.event_capacity);
    auto* bounded_source_count = builder.CreateSelect(
        builder.CreateICmpULE(
            count, source_capacity, "event.materialize.source.in.bounds"),
        count,
        source_capacity,
        "event.materialize.source.bounded.count");

    if (materialization.conversion.step_count == 0) {
        if (source.type != target.type) {
            return std::unexpected(
                "GraphJit identity event materialization changes event type");
        }
        auto* target_capacity = llvm::ConstantInt::get(
            size_type, target.event_capacity);
        auto* bounded_count = builder.CreateSelect(
            builder.CreateICmpULE(
                bounded_source_count,
                target_capacity,
                "event.materialize.target.in.bounds"),
            bounded_source_count,
            target_capacity,
            "event.materialize.bounded.count");
        auto* byte_count = builder.CreateMul(
            bounded_count,
            llvm::ConstantInt::get(size_type, sizeof(TimedEvent)),
            "event.materialize.bytes");
        builder.CreateMemCpy(
            target_events,
            llvm::Align(alignof(TimedEvent)),
            source_events,
            llvm::Align(alignof(TimedEvent)),
            byte_count);
        if (materialization.preserve_source_indices) {
            auto* index_byte_count = builder.CreateMul(
                bounded_count,
                llvm::ConstantInt::get(size_type, sizeof(std::size_t)),
                "event.materialize.index.bytes");
            builder.CreateMemCpy(
                target_indices,
                llvm::Align(alignof(std::size_t)),
                source_indices,
                llvm::Align(alignof(std::size_t)),
                index_byte_count);
        }
        builder.CreateStore(bounded_count, target_count_pointer);
        return {};
    }

    auto* plan_word_type = llvm::Type::getInt32Ty(context);
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* helper_type = llvm::FunctionType::get(
        size_type,
        {plan_word_type, plan_word_type, plan_word_type, plan_word_type,
         plan_word_type, size_type, pointer_type, size_type, pointer_type,
         size_type, pointer_type, pointer_type},
        false);
    auto* module = builder.GetInsertBlock()->getModule();
    auto helper = module->getOrInsertFunction(
        detail::event_sequence_conversion_symbol, helper_type);

    auto step = [&](std::size_t index) -> llvm::Constant* {
        auto const value = index < materialization.conversion.step_count
            ? static_cast<std::underlying_type_t<EventConversionStepId>>(
                  materialization.conversion.steps[index])
            : 0;
        return llvm::ConstantInt::get(plan_word_type, value);
    };
    auto* converted_count = builder.CreateCall(
        helper,
        {llvm::ConstantInt::get(
             plan_word_type,
             static_cast<std::underlying_type_t<EventTypeId>>(source.type)),
         llvm::ConstantInt::get(
             plan_word_type,
             static_cast<std::underlying_type_t<EventTypeId>>(target.type)),
         step(0), step(1), step(2),
         llvm::ConstantInt::get(
             size_type, materialization.conversion.step_count),
         source_events, bounded_source_count, target_events,
         llvm::ConstantInt::get(size_type, target.event_capacity),
         source_indices,
         target_indices},
        "event.materialize.converted.count");
    builder.CreateStore(converted_count, target_count_pointer);
    return {};
}

std::expected<void, std::string> emit_event_operations(
    llvm::IRBuilder<>& builder,
    detail::LoweringPlan const& plan,
    std::span<detail::EventOperationRef const> operations,
    std::vector<llvm::Value*> const& event_feedback_cursors,
    EmittedRealtimeStorage const& realtime_storage,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    for (auto const operation : operations) {
        switch (operation.kind) {
        case detail::EventOperationKind::sequence_reset: {
            auto emitted = emit_event_sequence_reset(
                builder, plan.event_ports, operation.index, realtime_storage);
            if (!emitted) return std::unexpected(std::move(emitted.error()));
            break;
        }
        case detail::EventOperationKind::persistent_ring_prune: {
            if (operation.index >= plan.event_ports.persistent_rings.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing persistent event ring");
            }
            auto emitted = emit_event_persistent_ring_prune(
                builder,
                plan.event_ports,
                plan.event_ports.persistent_rings[operation.index],
                realtime_storage,
                sample_index,
                block_size);
            if (!emitted) return std::unexpected(std::move(emitted.error()));
            break;
        }
        case detail::EventOperationKind::carry_restore:
        case detail::EventOperationKind::carry_commit: {
            if (operation.index >= plan.event_ports.carry_operations.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing event carry operation");
            }
            auto const restore = operation.kind
                == detail::EventOperationKind::carry_restore;
            auto const& carry = plan.event_ports.carry_operations[operation.index];
            auto emitted = emit_event_carry_operation(
                builder,
                plan.event_ports,
                carry,
                realtime_storage,
                sample_index,
                block_size,
                restore);
            if (!emitted) return std::unexpected(std::move(emitted.error()));
            if (restore) {
                auto initialized = initialize_event_feedback_cursors_after_carry_restore(
                    builder,
                    plan.event_ports,
                    carry,
                    event_feedback_cursors,
                    realtime_storage);
                if (!initialized) return std::unexpected(std::move(initialized.error()));
            }
            break;
        }
        case detail::EventOperationKind::merge: {
            if (operation.index >= plan.event_ports.merges.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing event merge");
            }
            auto emitted = emit_event_merge(
                builder,
                plan.event_ports,
                plan.event_ports.merges[operation.index],
                realtime_storage,
                root_index);
            if (!emitted) return std::unexpected(std::move(emitted.error()));
            break;
        }
        case detail::EventOperationKind::materialize: {
            if (operation.index >= plan.event_ports.materializations.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing event materialization");
            }
            auto emitted = emit_event_materialization(
                builder,
                plan.event_ports,
                plan.event_ports.materializations[operation.index],
                realtime_storage,
                sample_index,
                block_size);
            if (!emitted) return std::unexpected(std::move(emitted.error()));
            break;
        }
        case detail::EventOperationKind::feedback_append: {
            if (operation.index >= plan.event_ports.feedback_operations.size()
                || operation.index >= event_feedback_cursors.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing event feedback append");
            }
            auto emitted = emit_event_feedback_append(
                builder,
                plan.event_ports,
                plan.event_ports.feedback_operations[operation.index],
                event_feedback_cursors[operation.index],
                realtime_storage,
                sample_index);
            if (!emitted) return std::unexpected(std::move(emitted.error()));
            break;
        }
        }
    }
    return {};
}

std::expected<void, std::string> mark_event_activity_writes(
    detail::LoweringPlan const& plan,
    std::span<detail::EventOperationRef const> operations,
    ActivityEmissionCache& activity_cache)
{
    auto advance = [&](std::size_t representation)
        -> std::expected<void, std::string> {
        if (representation
            >= activity_cache.event_representation_epochs.size()) {
            return std::unexpected(
                "GraphJit activity cache references missing event storage");
        }
        auto& epoch = activity_cache
            .event_representation_epochs[representation];
        if (epoch == std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(
                "GraphJit event activity emission epoch overflows size_t");
        }
        ++epoch;
        return {};
    };

    for (auto const operation : operations) {
        std::size_t target = 0;
        switch (operation.kind) {
        case detail::EventOperationKind::sequence_reset:
            // A sequence-reset operation stores the representation index
            // directly. EventSequenceResetPlan is only the planning input that
            // supplies this value.
            if (operation.index >= plan.event_ports.ports.size()) {
                return std::unexpected(
                    "GraphJit event activity lost a sequence reset");
            }
            target = operation.index;
            break;
        case detail::EventOperationKind::persistent_ring_prune:
            if (operation.index >= plan.event_ports.persistent_rings.size()) {
                return std::unexpected(
                    "GraphJit event activity lost a persistent-ring prune");
            }
            target = plan.event_ports.persistent_rings[operation.index].storage;
            break;
        case detail::EventOperationKind::carry_restore:
        case detail::EventOperationKind::carry_commit:
            if (operation.index >= plan.event_ports.carry_operations.size()) {
                return std::unexpected(
                    "GraphJit event activity lost a carry operation");
            }
            target = operation.kind
                    == detail::EventOperationKind::carry_restore
                ? plan.event_ports.carry_operations[operation.index]
                      .working_representation
                : plan.event_ports.carry_operations[operation.index]
                      .persistent_representation;
            break;
        case detail::EventOperationKind::merge:
            if (operation.index >= plan.event_ports.merges.size()) {
                return std::unexpected(
                    "GraphJit event activity lost a merge");
            }
            target = plan.event_ports.merges[operation.index]
                .target_representation;
            break;
        case detail::EventOperationKind::materialize:
            if (operation.index >= plan.event_ports.materializations.size()) {
                return std::unexpected(
                    "GraphJit event activity lost a materialization");
            }
            target = plan.event_ports.materializations[operation.index]
                .target_representation;
            break;
        case detail::EventOperationKind::feedback_append:
            if (operation.index
                >= plan.event_ports.feedback_operations.size()) {
                return std::unexpected(
                    "GraphJit event activity lost a feedback append");
            }
            target = plan.event_ports.feedback_operations[operation.index]
                .target_representation;
            break;
        }
        if (auto marked = advance(target); !marked) return marked;
    }
    return {};
}

std::expected<void, std::string> emit_tick_capture_operations(
    llvm::IRBuilder<>& builder,
    BackgroundEvaluationPlan const& background,
    BackgroundNodeIndex node,
    TickNodeInvocationPlan const& invocation_plan,
    detail::PrimitiveSamplePortPlan const& sample_plan,
    EmittedPrimitiveSamplePorts const& sample_bindings,
    detail::PrimitiveEventPortPlan const& event_plan,
    EmittedPrimitiveEventPorts const& event_bindings,
    llvm::Value* invocation,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    auto validate_range = [](
        std::size_t begin,
        std::size_t count,
        std::size_t size,
        std::string_view kind) -> std::expected<void, std::string> {
        if (begin > size || count > size - begin) {
            return std::unexpected(
                "GraphJit Tick " + std::string(kind)
                + " capture range is out of bounds");
        }
        return {};
    };
    if (auto valid = validate_range(
            invocation_plan.sample_capture_begin,
            invocation_plan.sample_capture_count,
            background.tick_runtime.sample_captures.size(),
            "sample");
        !valid) {
        return valid;
    }
    if (auto valid = validate_range(
            invocation_plan.event_capture_begin,
            invocation_plan.event_capture_count,
            background.tick_runtime.event_captures.size(),
            "event");
        !valid) {
        return valid;
    }

    auto compact_output_index = [node, &background](
        auto const& outputs,
        TickCaptureBindingPlan const& capture,
        PortKind kind,
        std::size_t emitted_count)
        -> std::expected<std::size_t, std::string> {
        if (capture.port >= background.ports.size()) {
            return std::unexpected(
                "GraphJit Tick capture references a missing logical port");
        }
        auto const& port = background.ports[capture.port];
        auto const authored = port.configured_port.port_index;
        if (port.node != node || port.kind != kind
            || port.direction != PortDirection::output
            || !port.persisted_tick_output
            || port.retention != OutputRetention::persisted
            || authored >= outputs.size() || !outputs[authored].realtime) {
            return std::unexpected(
                "GraphJit Tick capture has no matching realtime output");
        }
        auto const compact = static_cast<std::size_t>(std::ranges::count_if(
            outputs.begin(),
            outputs.begin() + static_cast<std::ptrdiff_t>(authored),
            [](auto const& output) { return output.realtime; }));
        if (compact >= emitted_count) {
            return std::unexpected(
                "GraphJit Tick capture lost its reflected output binding");
        }
        return compact;
    };

    auto const call_offsets = tick_invocation_byte_offsets();
    auto* byte_type = llvm::Type::getInt8Ty(builder.getContext());
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(),
        static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };

    for (std::size_t offset = 0;
         offset < invocation_plan.sample_capture_count; ++offset) {
        auto const slot = invocation_plan.sample_capture_begin + offset;
        auto const& capture = background.tick_runtime.sample_captures[slot];
        auto binding = compact_output_index(
            sample_plan.outputs,
            capture,
            PortKind::sample,
            sample_bindings.output_count);
        if (!binding) return std::unexpected(std::move(binding.error()));
        if (sample_bindings.output_bindings == nullptr
            || *binding > std::numeric_limits<std::size_t>::max()
                    / sizeof(ReflectedContinuousOutputPortBinding)
            || slot > std::numeric_limits<std::size_t>::max()
                    / sizeof(TickSampleCaptureOperation)) {
            return std::unexpected(
                "GraphJit Tick sample capture binding is too large");
        }
        auto* output = builder.CreateInBoundsGEP(
            byte_type,
            sample_bindings.output_bindings,
            constant(*binding * sizeof(ReflectedContinuousOutputPortBinding)),
            "tick.capture.sample.output");
        emit_tick_capture_call(
            builder,
            invocation,
            call_offsets.sample_captures_data,
            call_offsets.sample_captures_size,
            slot,
            sizeof(TickSampleCaptureOperation),
            offsetof(TickSampleCaptureOperation, context),
            offsetof(TickSampleCaptureOperation, capture),
            output,
            sample_index,
            block_size,
            "tick.capture.sample");
    }

    for (std::size_t offset = 0;
         offset < invocation_plan.event_capture_count; ++offset) {
        auto const slot = invocation_plan.event_capture_begin + offset;
        auto const& capture = background.tick_runtime.event_captures[slot];
        auto binding = compact_output_index(
            event_plan.outputs,
            capture,
            PortKind::event,
            event_bindings.output_count);
        if (!binding) return std::unexpected(std::move(binding.error()));
        if (event_bindings.output_bindings == nullptr
            || *binding > std::numeric_limits<std::size_t>::max()
                    / sizeof(ReflectedEventOutputPortBinding)
            || slot > std::numeric_limits<std::size_t>::max()
                    / sizeof(TickEventCaptureOperation)) {
            return std::unexpected(
                "GraphJit Tick event capture binding is too large");
        }
        auto* output = builder.CreateInBoundsGEP(
            byte_type,
            event_bindings.output_bindings,
            constant(*binding * sizeof(ReflectedEventOutputPortBinding)),
            "tick.capture.event.output");
        emit_tick_capture_call(
            builder,
            invocation,
            call_offsets.event_captures_data,
            call_offsets.event_captures_size,
            slot,
            sizeof(TickEventCaptureOperation),
            offsetof(TickEventCaptureOperation, context),
            offsetof(TickEventCaptureOperation, capture),
            output,
            sample_index,
            block_size,
            "tick.capture.event");
    }
    return {};
}

std::expected<void, std::string> emit_execution_step(
    llvm::Module& module,
    llvm::IRBuilder<>& builder,
    detail::LoweringPlan const& plan,
    std::vector<EmittedNodeConfiguration> const& configurations,
    EmittedSamplePortBindings const& sample_bindings,
    EmittedEventPortBindings const& event_bindings,
    detail::PrimitiveExecutionStep const& step,
    std::vector<llvm::Value*> const& event_feedback_cursors,
    EmittedRealtimeStorage const& realtime_storage,
    ActivityEmissionCache& activity_cache,
    llvm::Value* invocation,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    bool skip,
    bool allow_primitive_slicing)
{
    if (step.configuration_index >= configurations.size()) {
        return std::unexpected(
            "GraphJit execution plan references a missing node configuration");
    }
    if (step.storage_index >= plan.declarations.primitive_storage.size()) {
        return std::unexpected(
            "GraphJit execution plan references a missing canonical storage plan");
    }
    if (step.configuration_index >= sample_bindings.primitives.size()) {
        return std::unexpected(
            "GraphJit execution plan references a missing sample-port runtime plan");
    }
    if (step.configuration_index >= event_bindings.primitives.size()) {
        return std::unexpected(
            "GraphJit execution plan references a missing event-port runtime plan");
    }
    if (step.configuration_index >= plan.imports.primitive_callbacks.size()) {
        return std::unexpected(
            "GraphJit execution plan references missing primitive callbacks");
    }
    auto const& primitive_callbacks =
        plan.imports.primitive_callbacks[step.configuration_index];
    auto mark_sample_write = [&](std::size_t representation)
        -> std::expected<void, std::string> {
        if (representation >= activity_cache.representation_epochs.size()) {
            return std::unexpected(
                "GraphJit activity cache references missing sample storage");
        }
        auto& epoch = activity_cache.representation_epochs[representation];
        if (epoch == std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(
                "GraphJit activity cache emission epoch overflows size_t");
        }
        ++epoch;
        return {};
    };
    BackgroundEvaluationPlan const* tick_background = nullptr;
    TickNodeInvocationPlan const* tick_bindings = nullptr;
    std::optional<BackgroundNodeIndex> tick_node{};
    if (step.configuration_index < plan.imports.primitive_callbacks.size()) {
        auto const bundle = plan.imports
            .primitive_callbacks[step.configuration_index].bundle;
        auto const& background = plan.connections.background;
        if (bundle < background.bundle_to_background_node.size()
            && background.bundle_to_background_node[bundle]) {
            auto const node = *background.bundle_to_background_node[bundle];
            if (node >= background.tick_runtime.nodes.size()) {
                return std::unexpected(
                    "GraphJit Tick invocation map references a missing node");
            }
            tick_background = &background;
            tick_bindings = &background.tick_runtime.nodes[node];
            tick_node = node;
            auto validate_sequential_ordinals = [&](
                std::span<TickSequentialBindingPlan const> bindings,
                std::size_t begin,
                std::size_t count,
                PortKind kind,
                std::size_t input_count)
                -> std::expected<void, std::string> {
                if (begin > bindings.size()
                    || count > bindings.size() - begin) {
                    return std::unexpected(
                        "GraphJit Tick Sequential binding range is out of bounds");
                }
                for (std::size_t slot = begin; slot < begin + count; ++slot) {
                    auto const port_index = bindings[slot].port;
                    if (port_index >= background.ports.size()) {
                        return std::unexpected(
                            "GraphJit Tick Sequential binding references a missing port");
                    }
                    auto const& port = background.ports[port_index];
                    if (port.node != node || port.kind != kind
                        || port.configured_port.port_index >= input_count) {
                        return std::unexpected(
                            "GraphJit Tick Sequential binding has no matching primitive input");
                    }
                }
                return {};
            };
            if (auto valid = validate_sequential_ordinals(
                    background.tick_runtime.sequential_sample_inputs,
                    tick_bindings->sequential_sample_begin,
                    tick_bindings->sequential_sample_count,
                    PortKind::sample,
                    sample_bindings.primitives[step.configuration_index]
                        .input_count);
                !valid) {
                return valid;
            }
            if (auto valid = validate_sequential_ordinals(
                    background.tick_runtime.sequential_event_inputs,
                    tick_bindings->sequential_event_begin,
                    tick_bindings->sequential_event_count,
                    PortKind::event,
                    event_bindings.primitives[step.configuration_index]
                        .input_count);
                !valid) {
                return valid;
            }
        }
    }

    auto event_before = emit_event_operations(
        builder,
        plan,
        step.event_operations_before,
        event_feedback_cursors,
        realtime_storage,
        root_index,
        sample_index,
        block_size);
    if (!event_before) return std::unexpected(std::move(event_before.error()));
    if (auto marked = mark_event_activity_writes(
            plan, step.event_operations_before, activity_cache);
        !marked) {
        return marked;
    }

    for (auto const carry_index : step.sample_carry_restores_before) {
        if (carry_index >= plan.sample_ports.storage.carry_operations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing sample carry restore");
        }
        auto restored = emit_sample_carry_operation(
            builder,
            plan.sample_ports.storage,
            plan.sample_ports.storage.carry_operations[carry_index],
            realtime_storage,
            root_index,
            sample_index,
            block_size,
            true);
        if (!restored) {
            return std::unexpected(std::move(restored.error()));
        }
        if (auto marked = mark_sample_write(
                plan.sample_ports.storage.carry_operations[carry_index]
                    .representation_index);
            !marked) {
            return marked;
        }
    }

    for (auto const materialization_index : step.sample_materializations_before) {
        if (materialization_index
            >= plan.sample_ports.storage.materializations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing pre-sample materialization");
        }
        auto materialized = emit_sample_materialization(
            builder,
            plan.sample_ports.storage,
            plan.sample_ports.storage.materializations[materialization_index],
            realtime_storage,
            root_index,
            sample_index,
            block_size);
        if (!materialized) {
            return std::unexpected(std::move(materialized.error()));
        }
        if (auto marked = mark_sample_write(
                plan.sample_ports.storage
                    .materializations[materialization_index]
                    .target_representation);
            !marked) {
            return marked;
        }
    }

    for (auto const composition_index : step.sample_compositions_before) {
        if (composition_index >= plan.sample_ports.storage.compositions.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing pre-sample composition");
        }
        auto composed = emit_sample_composition(
            builder,
            plan.sample_ports.storage,
            plan.sample_ports.storage.compositions[composition_index],
            realtime_storage,
            root_index,
            sample_index,
            block_size);
        if (!composed) {
            return std::unexpected(std::move(composed.error()));
        }
        if (auto marked = mark_sample_write(
                plan.sample_ports.storage.compositions[composition_index]
                    .target_representation);
            !marked) {
            return marked;
        }
    }

    detail::PrimitiveActivityPlan const* activity = nullptr;
    if (!skip) {
        if (step.configuration_index
            >= plan.declarations.primitive_activity.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing activity plan");
        }
        auto const& selected = plan.declarations
            .primitive_activity[step.configuration_index];
        if (selected) activity = &*selected;
    }
    auto const needs_skip_callback = skip || activity != nullptr;
    if (step.tick_callback_symbol.empty()
        || (needs_skip_callback && step.skip_callback_symbol.empty())) {
        return std::unexpected(
            "GraphJit execution plan references a missing primitive callback");
    }
    auto* tick_callback = module.getFunction(step.tick_callback_symbol);
    auto* skip_callback = needs_skip_callback
        ? module.getFunction(step.skip_callback_symbol)
        : nullptr;
    if (!tick_callback || tick_callback->isDeclaration()
        || (needs_skip_callback
            && (!skip_callback || skip_callback->isDeclaration()))) {
        return std::unexpected(
            "GraphJit execution plan references an unmaterialized primitive callback");
    }
    if (step.maximum_block_size == 0) {
        return std::unexpected(
            "GraphJit execution plan contains an invalid primitive maximum block size");
    }
    if (allow_primitive_slicing
        && step.maximum_block_size < plan.declarations.node_layout.max_block_size) {
        emit_sliced_primitive_calls(
            builder,
            tick_callback,
            skip_callback,
            plan.declarations,
            activity_cache,
            activity,
            skip,
            configurations[step.configuration_index],
            plan.declarations.primitive_storage[step.storage_index],
            sample_bindings.primitives[step.configuration_index],
            event_bindings.primitives[step.configuration_index],
            plan.sample_ports.primitives[step.configuration_index],
            plan.sample_ports.storage,
            plan.event_ports.primitives[step.configuration_index],
            plan.event_ports,
            tick_background,
            tick_bindings,
            invocation,
            realtime_storage,
            root_index,
            sample_index,
            block_size,
            step.maximum_block_size,
            primitive_callbacks.combine_sequential_input_values);
    } else {
        emit_primitive_call(
            builder,
            tick_callback,
            skip_callback,
            plan.declarations,
            activity_cache,
            activity,
            skip,
            configurations[step.configuration_index],
            plan.declarations.primitive_storage[step.storage_index],
            sample_bindings.primitives[step.configuration_index],
            event_bindings.primitives[step.configuration_index],
            plan.sample_ports.primitives[step.configuration_index],
            plan.sample_ports.storage,
            plan.event_ports.primitives[step.configuration_index],
            plan.event_ports,
            tick_background,
            tick_bindings,
            invocation,
            realtime_storage,
            root_index,
            sample_index,
            block_size,
            primitive_callbacks.combine_sequential_input_values);
    }
    for (auto const& output :
         plan.sample_ports.primitives[step.configuration_index].outputs) {
        if (!output.realtime || !output.storage) continue;
        if (auto marked = mark_sample_write(*output.storage); !marked) {
            return marked;
        }
    }
    for (auto const& output :
         plan.event_ports.primitives[step.configuration_index].outputs) {
        if (!output.realtime || !output.storage) continue;
        auto const representation = *output.storage;
        if (representation
            >= activity_cache.event_representation_epochs.size()) {
            return std::unexpected(
                "GraphJit activity cache references missing event output storage");
        }
        auto& epoch = activity_cache
            .event_representation_epochs[representation];
        if (epoch == std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(
                "GraphJit event activity emission epoch overflows size_t");
        }
        ++epoch;
    }

    auto event_after = emit_event_operations(
        builder,
        plan,
        step.event_operations_after,
        event_feedback_cursors,
        realtime_storage,
        root_index,
        sample_index,
        block_size);
    if (!event_after) return std::unexpected(std::move(event_after.error()));
    if (auto marked = mark_event_activity_writes(
            plan, step.event_operations_after, activity_cache);
        !marked) {
        return marked;
    }

    for (auto const materialization_index : step.sample_materializations_after) {
        if (materialization_index
            >= plan.sample_ports.storage.materializations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing sample materialization");
        }
        auto materialized = emit_sample_materialization(
            builder,
            plan.sample_ports.storage,
            plan.sample_ports.storage.materializations[materialization_index],
            realtime_storage,
            root_index,
            sample_index,
            block_size);
        if (!materialized) {
            return std::unexpected(std::move(materialized.error()));
        }
        if (auto marked = mark_sample_write(
                plan.sample_ports.storage
                    .materializations[materialization_index]
                    .target_representation);
            !marked) {
            return marked;
        }
    }

    for (auto const timeline_index : step.sample_feedback_writes_after) {
        if (timeline_index >= plan.sample_ports.storage.feedback_timelines.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing sample feedback timeline");
        }
        auto written = emit_sample_feedback_timeline_write(
            builder,
            plan.sample_ports.storage,
            plan.sample_ports.storage.feedback_timelines[timeline_index],
            realtime_storage,
            root_index,
            sample_index,
            block_size);
        if (!written) {
            return std::unexpected(std::move(written.error()));
        }
        if (auto marked = mark_sample_write(
                plan.sample_ports.storage.feedback_timelines[timeline_index]
                    .timeline_representation);
            !marked) {
            return marked;
        }
    }

    for (auto const composition_index : step.sample_compositions_after) {
        if (composition_index >= plan.sample_ports.storage.compositions.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing sample composition");
        }
        auto composed = emit_sample_composition(
            builder,
            plan.sample_ports.storage,
            plan.sample_ports.storage.compositions[composition_index],
            realtime_storage,
            root_index,
            sample_index,
            block_size);
        if (!composed) {
            return std::unexpected(std::move(composed.error()));
        }
        if (auto marked = mark_sample_write(
                plan.sample_ports.storage.compositions[composition_index]
                    .target_representation);
            !marked) {
            return marked;
        }
    }

    for (auto const materialization_index :
         step.sample_transition_materializations_after) {
        if (materialization_index
            >= plan.sample_ports.storage.materializations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing late transition materialization");
        }
        auto materialized = emit_sample_materialization(
            builder,
            plan.sample_ports.storage,
            plan.sample_ports.storage.materializations[materialization_index],
            realtime_storage,
            root_index,
            sample_index,
            block_size);
        if (!materialized) {
            return std::unexpected(std::move(materialized.error()));
        }
        if (auto marked = mark_sample_write(
                plan.sample_ports.storage
                    .materializations[materialization_index]
                    .target_representation);
            !marked) {
            return marked;
        }
    }

    for (auto const carry_index : step.sample_carry_commits_after) {
        if (carry_index >= plan.sample_ports.storage.carry_operations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing sample carry commit");
        }
        auto committed = emit_sample_carry_operation(
            builder,
            plan.sample_ports.storage,
            plan.sample_ports.storage.carry_operations[carry_index],
            realtime_storage,
            root_index,
            sample_index,
            block_size,
            false);
        if (!committed) {
            return std::unexpected(std::move(committed.error()));
        }
    }
    // Capture only after every operation assigned to this producer step has
    // finalized its authored output window. SCC execution reaches this point
    // once per semantic slice; ordinary primitive-internal slicing reaches it
    // once for the complete enclosing step.
    if (tick_background != nullptr && tick_bindings != nullptr && tick_node) {
        auto captured = emit_tick_capture_operations(
            builder,
            *tick_background,
            *tick_node,
            *tick_bindings,
            plan.sample_ports.primitives[step.configuration_index],
            sample_bindings.primitives[step.configuration_index],
            plan.event_ports.primitives[step.configuration_index],
            event_bindings.primitives[step.configuration_index],
            invocation,
            sample_index,
            block_size);
        if (!captured) {
            return std::unexpected(std::move(captured.error()));
        }
    }
    return {};
}

// Planning admits a multi-step ready group only when it can be emitted as one
// native batch; see realtime_ready_group_batches().
std::expected<void, std::string> emit_execution_batch(
    llvm::Module& module,
    llvm::IRBuilder<>& builder,
    detail::LoweringPlan const& plan,
    std::vector<EmittedNodeConfiguration> const& configurations,
    EmittedSamplePortBindings const& sample_bindings,
    EmittedEventPortBindings const& event_bindings,
    detail::ExecutionReadyGroupPlan const& group,
    EmittedRealtimeStorage const& realtime_storage,
    ActivityEmissionCache& activity_cache,
    llvm::Value* invocation,
    llvm::Value* root_index,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    bool skip)
{
    if (group.cyclic_region || group.primitive_steps.size() < 2) {
        return std::unexpected(
            "GraphJit realtime batch requires an acyclic multi-step ready group");
    }

    std::string_view tick_symbol;
    std::string_view skip_symbol;
    bool has_activity = false;
    struct LanePlan {
        detail::PrimitiveExecutionStep const* step = nullptr;
        BackgroundEvaluationPlan const* background = nullptr;
        TickNodeInvocationPlan const* tick_invocation = nullptr;
        detail::PrimitiveActivityPlan const* activity = nullptr;
    };
    std::vector<LanePlan> lanes;
    lanes.reserve(group.primitive_steps.size());
    for (auto const step_index : group.primitive_steps) {
        if (step_index >= plan.execution.primitive_steps.size()) {
            return std::unexpected(
                "GraphJit realtime batch references a missing primitive step");
        }
        auto const& step = plan.execution.primitive_steps[step_index];
        if (step.configuration_index >= configurations.size()
            || step.storage_index >= plan.declarations.primitive_storage.size()
            || step.configuration_index
                >= plan.imports.primitive_callbacks.size()
            || step.configuration_index >= sample_bindings.primitives.size()
            || step.configuration_index >= event_bindings.primitives.size()
            || step.configuration_index >= plan.sample_ports.primitives.size()
            || step.configuration_index >= plan.event_ports.primitives.size()
            || step.configuration_index
                >= plan.declarations.primitive_activity.size()) {
            return std::unexpected(
                "GraphJit realtime batch references an incomplete primitive plan");
        }
        // TTL activity is decided per lane at runtime. The lanes that tick
        // and the dormant lanes that skip are then invoked as two batches.
        detail::PrimitiveActivityPlan const* activity = nullptr;
        if (!skip) {
            auto const& selected = plan.declarations
                .primitive_activity[step.configuration_index];
            if (selected) activity = &*selected;
        }
        has_activity = has_activity || activity != nullptr;
        if (tick_symbol.empty()) {
            tick_symbol = step.tick_batch_callback_symbol;
            skip_symbol = step.skip_batch_callback_symbol;
        }

        BackgroundEvaluationPlan const* background = nullptr;
        TickNodeInvocationPlan const* tick_invocation = nullptr;
        auto const bundle = plan.imports
            .primitive_callbacks[step.configuration_index].bundle;
        auto const& background_plan = plan.connections.background;
        if (bundle < background_plan.bundle_to_background_node.size()
            && background_plan.bundle_to_background_node[bundle]) {
            auto const node =
                *background_plan.bundle_to_background_node[bundle];
            if (node >= background_plan.tick_runtime.nodes.size()) {
                return std::unexpected(
                    "GraphJit realtime batch references a missing Tick invocation");
            }
            background = &background_plan;
            tick_invocation = &background_plan.tick_runtime.nodes[node];
        }
        lanes.push_back(LanePlan{
            .step = std::addressof(step),
            .background = background,
            .tick_invocation = tick_invocation,
            .activity = activity,
        });
    }


    auto materialized_callback = [&](std::string_view symbol) -> llvm::Function* {
        auto* callback = module.getFunction(
            llvm::StringRef(symbol.data(), symbol.size()));
        return callback && !callback->isDeclaration() ? callback : nullptr;
    };
    auto* tick_callback = skip ? nullptr : materialized_callback(tick_symbol);
    auto* skip_callback = skip || has_activity
        ? materialized_callback(skip_symbol)
        : nullptr;
    if ((!skip && !tick_callback)
        || ((skip || has_activity) && !skip_callback)) {
        return std::unexpected(
            "GraphJit realtime batch references an unmaterialized callback");
    }
    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* byte_type = llvm::Type::getInt8Ty(context);
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto constant = [&](std::size_t value) {
        return llvm::ConstantInt::get(size_type, value);
    };
    if (lanes.size() > std::numeric_limits<std::size_t>::max()
            / sizeof(ReflectedNodeTickBatchLane)) {
        return std::unexpected("GraphJit realtime batch lane storage overflows size_t");
    }
    auto* lane_storage = builder.CreateAlloca(
        byte_type,
        constant(lanes.size() * sizeof(ReflectedNodeTickBatchLane)),
        "tick.batch.lanes");
    lane_storage->setAlignment(llvm::Align(alignof(ReflectedNodeTickBatchLane)));

    // Every lane decides Tick or skip before either batch runs. The lanes are
    // an antichain, so no lane observes another lane's outputs and the
    // decision matches scalar emission. Ticking lanes fill the lane array from
    // the front and skipped lanes fill it from the back. Each lane occupies
    // exactly one slot, so both batches share one N-lane array. Without
    // activity every slot and count is a compile-time constant.
    std::vector<std::optional<EmittedPrimitiveActivity>> decisions(lanes.size());
    llvm::Value* tick_count = constant(0);
    llvm::Value* skip_count = constant(0);
    for (std::size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
        auto const& lane = lanes[lane_index];
        auto const& step = *lane.step;
        auto const tick_context = emit_primitive_tick_context(
            builder,
            configurations[step.configuration_index],
            plan.declarations.primitive_storage[step.storage_index],
            sample_bindings.primitives[step.configuration_index],
            event_bindings.primitives[step.configuration_index],
            plan.sample_ports.primitives[step.configuration_index],
            plan.sample_ports.storage,
            plan.event_ports.primitives[step.configuration_index],
            plan.event_ports,
            lane.background,
            lane.tick_invocation,
            invocation,
            realtime_storage,
            root_index,
            sample_index,
            block_size,
            plan.imports.primitive_callbacks[step.configuration_index]
                .combine_sequential_input_values);
        llvm::Value* should_skip = skip
            ? llvm::ConstantInt::getTrue(context)
            : llvm::ConstantInt::getFalse(context);
        if (lane.activity != nullptr) {
            decisions[lane_index] = emit_primitive_activity_decision(
                builder,
                plan.declarations,
                activity_cache,
                *lane.activity,
                event_bindings.primitives[step.configuration_index],
                plan.sample_ports.primitives[step.configuration_index],
                plan.event_ports.primitives[step.configuration_index],
                lane.background,
                lane.tick_invocation,
                realtime_storage,
                tick_context.continuous_input_bindings,
                tick_context.event_input_bindings,
                sample_index,
                block_size);
            should_skip = decisions[lane_index]->should_skip;
        }
        auto* slot = builder.CreateSelect(
            should_skip,
            builder.CreateSub(
                constant(lanes.size() - 1), skip_count, "tick.batch.skip.slot"),
            tick_count,
            "tick.batch.slot");
        auto* lane_pointer = builder.CreateInBoundsGEP(
            byte_type,
            lane_storage,
            builder.CreateMul(
                slot,
                constant(sizeof(ReflectedNodeTickBatchLane)),
                "tick.batch.lane.offset"),
            "tick.batch.lane");
        auto* skipped = builder.CreateZExt(
            should_skip, size_type, "tick.batch.skipped");
        skip_count = builder.CreateAdd(
            skip_count, skipped, "tick.batch.skip.count");
        tick_count = builder.CreateAdd(
            tick_count,
            builder.CreateSub(constant(1), skipped, "tick.batch.ticked"),
            "tick.batch.tick.count");

        auto* node_pointer = byte_offset_pointer(
            builder,
            lane_pointer,
            offsetof(ReflectedNodeTickBatchLane, node_data),
            "tick.batch.node.pointer");
        builder.CreateStore(
            configurations[step.configuration_index].node_config,
            node_pointer);
        auto* context_pointer = byte_offset_pointer(
            builder,
            lane_pointer,
            offsetof(ReflectedNodeTickBatchLane, context),
            "tick.batch.context.pointer");
        builder.CreateMemCpy(
            context_pointer,
            llvm::Align(alignof(ReflectedNodeTickContext)),
            tick_context.storage,
            llvm::Align(alignof(ReflectedNodeTickContext)),
            sizeof(ReflectedNodeTickContext));
        auto* index_pointer = byte_offset_pointer(
            builder,
            lane_pointer,
            offsetof(ReflectedNodeTickBatchLane, index),
            "tick.batch.index.pointer");
        builder.CreateStore(sample_index, index_pointer);
        auto* size_pointer = byte_offset_pointer(
            builder,
            lane_pointer,
            offsetof(ReflectedNodeTickBatchLane, block_size),
            "tick.batch.size.pointer");
        builder.CreateStore(block_size, size_pointer);
    }

    auto emit_batch_call = [&](llvm::Function* callback,
                               llvm::Value* first_slot,
                               llvm::Value* count,
                               llvm::Twine const& name) {
        auto* constant_count = llvm::dyn_cast<llvm::ConstantInt>(count);
        if (constant_count && constant_count->isZero()) return;
        auto* batch_context = builder.CreateAlloca(
            byte_type,
            constant(sizeof(ReflectedNodeTickBatchContext)),
            name + ".context");
        batch_context->setAlignment(
            llvm::Align(alignof(ReflectedNodeTickBatchContext)));
        llvm::BasicBlock* complete = nullptr;
        if (!constant_count) {
            auto* invoke = llvm::BasicBlock::Create(
                context, name + ".invoke", function);
            complete = llvm::BasicBlock::Create(
                context, name + ".complete", function);
            builder.CreateCondBr(
                builder.CreateICmpNE(count, constant(0), name + ".nonempty"),
                invoke,
                complete);
            builder.SetInsertPoint(invoke);
        }
        store_context_span_pointer(
            builder,
            batch_context,
            offsetof(ReflectedNodeTickBatchContext, lanes)
                + offsetof(ReflectedSpan<ReflectedNodeTickBatchLane const>, pointer),
            offsetof(ReflectedNodeTickBatchContext, lanes)
                + offsetof(ReflectedSpan<ReflectedNodeTickBatchLane const>, extent),
            builder.CreateInBoundsGEP(
                byte_type,
                lane_storage,
                builder.CreateMul(
                    first_slot,
                    constant(sizeof(ReflectedNodeTickBatchLane)),
                    name + ".lanes.offset"),
                name + ".lanes"),
            0);
        builder.CreateStore(
            count,
            byte_offset_pointer(
                builder,
                batch_context,
                offsetof(ReflectedNodeTickBatchContext, lanes)
                    + offsetof(
                        ReflectedSpan<ReflectedNodeTickBatchLane const>,
                        extent),
                name + ".extent.pointer"));
        auto* call = builder.CreateCall(callback, {batch_context});
        call->setCallingConv(callback->getCallingConv());
        if (complete) {
            builder.CreateBr(complete);
            builder.SetInsertPoint(complete);
        }
    };
    if (tick_callback) {
        emit_batch_call(tick_callback, constant(0), tick_count, "tick.batch");
    }
    if (skip_callback) {
        emit_batch_call(
            skip_callback,
            builder.CreateSub(
                constant(lanes.size()), skip_count, "skip.batch.first"),
            skip_count,
            "skip.batch");
    }

    // A ticked lane finalizes its activity state from the outputs written by
    // the Tick batch. A skipped lane keeps its dormant state, as in scalar
    // emission.
    for (std::size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
        auto const& decision = decisions[lane_index];
        if (!decision) continue;
        auto const& lane = lanes[lane_index];
        auto const configuration_index = lane.step->configuration_index;
        auto* update = llvm::BasicBlock::Create(
            context, "tick.batch.activity.update", function);
        auto* next = llvm::BasicBlock::Create(
            context, "tick.batch.activity.next", function);
        builder.CreateCondBr(decision->should_skip, next, update);
        builder.SetInsertPoint(update);
        emit_primitive_activity_update(
            builder,
            *lane.activity,
            *decision,
            sample_bindings.primitives[configuration_index],
            event_bindings.primitives[configuration_index],
            plan.sample_ports.primitives[configuration_index],
            sample_index,
            block_size);
        builder.CreateBr(next);
        builder.SetInsertPoint(next);
    }

    // Keep compile-time activity-cache epochs identical to scalar emission.
    // The selected conservative groups have no generated post-operations or
    // capture hooks, so the Tick and skip batches are the only runtime
    // producer actions.
    for (auto const& lane : lanes) {
        auto const configuration_index = lane.step->configuration_index;
        for (auto const& output :
             plan.sample_ports.primitives[configuration_index].outputs) {
            if (!output.realtime || !output.storage) continue;
            if (*output.storage >= activity_cache.representation_epochs.size()) {
                return std::unexpected(
                    "GraphJit realtime batch sample output references a missing activity epoch");
            }
            auto& epoch = activity_cache.representation_epochs[*output.storage];
            if (epoch == std::numeric_limits<std::size_t>::max()) {
                return std::unexpected(
                    "GraphJit realtime batch sample activity epoch overflows size_t");
            }
            ++epoch;
        }
        for (auto const& output :
             plan.event_ports.primitives[configuration_index].outputs) {
            if (!output.realtime || !output.storage) continue;
            if (*output.storage
                >= activity_cache.event_representation_epochs.size()) {
                return std::unexpected(
                    "GraphJit realtime batch event output references a missing activity epoch");
            }
            auto& epoch =
                activity_cache.event_representation_epochs[*output.storage];
            if (epoch == std::numeric_limits<std::size_t>::max()) {
                return std::unexpected(
                    "GraphJit realtime batch event activity epoch overflows size_t");
            }
            ++epoch;
        }
    }
    return {};
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
    auto* invocation = &*arguments++;
    auto* sample_index = &*arguments++;
    auto* block_size = &*arguments;
    storage_base->setName("storage_base");
    invocation->setName("invocation");
    sample_index->setName("sample_index");
    block_size->setName("block_size");

    auto* entry = llvm::BasicBlock::Create(module.getContext(), "entry", function);
    llvm::IRBuilder<> builder(entry);

    auto* size_type = llvm::IntegerType::get(
        module.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto allocate_arena = [&](std::size_t size,
                              std::size_t alignment,
                              llvm::Twine const& name) -> llvm::Value* {
        if (size == 0) return nullptr;
        auto* arena = builder.CreateAlloca(
            llvm::Type::getInt8Ty(module.getContext()),
            llvm::ConstantInt::get(size_type, size),
            name);
        arena->setAlignment(llvm::Align(alignment));
        return arena;
    };

    auto* transient_stack = allocate_arena(
        plan.root_stack.size_bytes,
        plan.root_stack.alignment,
        "realtime.stack");
    auto stack_pointer = [&](std::size_t offset,
                             std::size_t size,
                             llvm::Twine const& name) -> llvm::Value* {
        if (size == 0) return nullptr;
        if (transient_stack == nullptr
            || offset > plan.root_stack.size_bytes
            || size > plan.root_stack.size_bytes - offset) {
            return nullptr;
        }
        if (offset == 0) return transient_stack;
        return builder.CreateInBoundsGEP(
            llvm::Type::getInt8Ty(module.getContext()),
            transient_stack,
            {llvm::ConstantInt::get(size_type, offset)},
            name);
    };

    EmittedRealtimeStorage realtime_storage{
        .persistent_base = storage_base,
        .sample_transient_base = stack_pointer(
            plan.root_stack.sample_offset,
            plan.sample_ports.storage.transient_arena_size,
            "sample.stack.base"),
        .event_transient_base = stack_pointer(
            plan.root_stack.event_offset,
            plan.event_ports.transient_arena_size,
            "event.stack.base"),
    };
    realtime_storage.sample_representations.resize(
        plan.sample_ports.storage.representations.size());
    for (std::size_t representation_index = 0;
         representation_index
            < plan.sample_ports.storage.representations.size();
         ++representation_index) {
        auto const& storage =
            plan.sample_ports.storage.representations[representation_index];
        if (storage.constant_value) {
            auto const channels = channel_count(storage.channel_layout);
            if (channels == 0
                || storage.frame_capacity
                    > std::numeric_limits<std::size_t>::max() / channels
                || storage.constant_value->size()
                    != storage.geometry.value_size_bytes
                || channels * storage.frame_capacity
                    > std::numeric_limits<std::size_t>::max()
                        / storage.geometry.value_size_bytes) {
                return std::unexpected(
                    "GraphJit disconnected continuous input has invalid storage geometry");
            }
            auto const value_count =
                channels * storage.frame_capacity;
            std::vector<std::byte> values(
                value_count * storage.geometry.value_size_bytes);
            for (std::size_t value = 0; value < value_count; ++value) {
                std::memcpy(
                    values.data() + value * storage.geometry.value_size_bytes,
                    storage.constant_value->data(),
                    storage.geometry.value_size_bytes);
            }
            realtime_storage.sample_representations[representation_index] =
                immutable_bytes_global(
                    module,
                    values.data(),
                    values.size(),
                    storage.geometry.element_alignment,
                    "__iv_graph_continuous_default_"
                        + std::to_string(representation_index));
        } else if (storage.transient_allocation
                   != detail::no_sample_transient_allocation) {
            if (realtime_storage.sample_transient_base == nullptr
                || storage.transient_allocation
                    >= plan.sample_ports.storage.transient_allocations.size()) {
                return std::unexpected(
                    "GraphJit sample storage lost its transient root storage");
            }
            auto const& allocation = plan.sample_ports.storage
                .transient_allocations[storage.transient_allocation];
            realtime_storage.sample_representations[representation_index] =
                byte_offset_pointer(
                    builder,
                    realtime_storage.sample_transient_base,
                    allocation.region_relative_offset,
                    "sample.storage." + std::to_string(representation_index));
        } else if (storage.persistent_allocation
                   != detail::no_sample_persistent_allocation) {
            if (storage.persistent_allocation
                >= plan.sample_ports.storage.persistent_allocations.size()) {
                return std::unexpected(
                    "GraphJit sample storage lost its persistent storage");
            }
            auto const& allocation = plan.sample_ports.storage
                .persistent_allocations[storage.persistent_allocation];
            realtime_storage.sample_representations[representation_index] =
                byte_offset_pointer(
                    builder,
                    storage_base,
                    allocation.storage_offset,
                    "sample.storage." + std::to_string(representation_index));
        } else {
            return std::unexpected(
                "GraphJit sample storage has no resolved storage");
        }
    }

    realtime_storage.event_representations.resize(
        plan.event_ports.ports.size());
    realtime_storage.event_overflow_counts.resize(
        plan.event_ports.ports.size());
    for (std::size_t representation_index = 0;
         representation_index < plan.event_ports.ports.size();
         ++representation_index) {
        auto const& storage =
            plan.event_ports.ports[representation_index];
        if (storage.immutable_empty) {
            std::vector<std::byte> const empty(storage.size_bytes);
            realtime_storage.event_representations[representation_index] =
                immutable_bytes_global(
                    module,
                    empty.data(),
                    empty.size(),
                    storage.alignment,
                    "__iv_graph_event_empty_"
                        + std::to_string(representation_index));
        } else if (storage.persistent) {
            realtime_storage.event_representations[representation_index] =
                byte_offset_pointer(
                    builder,
                    storage_base,
                    storage.storage_offset,
                    "event.storage." + std::to_string(representation_index));
        } else {
            if (realtime_storage.event_transient_base == nullptr
                || storage.transient_allocation
                    >= plan.event_ports.transient_allocations.size()) {
                return std::unexpected(
                    "GraphJit event storage lost its transient root storage");
            }
            auto const& allocation = plan.event_ports.transient_allocations[
                storage.transient_allocation];
            realtime_storage.event_representations[representation_index] =
                byte_offset_pointer(
                    builder,
                    realtime_storage.event_transient_base,
                    allocation.region_relative_offset,
                    "event.storage." + std::to_string(representation_index));
        }
        if (storage.has_producer_overflow_counter) {
            realtime_storage.event_overflow_counts[representation_index] =
                byte_offset_pointer(
                    builder,
                    storage_base,
                    storage.overflow_count_storage_offset,
                    "event.overflow." + std::to_string(representation_index));
        }
    }

    realtime_storage.producer_authored_extents.reserve(
        plan.sample_ports.storage.producer_authored_extents.size());
    for (std::size_t extent_index = 0;
         extent_index
            < plan.sample_ports.storage.producer_authored_extents.size();
         ++extent_index) {
        auto const& extent = plan.sample_ports.storage
            .producer_authored_extents[extent_index];
        auto* pointer = byte_offset_pointer(
            builder,
            storage_base,
            extent.storage_offset,
            "continuous.authored.extent."
                + std::to_string(extent_index));
        realtime_storage.producer_authored_extents.push_back(
            builder.CreateLoad(
                size_type,
                pointer,
                "continuous.authored.extent.root."
                    + std::to_string(extent_index)));
    }
    realtime_storage.sequential_pace_cursors.reserve(
        plan.sample_ports.storage.sequential_pace_cursors.size());
    for (std::size_t cursor_index = 0;
         cursor_index < plan.sample_ports.storage.sequential_pace_cursors.size();
         ++cursor_index) {
        auto const& cursor = plan.sample_ports.storage
            .sequential_pace_cursors[cursor_index];
        auto* pointer = byte_offset_pointer(
            builder,
            storage_base,
            cursor.storage_offset,
            "continuous.sequential.cursor."
                + std::to_string(cursor_index));
        realtime_storage.sequential_pace_cursors.push_back(
            builder.CreateLoad(
                size_type,
                pointer,
                "continuous.sequential.cursor.root."
                    + std::to_string(cursor_index)));
    }

    auto sample_bindings = emit_sample_port_bindings(
        builder, module, plan.sample_ports, realtime_storage);
    if (!sample_bindings) {
        return std::unexpected(std::move(sample_bindings.error()));
    }
    auto event_bindings = emit_event_port_bindings(
        builder, module, plan.event_ports, realtime_storage);
    if (!event_bindings) {
        return std::unexpected(std::move(event_bindings.error()));
    }

    auto* feedback_cursor_type = llvm::IntegerType::get(
        module.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* feedback_cursor_zero = llvm::ConstantInt::get(feedback_cursor_type, 0);
    std::vector<llvm::Value*> event_feedback_cursors;
    event_feedback_cursors.reserve(plan.event_ports.feedback_operations.size());
    for (std::size_t i = 0; i < plan.event_ports.feedback_operations.size(); ++i) {
        auto const& feedback = plan.event_ports.feedback_operations[i];
        if (feedback.source_representation
            >= plan.event_ports.ports.size()) {
            return std::unexpected(
                "GraphJit event feedback cursor references a missing source storage");
        }
        auto const& source =
            plan.event_ports.ports[feedback.source_representation];
        if (feedback.source_sequence_restarts_each_append) {
            event_feedback_cursors.push_back(nullptr);
            continue;
        }
        auto* cursor = builder.CreateAlloca(
            feedback_cursor_type, nullptr,
            "event.feedback.cursor." + std::to_string(i));
        llvm::Value* initial_cursor = feedback_cursor_zero;
        if (source.persistent_ring) {
            if (!source.region.valid()) {
                return std::unexpected(
                    "GraphJit persistent event feedback source has no storage region");
            }
            auto* source_write_pointer = byte_offset_pointer(
                builder,
                realtime_storage.event_representations[
                    feedback.source_representation],
                source.write_index_relative_offset,
                "event.feedback.cursor.source.write." + std::to_string(i));
            initial_cursor = builder.CreateLoad(
                feedback_cursor_type,
                source_write_pointer,
                "event.feedback.cursor.initial." + std::to_string(i));
        }
        builder.CreateStore(initial_cursor, cursor);
        event_feedback_cursors.push_back(cursor);
    }

    ActivityEmissionCache activity_cache{
        .representation_epochs = std::vector<std::size_t>(
            plan.sample_ports.storage.representations.size(), 0),
        .event_representation_epochs = std::vector<std::size_t>(
            plan.event_ports.ports.size(), 0),
    };

    for (std::size_t group_index = 0;
         group_index < plan.execution.ready_groups.size(); ++group_index) {
        auto const& group = plan.execution.ready_groups[group_index];
        if (!group.cyclic_region && group.primitive_steps.size() > 1) {
            auto batched = emit_execution_batch(
                module,
                builder,
                plan,
                configurations,
                *sample_bindings,
                *event_bindings,
                group,
                realtime_storage,
                activity_cache,
                invocation,
                sample_index,
                sample_index,
                block_size,
                skip);
            if (!batched) {
                return std::unexpected(std::move(batched.error()));
            }
            continue;
        }
        if (!group.cyclic_region) {
            for (auto const step_index : group.primitive_steps) {
                if (step_index >= plan.execution.primitive_steps.size()) {
                    return std::unexpected(
                        "GraphJit ready group references a missing primitive step");
                }
                auto emitted = emit_execution_step(
                    module,
                    builder,
                    plan,
                    configurations,
                    *sample_bindings,
                    *event_bindings,
                    plan.execution.primitive_steps[step_index],
                    event_feedback_cursors,
                    realtime_storage,
                    activity_cache,
                    invocation,
                    sample_index,
                    sample_index,
                    block_size,
                    skip,
                    true);
                if (!emitted) {
                    return std::unexpected(std::move(emitted.error()));
                }
            }
            continue;
        }

        auto const region_index = *group.cyclic_region;
        if (region_index >= plan.execution.regions.size()) {
            return std::unexpected(
                "GraphJit ready group references a missing cyclic region");
        }
        auto const& region = plan.execution.regions[region_index];
        if (!region.cyclic || !group.primitive_steps.empty()) {
            return std::unexpected(
                "GraphJit cyclic ready group has invalid execution contents");
        }

        if (region.maximum_block_size == 0
            || region.scc_feedback_latency == 0) {
            return std::unexpected(
                "GraphJit cyclic execution region has invalid slice semantics");
        }

        auto region_before = emit_event_operations(
            builder,
            plan,
            region.event_operations_before,
            event_feedback_cursors,
            realtime_storage,
            sample_index,
            sample_index,
            block_size);
        if (!region_before) {
            return std::unexpected(std::move(region_before.error()));
        }
        if (auto marked = mark_event_activity_writes(
                plan, region.event_operations_before, activity_cache);
            !marked) {
            return std::unexpected(std::move(marked.error()));
        }

        auto& context = module.getContext();
        auto* size_type = llvm::IntegerType::get(
            context, static_cast<unsigned>(sizeof(std::size_t) * 8));
        auto* zero = llvm::ConstantInt::get(size_type, 0);
        auto* quantum = llvm::ConstantInt::get(
            size_type, region.maximum_block_size);
        auto* preheader = builder.GetInsertBlock();
        auto* loop = llvm::BasicBlock::Create(
            context,
            "scc." + std::to_string(region_index) + ".slice",
            function);
        auto* exit = llvm::BasicBlock::Create(
            context,
            "scc." + std::to_string(region_index) + ".end",
            function);
        auto* nonempty = builder.CreateICmpNE(
            block_size, zero, "scc.slice.nonempty");
        builder.CreateCondBr(nonempty, loop, exit);

        builder.SetInsertPoint(loop);
        auto* offset = builder.CreatePHI(size_type, 2, "scc.slice.offset");
        offset->addIncoming(zero, preheader);
        auto* remaining = builder.CreateSub(
            block_size, offset, "scc.slice.remaining");
        auto* tail = builder.CreateICmpULT(
            remaining, quantum, "scc.slice.tail");
        auto* slice_size = builder.CreateSelect(
            tail, remaining, quantum, "scc.slice.size");
        auto* slice_index = builder.CreateAdd(
            sample_index, offset, "scc.slice.index");

        for (auto const step_index : region.primitive_steps) {
            auto const& step = plan.execution.primitive_steps[step_index];
            if (step.maximum_block_size < region.maximum_block_size) {
                return std::unexpected(
                    "GraphJit SCC quantum exceeds a primitive maximum block size");
            }
            auto emitted = emit_execution_step(
                module,
                builder,
                plan,
                configurations,
                *sample_bindings,
                *event_bindings,
                step,
                event_feedback_cursors,
                realtime_storage,
                activity_cache,
                invocation,
                sample_index,
                slice_index,
                slice_size,
                skip,
                false);
            if (!emitted) {
                return std::unexpected(std::move(emitted.error()));
            }
        }

        auto* next_offset = builder.CreateAdd(
            offset, slice_size, "scc.slice.next");
        auto* done = builder.CreateICmpUGE(
            next_offset, block_size, "scc.slice.done");
        builder.CreateCondBr(done, exit, loop);
        offset->addIncoming(next_offset, builder.GetInsertBlock());
        builder.SetInsertPoint(exit);

        auto region_after = emit_event_operations(
            builder,
            plan,
            region.event_operations_after,
            event_feedback_cursors,
            realtime_storage,
            sample_index,
            sample_index,
            block_size);
        if (!region_after) {
            return std::unexpected(std::move(region_after.error()));
        }
        if (auto marked = mark_event_activity_writes(
                plan, region.event_operations_after, activity_cache);
            !marked) {
            return std::unexpected(std::move(marked.error()));
        }
    }

    for (std::size_t extent_index = 0;
         extent_index
            < plan.sample_ports.storage.producer_authored_extents.size();
         ++extent_index) {
        auto const& extent = plan.sample_ports.storage
            .producer_authored_extents[extent_index];
        auto* pointer = byte_offset_pointer(
            builder,
            storage_base,
            extent.storage_offset,
            "continuous.authored.extent");
        auto* maximum = llvm::ConstantInt::get(
            size_type, extent.maximum_revision_frames);
        auto* current = emit_unsigned_min(
            builder,
            realtime_storage.producer_authored_extents[extent_index],
            maximum,
            "continuous.authored.extent.current");
        auto const projected = emit_lattice_projection(
            builder,
            sample_index,
            block_size,
            extent.absolute_pace_exponent,
            "continuous.authored.extent.advance");
        auto* remaining = builder.CreateSub(
            maximum,
            current,
            "continuous.authored.extent.remaining");
        auto* increment = emit_unsigned_min(
            builder,
            projected.value_count,
            remaining,
            "continuous.authored.extent.increment");
        builder.CreateStore(
            builder.CreateAdd(
                current,
                increment,
                "continuous.authored.extent.next"),
            pointer);
    }

    for (std::size_t cursor_index = 0;
         cursor_index < plan.sample_ports.storage.sequential_pace_cursors.size();
         ++cursor_index) {
        auto const& cursor = plan.sample_ports.storage
            .sequential_pace_cursors[cursor_index];
        auto* pointer = byte_offset_pointer(
            builder,
            storage_base,
            cursor.storage_offset,
            "continuous.sequential.cursor");
        auto* current = realtime_storage.sequential_pace_cursors[cursor_index];
        auto const projected = emit_lattice_projection(
            builder,
            sample_index,
            block_size,
            cursor.absolute_pace_exponent,
            "continuous.sequential.cursor.advance");
        builder.CreateStore(
            builder.CreateAdd(
                current,
                projected.value_count,
                "continuous.sequential.cursor.next"),
            pointer);
    }

    builder.CreateRetVoid();
    return function;
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
        if (!imported || imported->isDeclaration()) {
            return std::unexpected(
                "GraphJit background batch references an unmaterialized callback");
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
        auto const output_window = requirement.direction
                == PortDirection::output
            && (requirement.role
                    == RealtimePortStateRole::tick_output_history
                || requirement.role
                    == RealtimePortStateRole::tick_output_latency);
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
                    != SampleRealtimePortStateAccess::composed)) {
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
        if (requirement.configured_port.node_bundle_handle
            >= transition_plan.connections.schedule
                .bundle_execution_position.size()) {
            return std::unexpected(
                "GraphJit transition suffix lost its target execution position");
        }
        auto const target_position = transition_plan.connections.schedule
            .bundle_execution_position[
                requirement.configured_port.node_bundle_handle];
        std::optional<std::size_t> source_position;
        bool source_after_composition = false;
        bool source_before_consumer_materialization = false;
        if (output_window) {
            source_position = target_position;
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
                source_after_composition = true;
            } else {
                if (source.producer_group_index
                        >= transition_plan.connections.sample_producer_groups.size()) {
                    return std::unexpected(
                        "GraphJit transition input suffix lost its producer group");
                }
                auto const& producer_group = transition_plan.connections
                    .sample_producer_groups[source.producer_group_index];
                if (!producer_group.source_port
                    || producer_group.source_port->node_bundle_handle
                        >= transition_plan.connections.schedule
                            .bundle_execution_position.size()) {
                    return std::unexpected(
                        "GraphJit transition suffix lost its source producer");
                }
                source_position = transition_plan.connections.schedule
                    .bundle_execution_position[
                        producer_group.source_port->node_bundle_handle];
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
            if (source_before_consumer_materialization) {
                mirror.before_execution_position = *source_position;
            } else {
                mirror.after_execution_position = *source_position;
            }
            if (output_window) {
                mirror.retained_before = target_ports.outputs[
                    requirement.configured_port.port_index].latency;
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
            if (source_is_feedback_timeline || source_after_composition) {
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
            && (requirement.role
                    == RealtimePortStateRole::tick_output_history
                || requirement.role
                    == RealtimePortStateRole::tick_output_latency);
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
    auto imported = emit_package_imports(input, plan.imports, output_module);
    if (!imported) return std::unexpected(std::move(imported.error()));

    auto configurations =
        emit_node_configurations(plan.configurations, output_module);
    if (!configurations) {
        return std::unexpected(std::move(configurations.error()));
    }

    if (transition
        && (!transition->output.state_transition.sample_expiry_cursors.empty()
            || transition->output.state_transition.event_expiry_samples != 0)) {
        auto transition_tick = define_root_operation(
            output_module,
            root_transition_tick_block_symbol,
            transition->root_plan,
            *configurations,
            false);
        if (!transition_tick) {
            return std::unexpected(std::move(transition_tick.error()));
        }
    }

    auto tick = define_root_operation(
        output_module,
        root_tick_block_symbol,
        plan,
        *configurations,
        false);
    if (!tick) return std::unexpected(std::move(tick.error()));

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

    auto const has_transition_root = transition
        && (!transition->output.state_transition.sample_expiry_cursors.empty()
            || transition->output.state_transition.event_expiry_samples != 0);
    return LoweringOutput{
        .port_realization = std::move(plan.port_realization),
        .node_layout = std::move(plan.declarations.node_layout),
        .tock_state_layout = std::move(plan.declarations.tock_state_layout),
        .background_evaluation_plan = std::move(plan.connections.background),
        .realtime_port_state_requirements =
            std::move(plan.connections.realtime_port_states),
        .realtime_port_state_realizations =
            std::move(plan.realtime_port_state_realizations),
        .transition = transition
            ? std::optional<LoweredGraphTransition>{
                std::move(transition->output)}
            : std::nullopt,
        .root_symbols = {
            .tick_block = std::string(root_tick_block_symbol),
            .transition_tick_block = has_transition_root
                ? std::string(root_transition_tick_block_symbol)
                : std::string{},
            .propagate_background_forward = has_background_program
                ? std::string(root_background_forward_symbol)
                : std::string{},
            .propagate_background_reverse = has_background_program
                ? std::string(root_background_reverse_symbol)
                : std::string{},
            .evaluate_background = has_background_program
                ? std::string(root_background_evaluate_symbol)
                : std::string{},
        },
    };
}
} // namespace

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput& input,
    llvm::Module& output_module)
{
    if (output_module.getNamedValue(root_tick_block_symbol)
        || output_module.getNamedValue(root_transition_tick_block_symbol)
        || output_module.getNamedValue(root_background_forward_symbol)
        || output_module.getNamedValue(root_background_reverse_symbol)
        || output_module.getNamedValue(root_background_evaluate_symbol)) {
        return std::unexpected(
            "ConfiguredGraph -> LLVM IR lowering output module already contains reserved root symbols");
    }

    // Host-side graph/declaration/import/configuration/execution planning must
    // succeed before output LLVM is mutated or a compile-local package module is
    // consumed. The emitter then realizes that immutable plan in one direction.
    auto plan = detail::build_lowering_plan(input);
    if (!plan) return std::unexpected(std::move(plan.error()));

    auto transition = specialize_transition_plan(*plan, input.transition);
    if (!transition) return std::unexpected(std::move(transition.error()));

    return emit_lowering_plan(
        input, std::move(*plan), std::move(*transition), output_module);
}
} // namespace iv::graph_jit
