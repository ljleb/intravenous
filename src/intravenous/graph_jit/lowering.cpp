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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv::graph_jit {
namespace {
constexpr std::string_view root_tick_block_symbol = "__iv_graph_root_tick_block";
constexpr std::string_view root_skip_block_symbol = "__iv_graph_root_skip_block";

struct ReflectedContextByteOffsets {
    std::size_t sample_storage_base = 0;
    std::size_t sample_input_bindings_data = 0;
    std::size_t sample_input_bindings_size = 0;
    std::size_t sample_output_bindings_data = 0;
    std::size_t sample_output_bindings_size = 0;
    std::size_t event_storage_base = 0;
    std::size_t event_input_bindings_data = 0;
    std::size_t event_input_bindings_size = 0;
    std::size_t event_output_bindings_data = 0;
    std::size_t event_output_bindings_size = 0;
    std::size_t compiled_state_data = 0;
    std::size_t compiled_state_size = 0;
    std::size_t state_data = 0;
    std::size_t state_size = 0;
};

struct EmittedNodeConfiguration {
    llvm::GlobalVariable* node_config = nullptr;
    llvm::GlobalVariable* tick_context_template = nullptr;
};

struct EmittedPrimitiveSamplePorts {
    llvm::GlobalVariable* input_bindings = nullptr;
    std::size_t input_count = 0;
    llvm::GlobalVariable* output_bindings = nullptr;
    std::size_t output_count = 0;
};

struct EmittedSamplePortBindings {
    std::vector<EmittedPrimitiveSamplePorts> primitives{};
};

struct EmittedPrimitiveEventPorts {
    llvm::GlobalVariable* input_bindings = nullptr;
    std::size_t input_count = 0;
    llvm::GlobalVariable* output_bindings = nullptr;
    std::size_t output_count = 0;
};

struct EmittedEventPortBindings {
    std::vector<EmittedPrimitiveEventPorts> primitives{};
};

constexpr ReflectedContextByteOffsets reflected_context_byte_offsets() noexcept
{
    return {
        .sample_storage_base = offsetof(ReflectedNodeTickContext, sample_storage_base),
        .sample_input_bindings_data =
            offsetof(ReflectedNodeTickContext, sample_input_bindings)
            + offsetof(ReflectedSpan<ReflectedSampleInputPortBinding const>, pointer),
        .sample_input_bindings_size =
            offsetof(ReflectedNodeTickContext, sample_input_bindings)
            + offsetof(ReflectedSpan<ReflectedSampleInputPortBinding const>, extent),
        .sample_output_bindings_data =
            offsetof(ReflectedNodeTickContext, sample_output_bindings)
            + offsetof(ReflectedSpan<ReflectedSampleOutputPortBinding const>, pointer),
        .sample_output_bindings_size =
            offsetof(ReflectedNodeTickContext, sample_output_bindings)
            + offsetof(ReflectedSpan<ReflectedSampleOutputPortBinding const>, extent),
        .event_storage_base = offsetof(ReflectedNodeTickContext, event_storage_base),
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

std::expected<ReflectedSamplePortStorageBinding, std::string>
sample_storage_binding(
    detail::SamplePhysicalPlan const& plan,
    std::size_t representation_index)
{
    if (representation_index >= plan.representations.size()) {
        return std::unexpected(
            "GraphJit sample binding references a missing physical representation");
    }
    auto const& representation = plan.representations[representation_index];

    std::optional<std::size_t> storage_offset;
    if (representation.transient_allocation
        != detail::no_sample_transient_allocation) {
        if (representation.transient_allocation >= plan.transient_allocations.size()) {
            return std::unexpected(
                "GraphJit sample representation references a missing transient allocation");
        }
        auto const& allocation =
            plan.transient_allocations[representation.transient_allocation];
        if (allocation.representation_index != representation_index) {
            return std::unexpected(
                "GraphJit sample transient allocation points at the wrong representation");
        }
        storage_offset = allocation.storage_offset;
    } else if (representation.persistent_allocation
               != detail::no_sample_persistent_allocation) {
        if (representation.persistent_allocation >= plan.persistent_allocations.size()) {
            return std::unexpected(
                "GraphJit sample representation references a missing persistent allocation");
        }
        auto const& allocation =
            plan.persistent_allocations[representation.persistent_allocation];
        if (allocation.representation_index != representation_index) {
            return std::unexpected(
                "GraphJit sample persistent allocation points at the wrong representation");
        }
        if (allocation.kind != detail::SamplePersistentStorageKind::ring) {
            return std::unexpected(
                "GraphJit compact sample carry must bind through its transient working representation");
        }
        storage_offset = allocation.storage_offset;
    }

    if (!storage_offset) {
        return std::unexpected(
            "GraphJit sample representation has no realized physical storage");
    }
    return ReflectedSamplePortStorageBinding{
        .storage_offset = *storage_offset,
        .frame_capacity = representation.frame_capacity,
        .storage_latency = 0,
        .channel_layout = representation.channel_layout,
    };
}

std::expected<EmittedSamplePortBindings, std::string> emit_sample_port_bindings(
    llvm::Module& module,
    detail::SamplePortBindingPlan const& plan)
{
    EmittedSamplePortBindings emitted;
    emitted.primitives.resize(plan.primitives.size());

    for (std::size_t primitive_index = 0;
         primitive_index < plan.primitives.size();
         ++primitive_index) {
        auto const& primitive = plan.primitives[primitive_index];
        auto& result = emitted.primitives[primitive_index];
        result.input_count = primitive.inputs.size();
        result.output_count = primitive.outputs.size();

        if (!primitive.inputs.empty()) {
            std::vector<ReflectedSampleInputPortBinding> bindings;
            bindings.reserve(primitive.inputs.size());
            for (auto const& input : primitive.inputs) {
                if (!input.representation) {
                    return std::unexpected(
                        "GraphJit sample input binding has no physical representation");
                }
                auto storage = sample_storage_binding(
                    plan.physical, *input.representation);
                if (!storage) return std::unexpected(std::move(storage.error()));
                bindings.push_back(ReflectedSampleInputPortBinding{
                    .storage = *storage,
                    .history = input.history,
                    .read_latency = input.read_latency,
                });
            }
            result.input_bindings = immutable_bytes_global(
                module,
                bindings.data(),
                bindings.size() * sizeof(ReflectedSampleInputPortBinding),
                alignof(ReflectedSampleInputPortBinding),
                "__iv_graph_sample_inputs_" + std::to_string(primitive_index));
        }

        if (!primitive.outputs.empty()) {
            std::vector<ReflectedSampleOutputPortBinding> bindings;
            bindings.reserve(primitive.outputs.size());
            for (auto const& output : primitive.outputs) {
                if (!output.representation) {
                    return std::unexpected(
                        "GraphJit sample output binding has no physical representation");
                }
                auto storage = sample_storage_binding(
                    plan.physical, *output.representation);
                if (!storage) return std::unexpected(std::move(storage.error()));
                bindings.push_back(ReflectedSampleOutputPortBinding{
                    .storage = *storage,
                    .history = output.history,
                });
            }
            result.output_bindings = immutable_bytes_global(
                module,
                bindings.data(),
                bindings.size() * sizeof(ReflectedSampleOutputPortBinding),
                alignof(ReflectedSampleOutputPortBinding),
                "__iv_graph_sample_outputs_" + std::to_string(primitive_index));
        }
    }
    return emitted;
}

std::expected<EmittedEventPortBindings, std::string> emit_event_port_bindings(
    llvm::Module& module,
    detail::EventPortBindingPlan const& plan)
{
    EmittedEventPortBindings emitted;
    emitted.primitives.resize(plan.primitives.size());

    auto storage_binding = [&](std::size_t representation_index)
        -> std::expected<ReflectedEventPortStorageBinding, std::string> {
        if (representation_index >= plan.representations.size()) {
            return std::unexpected(
                "GraphJit event binding references a missing physical representation");
        }
        auto const& representation = plan.representations[representation_index];
        if (!representation.region.valid()) {
            return std::unexpected(
                "GraphJit event representation has no finalized raw storage region");
        }
        return ReflectedEventPortStorageBinding{
            .count_offset = representation.count_storage_offset,
            .events_offset = representation.events_storage_offset,
            .event_capacity = representation.event_capacity,
            .type = representation.type,
        };
    };

    for (std::size_t primitive_index = 0;
         primitive_index < plan.primitives.size();
         ++primitive_index) {
        auto const& primitive = plan.primitives[primitive_index];
        auto& result = emitted.primitives[primitive_index];
        result.input_count = primitive.inputs.size();
        result.output_count = primitive.outputs.size();

        if (!primitive.inputs.empty()) {
            std::vector<ReflectedEventInputPortBinding> bindings;
            bindings.reserve(primitive.inputs.size());
            for (auto const& input : primitive.inputs) {
                if (!input.representation) {
                    return std::unexpected(
                        "GraphJit event input binding has no physical representation");
                }
                auto storage = storage_binding(*input.representation);
                if (!storage) return std::unexpected(std::move(storage.error()));
                bindings.push_back(ReflectedEventInputPortBinding{
                    .storage = *storage,
                });
            }
            result.input_bindings = immutable_bytes_global(
                module,
                bindings.data(),
                bindings.size() * sizeof(ReflectedEventInputPortBinding),
                alignof(ReflectedEventInputPortBinding),
                "__iv_graph_event_inputs_" + std::to_string(primitive_index));
        }

        if (!primitive.outputs.empty()) {
            std::vector<ReflectedEventOutputPortBinding> bindings;
            bindings.reserve(primitive.outputs.size());
            for (auto const& output : primitive.outputs) {
                if (!output.representation) {
                    return std::unexpected(
                        "GraphJit event output binding has no physical representation");
                }
                auto storage = storage_binding(*output.representation);
                if (!storage) return std::unexpected(std::move(storage.error()));
                bindings.push_back(ReflectedEventOutputPortBinding{
                    .storage = *storage,
                    .source_type = output.source_type,
                    .history = output.history,
                    .latency = output.latency,
                });
            }
            result.output_bindings = immutable_bytes_global(
                module,
                bindings.data(),
                bindings.size() * sizeof(ReflectedEventOutputPortBinding),
                alignof(ReflectedEventOutputPortBinding),
                "__iv_graph_event_outputs_" + std::to_string(primitive_index));
        }
    }
    return emitted;
}

void store_context_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* context_storage,
    std::size_t field_offset,
    llvm::Value* value)
{
    auto& llvm_context = builder.getContext();
    auto* byte_type = llvm::Type::getInt8Ty(llvm_context);
    auto* size_type = llvm::IntegerType::get(
        llvm_context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* slot = builder.CreateInBoundsGEP(
        byte_type,
        context_storage,
        llvm::ConstantInt::get(size_type, field_offset),
        "context.pointer.slot");
    builder.CreateStore(value, slot);
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

    auto* data_slot = builder.CreateInBoundsGEP(
        byte_type, context_storage, offset(data_field_offset), "span.data.slot");
    builder.CreateStore(data, data_slot);
    auto* size_slot = builder.CreateInBoundsGEP(
        byte_type, context_storage, offset(size_field_offset), "span.size.slot");
    builder.CreateStore(offset(span_size), size_slot);
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

void emit_primitive_call(
    llvm::IRBuilder<>& builder,
    llvm::Function* primitive_callback,
    EmittedNodeConfiguration const& configuration,
    detail::PrimitiveStoragePlan const& storage,
    EmittedPrimitiveSamplePorts const& sample_ports,
    EmittedPrimitiveEventPorts const& event_ports,
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
    store_context_pointer(
        builder,
        context_storage,
        offsets.sample_storage_base,
        storage_base);
    if (sample_ports.input_count != 0) {
        store_context_span_pointer(
            builder,
            context_storage,
            offsets.sample_input_bindings_data,
            offsets.sample_input_bindings_size,
            sample_ports.input_bindings,
            sample_ports.input_count);
    }
    if (sample_ports.output_count != 0) {
        store_context_span_pointer(
            builder,
            context_storage,
            offsets.sample_output_bindings_data,
            offsets.sample_output_bindings_size,
            sample_ports.output_bindings,
            sample_ports.output_count);
    }
    store_context_pointer(
        builder,
        context_storage,
        offsets.event_storage_base,
        storage_base);
    if (event_ports.input_count != 0) {
        store_context_span_pointer(
            builder,
            context_storage,
            offsets.event_input_bindings_data,
            offsets.event_input_bindings_size,
            event_ports.input_bindings,
            event_ports.input_count);
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
    EmittedPrimitiveSamplePorts const& sample_ports,
    EmittedPrimitiveEventPorts const& event_ports,
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
        sample_ports,
        event_ports,
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


llvm::Value* sample_element_pointer(
    llvm::IRBuilder<>& builder,
    llvm::Value* storage_base,
    ReflectedSamplePortStorageBinding const& binding,
    llvm::Value* absolute_frame,
    std::size_t channel,
    llvm::Twine const& name)
{
    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* sample_type = llvm::Type::getFloatTy(context);
    auto const channels = channel_count(binding.channel_layout);
    auto* frame = builder.CreateAnd(
        absolute_frame,
        llvm::ConstantInt::get(size_type, binding.frame_capacity - 1),
        name + ".frame");
    llvm::Value* element = nullptr;
    if (binding.channel_layout.sample_layout == SampleStreamLayout::planar) {
        element = builder.CreateAdd(
            frame,
            llvm::ConstantInt::get(
                size_type, channel * binding.frame_capacity),
            name + ".element");
    } else {
        element = builder.CreateAdd(
            builder.CreateMul(
                frame,
                llvm::ConstantInt::get(size_type, channels),
                name + ".frame.base"),
            llvm::ConstantInt::get(size_type, channel),
            name + ".element");
    }
    auto* base = byte_offset_pointer(
        builder, storage_base, binding.storage_offset, name + ".base");
    return builder.CreateInBoundsGEP(sample_type, base, element, name + ".ptr");
}

std::expected<void, std::string> emit_sample_materialization(
    llvm::IRBuilder<>& builder,
    detail::SamplePhysicalPlan const& physical,
    detail::SampleMaterializationPlan const& materialization,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    static_assert(sizeof(Sample) == sizeof(Sample::storage));
    static_assert(alignof(Sample) == alignof(Sample::storage));
    static_assert(std::is_same_v<Sample::storage, float>);

    if (materialization.source_representation >= physical.representations.size()
        || materialization.target_representation >= physical.representations.size()) {
        return std::unexpected(
            "GraphJit sample materialization references a missing representation");
    }
    auto const& source_representation =
        physical.representations[materialization.source_representation];
    auto const& target_representation =
        physical.representations[materialization.target_representation];
    if (source_representation.channel_layout != materialization.source_layout
        || target_representation.channel_layout != materialization.target_layout) {
        return std::unexpected(
            "GraphJit sample materialization layout disagrees with its representations");
    }
    auto source = sample_storage_binding(
        physical, materialization.source_representation);
    if (!source) return std::unexpected(std::move(source.error()));
    auto target = sample_storage_binding(
        physical, materialization.target_representation);
    if (!target) return std::unexpected(std::move(target.error()));
    if (source->frame_capacity == 0 || target->frame_capacity == 0
        || !is_power_of_2(source->frame_capacity)
        || !is_power_of_2(target->frame_capacity)) {
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

    auto load_source = [&](std::size_t channel, llvm::Twine const& name) {
        return builder.CreateLoad(
            sample_type,
            sample_element_pointer(
                builder,
                storage_base,
                *source,
                absolute_frame,
                channel,
                name),
            name + ".value");
    };
    auto store_target = [&](std::size_t channel,
                            llvm::Value* value,
                            llvm::Twine const& name) {
        builder.CreateStore(
            value,
            sample_element_pointer(
                builder,
                storage_base,
                *target,
                absolute_frame,
                channel,
                name));
    };

    switch (materialization.source_layout.channel_type) {
    case ChannelTypeId::mono: {
        auto* mono = load_source(0, "sample.materialize.mono");
        switch (materialization.target_layout.channel_type) {
        case ChannelTypeId::mono:
            store_target(0, mono, "sample.materialize.out.mono");
            break;
        case ChannelTypeId::stereo:
            store_target(0, mono, "sample.materialize.out.left");
            store_target(1, mono, "sample.materialize.out.right");
            break;
        case ChannelTypeId::count:
            return std::unexpected(
                "GraphJit sample materialization has an invalid target channel type");
        }
        break;
    }
    case ChannelTypeId::stereo: {
        auto* left = load_source(0, "sample.materialize.left");
        auto* right = load_source(1, "sample.materialize.right");
        switch (materialization.target_layout.channel_type) {
        case ChannelTypeId::mono: {
            auto* sum = builder.CreateFAdd(
                left, right, "sample.materialize.stereo.sum");
            auto* mono = builder.CreateFMul(
                sum,
                llvm::ConstantFP::get(sample_type, 0.5),
                "sample.materialize.stereo.average");
            store_target(0, mono, "sample.materialize.out.mono");
            break;
        }
        case ChannelTypeId::stereo:
            store_target(0, left, "sample.materialize.out.left");
            store_target(1, right, "sample.materialize.out.right");
            break;
        case ChannelTypeId::count:
            return std::unexpected(
                "GraphJit sample materialization has an invalid target channel type");
        }
        break;
    }
    case ChannelTypeId::count:
        return std::unexpected(
            "GraphJit sample materialization has an invalid source channel type");
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


std::expected<void, std::string> emit_sample_composition(
    llvm::IRBuilder<>& builder,
    detail::SamplePhysicalPlan const& physical,
    detail::SampleCompositionPlan const& composition,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    static_assert(sizeof(Sample) == sizeof(Sample::storage));
    static_assert(alignof(Sample) == alignof(Sample::storage));
    static_assert(std::is_same_v<Sample::storage, float>);

    if (composition.target_representation >= physical.representations.size()) {
        return std::unexpected(
            "GraphJit sample composition references a missing target representation");
    }
    auto const& target_representation =
        physical.representations[composition.target_representation];
    if (target_representation.channel_layout != composition.target_layout) {
        return std::unexpected(
            "GraphJit sample composition target layout disagrees with its representation");
    }
    if (composition.sources.size() != channel_count(composition.target_layout)) {
        return std::unexpected(
            "GraphJit sample composition requires exactly one source per target channel");
    }

    auto target = sample_storage_binding(
        physical, composition.target_representation);
    if (!target) return std::unexpected(std::move(target.error()));
    if (target->frame_capacity == 0
        || !is_power_of_2(target->frame_capacity)) {
        return std::unexpected(
            "GraphJit sample composition requires bounded power-of-two target storage");
    }

    struct SourceBinding {
        ReflectedSamplePortStorageBinding storage{};
        std::size_t source_channel = 0;
        std::size_t target_channel = 0;
        std::size_t read_latency = 0;
    };
    std::vector<SourceBinding> sources;
    sources.reserve(composition.sources.size());
    std::vector<bool> target_channels(
        channel_count(composition.target_layout), false);
    for (auto const& source_plan : composition.sources) {
        if (source_plan.source_representation >= physical.representations.size()) {
            return std::unexpected(
                "GraphJit sample composition references a missing source representation");
        }
        auto source = sample_storage_binding(
            physical, source_plan.source_representation);
        if (!source) return std::unexpected(std::move(source.error()));
        if (source->frame_capacity == 0
            || !is_power_of_2(source->frame_capacity)
            || source_plan.source_channel >= channel_count(source->channel_layout)
            || source_plan.target_channel >= target_channels.size()
            || target_channels[source_plan.target_channel]) {
            return std::unexpected(
                "GraphJit sample composition contains an invalid channel mapping");
        }
        target_channels[source_plan.target_channel] = true;
        sources.push_back(SourceBinding{
            .storage = *source,
            .source_channel = source_plan.source_channel,
            .target_channel = source_plan.target_channel,
            .read_latency = source_plan.read_latency,
        });
    }
    if (!std::ranges::all_of(target_channels, [](bool value) { return value; })) {
        return std::unexpected(
            "GraphJit sample composition does not populate every target channel");
    }

    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* sample_type = llvm::Type::getFloatTy(context);
    auto* zero = llvm::ConstantInt::get(size_type, 0);
    auto* history = llvm::ConstantInt::get(
        size_type, composition.target_history);
    auto* compose_count = builder.CreateAdd(
        block_size, history, "sample.compose.count");
    auto* preheader = builder.GetInsertBlock();
    auto* has_frames = builder.CreateICmpNE(
        compose_count, zero, "sample.compose.nonempty");
    auto* loop = llvm::BasicBlock::Create(
        context, "sample.compose", function);
    auto* exit = llvm::BasicBlock::Create(
        context, "sample.compose.end", function);
    builder.CreateCondBr(has_frames, loop, exit);

    builder.SetInsertPoint(loop);
    auto* frame_offset = builder.CreatePHI(
        size_type, 2, "sample.compose.frame");
    frame_offset->addIncoming(zero, preheader);
    auto* first_frame = builder.CreateSub(
        sample_index, history, "sample.compose.first");
    auto* target_frame = builder.CreateAdd(
        first_frame, frame_offset, "sample.compose.absolute");

    for (std::size_t i = 0; i < sources.size(); ++i) {
        auto const& source = sources[i];
        auto* source_frame = builder.CreateSub(
            target_frame,
            llvm::ConstantInt::get(size_type, source.read_latency),
            "sample.compose.source.frame." + std::to_string(i));
        auto* source_pointer = sample_element_pointer(
            builder,
            storage_base,
            source.storage,
            source_frame,
            source.source_channel,
            "sample.compose.source." + std::to_string(i));
        auto* value = builder.CreateLoad(
            sample_type,
            source_pointer,
            "sample.compose.value." + std::to_string(i));
        auto* target_pointer = sample_element_pointer(
            builder,
            storage_base,
            *target,
            target_frame,
            source.target_channel,
            "sample.compose.target." + std::to_string(i));
        builder.CreateStore(value, target_pointer);
    }

    auto* next = builder.CreateAdd(
        frame_offset,
        llvm::ConstantInt::get(size_type, 1),
        "sample.compose.next");
    auto* done = builder.CreateICmpUGE(
        next, compose_count, "sample.compose.done");
    builder.CreateCondBr(done, exit, loop);
    frame_offset->addIncoming(next, loop);
    builder.SetInsertPoint(exit);
    return {};
}

std::expected<detail::SamplePersistentAllocationPlan const*, std::string>
compact_carry_allocation(
    detail::SamplePhysicalPlan const& physical,
    detail::SampleCarryOperationPlan const& operation)
{
    if (operation.representation_index >= physical.representations.size()) {
        return std::unexpected(
            "GraphJit sample carry references a missing representation");
    }
    if (operation.persistent_allocation >= physical.persistent_allocations.size()) {
        return std::unexpected(
            "GraphJit sample carry references a missing persistent allocation");
    }
    auto const& representation =
        physical.representations[operation.representation_index];
    auto const& allocation =
        physical.persistent_allocations[operation.persistent_allocation];
    if (allocation.kind != detail::SamplePersistentStorageKind::compact_carry
        || allocation.representation_index != operation.representation_index
        || representation.persistent_allocation != operation.persistent_allocation
        || representation.transient_allocation
               == detail::no_sample_transient_allocation
        || allocation.retained_frames != operation.retained_frames
        || operation.retained_frames == 0) {
        return std::unexpected(
            "GraphJit sample carry plan is inconsistent with its physical representation");
    }
    return &allocation;
}

llvm::Value* compact_carry_element_pointer(
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
    auto* sample_type = llvm::Type::getFloatTy(context);
    auto const channels = channel_count(allocation.channel_layout);
    llvm::Value* element = nullptr;
    if (allocation.channel_layout.sample_layout == SampleStreamLayout::planar) {
        element = builder.CreateAdd(
            carry_frame,
            llvm::ConstantInt::get(
                size_type, channel * allocation.retained_frames),
            name + ".element");
    } else {
        element = builder.CreateAdd(
            builder.CreateMul(
                carry_frame,
                llvm::ConstantInt::get(size_type, channels),
                name + ".frame.base"),
            llvm::ConstantInt::get(size_type, channel),
            name + ".element");
    }
    auto* base = byte_offset_pointer(
        builder, storage_base, allocation.storage_offset, name + ".base");
    return builder.CreateInBoundsGEP(sample_type, base, element, name + ".ptr");
}

std::expected<void, std::string> emit_sample_carry_operation(
    llvm::IRBuilder<>& builder,
    detail::SamplePhysicalPlan const& physical,
    detail::SampleCarryOperationPlan const& operation,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    bool restore)
{
    static_assert(sizeof(Sample) == sizeof(Sample::storage));
    static_assert(std::is_same_v<Sample::storage, float>);

    auto allocation = compact_carry_allocation(physical, operation);
    if (!allocation) return std::unexpected(std::move(allocation.error()));
    auto working = sample_storage_binding(physical, operation.representation_index);
    if (!working) return std::unexpected(std::move(working.error()));
    if (working->frame_capacity == 0
        || !is_power_of_2(working->frame_capacity)
        || working->channel_layout != (*allocation)->channel_layout) {
        return std::unexpected(
            "GraphJit sample carry working representation is invalid");
    }

    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* sample_type = llvm::Type::getFloatTy(context);
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
    llvm::Value* first_absolute = nullptr;
    if (restore) {
        first_absolute = builder.CreateSub(
            sample_index, retained, "sample.carry.restore.first");
    } else {
        first_absolute = builder.CreateSub(
            builder.CreateAdd(
                sample_index, block_size, "sample.carry.commit.end.index"),
            retained,
            "sample.carry.commit.first");
    }
    auto* absolute_frame = builder.CreateAdd(
        first_absolute,
        carry_frame,
        restore ? "sample.carry.restore.absolute"
                : "sample.carry.commit.absolute");

    auto const channels = channel_count((*allocation)->channel_layout);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        auto* carry_pointer = compact_carry_element_pointer(
            builder,
            storage_base,
            **allocation,
            carry_frame,
            channel,
            restore ? "sample.carry.restore.persist"
                    : "sample.carry.commit.persist");
        auto* working_pointer = sample_element_pointer(
            builder,
            storage_base,
            *working,
            absolute_frame,
            channel,
            restore ? "sample.carry.restore.working"
                    : "sample.carry.commit.working");
        if (restore) {
            auto* value = builder.CreateLoad(
                sample_type, carry_pointer, "sample.carry.restore.value");
            builder.CreateStore(value, working_pointer);
        } else {
            auto* value = builder.CreateLoad(
                sample_type, working_pointer, "sample.carry.commit.value");
            builder.CreateStore(value, carry_pointer);
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

std::expected<llvm::Function*, std::string> define_root_operation(
    llvm::Module& module,
    std::string_view symbol,
    detail::LoweringPlan const& plan,
    std::vector<EmittedNodeConfiguration> const& configurations,
    EmittedSamplePortBindings const& sample_bindings,
    EmittedEventPortBindings const& event_bindings,
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
        if (step.configuration_index >= sample_bindings.primitives.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing sample-port runtime plan");
        }
        if (step.configuration_index >= event_bindings.primitives.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing event-port runtime plan");
        }

        for (auto const carry_index : step.sample_carry_restores_before) {
            if (carry_index >= plan.sample_ports.physical.carry_operations.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing sample carry restore");
            }
            auto restored = emit_sample_carry_operation(
                builder,
                plan.sample_ports.physical,
                plan.sample_ports.physical.carry_operations[carry_index],
                storage_base,
                sample_index,
                block_size,
                true);
            if (!restored) {
                return std::unexpected(std::move(restored.error()));
            }
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
                sample_bindings.primitives[step.configuration_index],
                event_bindings.primitives[step.configuration_index],
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
                sample_bindings.primitives[step.configuration_index],
                event_bindings.primitives[step.configuration_index],
                storage_base,
                sample_index,
                block_size);
        }

        for (auto const materialization_index :
             step.sample_materializations_after) {
            if (materialization_index
                >= plan.sample_ports.physical.materializations.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing sample materialization");
            }
            auto materialized = emit_sample_materialization(
                builder,
                plan.sample_ports.physical,
                plan.sample_ports.physical.materializations[materialization_index],
                storage_base,
                sample_index,
                block_size);
            if (!materialized) {
                return std::unexpected(std::move(materialized.error()));
            }
        }

        for (auto const composition_index : step.sample_compositions_after) {
            if (composition_index >= plan.sample_ports.physical.compositions.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing sample composition");
            }
            auto composed = emit_sample_composition(
                builder,
                plan.sample_ports.physical,
                plan.sample_ports.physical.compositions[composition_index],
                storage_base,
                sample_index,
                block_size);
            if (!composed) {
                return std::unexpected(std::move(composed.error()));
            }
        }

        for (auto const carry_index : step.sample_carry_commits_after) {
            if (carry_index >= plan.sample_ports.physical.carry_operations.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing sample carry commit");
            }
            auto committed = emit_sample_carry_operation(
                builder,
                plan.sample_ports.physical,
                plan.sample_ports.physical.carry_operations[carry_index],
                storage_base,
                sample_index,
                block_size,
                false);
            if (!committed) {
                return std::unexpected(std::move(committed.error()));
            }
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

    auto sample_bindings = emit_sample_port_bindings(
        output_module, plan.sample_ports);
    if (!sample_bindings) {
        return std::unexpected(std::move(sample_bindings.error()));
    }
    auto event_bindings = emit_event_port_bindings(
        output_module, plan.event_ports);
    if (!event_bindings) {
        return std::unexpected(std::move(event_bindings.error()));
    }

    auto tick = define_root_operation(
        output_module,
        root_tick_block_symbol,
        plan,
        *configurations,
        *sample_bindings,
        *event_bindings,
        false);
    if (!tick) return std::unexpected(std::move(tick.error()));

    std::string skip_symbol;
    if (plan.execution.root_skippable) {
        auto skip = define_root_operation(
            output_module,
            root_skip_block_symbol,
            plan,
            *configurations,
            *sample_bindings,
            *event_bindings,
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
