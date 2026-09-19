#include <intravenous/graph_jit/lowering.h>
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
                    .latency = output.latency,
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
            .read_index_offset = representation.read_index_storage_offset,
            .write_index_offset = representation.write_index_storage_offset,
            .events_offset = representation.events_storage_offset,
            .event_capacity = representation.event_capacity,
            .type = representation.type,
            .persistent_ring = representation.persistent_ring,
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
                auto const& representation =
                    plan.representations[*output.representation];
                if (!representation.has_producer_overflow_counter) {
                    return std::unexpected(
                        "GraphJit event output representation lost producer telemetry");
                }
                bindings.push_back(ReflectedEventOutputPortBinding{
                    .storage = *storage,
                    .overflow_count_offset =
                        representation.overflow_count_storage_offset,
                    .source_type = output.source_type,
                    .history = output.history,
                    .latency = output.latency,
                    .write_capacity = output.write_capacity,
                    .append_existing = output.append_existing,
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


std::expected<void, std::string> emit_sample_composition_write(
    llvm::IRBuilder<>& builder,
    detail::SamplePhysicalPlan const& physical,
    std::vector<detail::SampleCompositionContributionPlan> const& contribution_plans,
    std::size_t target_representation_index,
    ChannelLayout target_layout,
    std::size_t target_history,
    bool shift_writes_by_read_latency,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    static_assert(sizeof(Sample) == sizeof(Sample::storage));
    static_assert(alignof(Sample) == alignof(Sample::storage));
    static_assert(std::is_same_v<Sample::storage, float>);

    if (target_representation_index >= physical.representations.size()) {
        return std::unexpected(
            "GraphJit sample composition references a missing target representation");
    }
    auto const& target_representation =
        physical.representations[target_representation_index];
    if (target_representation.channel_layout != target_layout) {
        return std::unexpected(
            "GraphJit sample composition target layout disagrees with its representation");
    }
    if (contribution_plans.empty()) {
        return std::unexpected(
            "GraphJit sample composition contains no semantic contributions");
    }
    if (shift_writes_by_read_latency
        && (target_history != 0
            || target_representation.implementation
                != SampleConnectionImplementationKind::feedback_ring
            || target_representation.persistent_allocation
                == detail::no_sample_persistent_allocation)) {
        return std::unexpected(
            "GraphJit shifted sample composition requires a persistent feedback target");
    }

    auto target = sample_storage_binding(
        physical, target_representation_index);
    if (!target) return std::unexpected(std::move(target.error()));
    if (target->frame_capacity == 0
        || !is_power_of_2(target->frame_capacity)) {
        return std::unexpected(
            "GraphJit sample composition requires bounded power-of-two target storage");
    }

    struct SourceBinding {
        ReflectedSamplePortStorageBinding storage{};
        std::size_t source_channel = 0;
        std::size_t read_latency = 0;
    };
    struct ContributionBinding {
        ChannelLayout source_layout{};
        ChannelLayout converted_layout{};
        std::vector<SourceBinding> sources{};
        std::vector<std::size_t> target_channels{};
        std::vector<std::size_t> shifted_write_latencies{};
        std::optional<ReflectedSamplePortStorageBinding> feedback_alignment_storage{};
        std::size_t feedback_alignment_write_latency = 0;
    };

    std::vector<ContributionBinding> contributions;
    contributions.reserve(contribution_plans.size());
    std::vector<bool> populated_targets(channel_count(target_layout), false);
    for (auto const& contribution_plan : contribution_plans) {
        if (contribution_plan.sources.size()
                != channel_count(contribution_plan.source_layout)
            || contribution_plan.target_channels.size()
                != channel_count(contribution_plan.converted_layout)) {
            return std::unexpected(
                "GraphJit sample composition contribution has inconsistent semantic channel counts");
        }
        try {
            (void)ChannelConversionRegistry::plan(
                contribution_plan.source_layout,
                contribution_plan.converted_layout);
        } catch (std::exception const& e) {
            return std::unexpected(
                "GraphJit sample composition conversion is unsupported: "
                + std::string(e.what()));
        }

        ContributionBinding contribution{
            .source_layout = contribution_plan.source_layout,
            .converted_layout = contribution_plan.converted_layout,
            .target_channels = contribution_plan.target_channels,
        };
        contribution.sources.reserve(contribution_plan.sources.size());
        for (auto const& source_plan : contribution_plan.sources) {
            if (source_plan.source_representation
                >= physical.representations.size()) {
                return std::unexpected(
                    "GraphJit sample composition references a missing source representation");
            }
            auto source = sample_storage_binding(
                physical, source_plan.source_representation);
            if (!source) return std::unexpected(std::move(source.error()));
            if (source->frame_capacity == 0
                || !is_power_of_2(source->frame_capacity)
                || source_plan.source_channel
                    >= channel_count(source->channel_layout)) {
                return std::unexpected(
                    "GraphJit sample composition contains an invalid source channel");
            }
            contribution.sources.push_back(SourceBinding{
                .storage = *source,
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
                channel_count(converted_type));
            if (source_type == converted_type) {
                for (std::size_t channel = 0;
                     channel < contribution.shifted_write_latencies.size();
                     ++channel) {
                    contribution.shifted_write_latencies[channel] =
                        contribution.sources[channel].read_latency;
                }
            } else if (source_type == ChannelTypeId::mono) {
                auto const latency = contribution.sources.front().read_latency;
                std::ranges::fill(
                    contribution.shifted_write_latencies, latency);
            } else if (source_type == ChannelTypeId::stereo
                && converted_type == ChannelTypeId::mono) {
                auto const minimum_latency = std::min(
                    contribution.sources[0].read_latency,
                    contribution.sources[1].read_latency);
                auto const maximum_latency = std::max(
                    contribution.sources[0].read_latency,
                    contribution.sources[1].read_latency);
                contribution.shifted_write_latencies[0] = minimum_latency;
                if (minimum_latency != maximum_latency) {
                    if (contribution_plan.feedback_alignment_representation
                            >= physical.representations.size()
                        || contribution_plan.feedback_alignment_write_latency
                            != minimum_latency) {
                        return std::unexpected(
                            "GraphJit unequal-latency feedback mixing lost its alignment plan");
                    }
                    auto alignment = sample_storage_binding(
                        physical,
                        contribution_plan.feedback_alignment_representation);
                    if (!alignment) {
                        return std::unexpected(std::move(alignment.error()));
                    }
                    if (alignment->channel_layout != contribution.source_layout
                        || alignment->frame_capacity == 0
                        || !is_power_of_2(alignment->frame_capacity)) {
                        return std::unexpected(
                            "GraphJit unequal-latency feedback mixing has invalid alignment storage");
                    }
                    contribution.feedback_alignment_storage = *alignment;
                    contribution.feedback_alignment_write_latency =
                        minimum_latency;
                } else if (contribution_plan.feedback_alignment_representation
                               != detail::no_sample_representation) {
                    return std::unexpected(
                        "GraphJit equal-latency feedback mixing unexpectedly owns alignment storage");
                }
            } else {
                return std::unexpected(
                    "GraphJit shifted sample composition has an unsupported semantic conversion");
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
    auto* history = llvm::ConstantInt::get(size_type, target_history);
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
                    "sample.compose.source.frame."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index));
            }
            auto* source_pointer = sample_element_pointer(
                builder,
                storage_base,
                source.storage,
                source_frame,
                source.source_channel,
                "sample.compose.source."
                    + std::to_string(contribution_index) + "."
                    + std::to_string(source_index));
            source_values.push_back(builder.CreateLoad(
                sample_type,
                source_pointer,
                "sample.compose.value."
                    + std::to_string(contribution_index) + "."
                    + std::to_string(source_index)));
        }

        if (contribution.feedback_alignment_storage) {
            for (std::size_t source_index = 0;
                 source_index < contribution.sources.size(); ++source_index) {
                auto* alignment_pointer = sample_element_pointer(
                    builder,
                    storage_base,
                    *contribution.feedback_alignment_storage,
                    target_frame,
                    source_index,
                    "sample.compose.align.write."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index));
                builder.CreateStore(
                    source_values[source_index], alignment_pointer);
            }

            for (std::size_t source_index = 0;
                 source_index < contribution.sources.size(); ++source_index) {
                auto const latency_delta =
                    contribution.sources[source_index].read_latency
                    - contribution.feedback_alignment_write_latency;
                auto* aligned_frame = builder.CreateSub(
                    target_frame,
                    llvm::ConstantInt::get(size_type, latency_delta),
                    "sample.compose.align.frame."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index));
                auto* alignment_pointer = sample_element_pointer(
                    builder,
                    storage_base,
                    *contribution.feedback_alignment_storage,
                    aligned_frame,
                    source_index,
                    "sample.compose.align.read."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index));
                source_values[source_index] = builder.CreateLoad(
                    sample_type,
                    alignment_pointer,
                    "sample.compose.align.value."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(source_index));
            }

        }

        std::vector<llvm::Value*> converted_values;
        switch (contribution.source_layout.channel_type) {
        case ChannelTypeId::mono: {
            auto* mono = source_values[0];
            switch (contribution.converted_layout.channel_type) {
            case ChannelTypeId::mono:
                converted_values.push_back(mono);
                break;
            case ChannelTypeId::stereo:
                converted_values.push_back(mono);
                converted_values.push_back(mono);
                break;
            case ChannelTypeId::count:
                return std::unexpected(
                    "GraphJit sample composition has an invalid converted channel type");
            }
            break;
        }
        case ChannelTypeId::stereo: {
            auto* left = source_values[0];
            auto* right = source_values[1];
            switch (contribution.converted_layout.channel_type) {
            case ChannelTypeId::mono: {
                auto* sum = builder.CreateFAdd(
                    left, right, "sample.compose.stereo.sum");
                converted_values.push_back(builder.CreateFMul(
                    sum,
                    llvm::ConstantFP::get(sample_type, 0.5),
                    "sample.compose.stereo.average"));
                break;
            }
            case ChannelTypeId::stereo:
                converted_values.push_back(left);
                converted_values.push_back(right);
                break;
            case ChannelTypeId::count:
                return std::unexpected(
                    "GraphJit sample composition has an invalid converted channel type");
            }
            break;
        }
        case ChannelTypeId::count:
            return std::unexpected(
                "GraphJit sample composition has an invalid source channel type");
        }

        if (converted_values.size() != contribution.target_channels.size()) {
            return std::unexpected(
                "GraphJit sample composition conversion produced an invalid channel count");
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
                    "sample.compose.target.frame."
                        + std::to_string(contribution_index) + "."
                        + std::to_string(converted_channel));
            }
            auto* target_pointer = sample_element_pointer(
                builder,
                storage_base,
                *target,
                target_storage_frame,
                contribution.target_channels[converted_channel],
                "sample.compose.target."
                    + std::to_string(contribution_index) + "."
                    + std::to_string(converted_channel));
            builder.CreateStore(
                converted_values[converted_channel], target_pointer);
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
    detail::SamplePhysicalPlan const& physical,
    detail::SampleCompositionPlan const& composition,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    return emit_sample_composition_write(
        builder,
        physical,
        composition.contributions,
        composition.target_representation,
        composition.target_layout,
        composition.target_history,
        false,
        storage_base,
        sample_index,
        block_size);
}

std::expected<void, std::string> emit_sample_feedback_timeline_write(
    llvm::IRBuilder<>& builder,
    detail::SamplePhysicalPlan const& physical,
    detail::SampleFeedbackTimelinePlan const& timeline,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    static_assert(sizeof(Sample) == sizeof(Sample::storage));
    static_assert(std::is_same_v<Sample::storage, float>);

    if (timeline.timeline_representation >= physical.representations.size()) {
        return std::unexpected(
            "GraphJit sample feedback timeline references a missing representation");
    }
    auto const& timeline_plan =
        physical.representations[timeline.timeline_representation];
    if (timeline_plan.implementation
            != SampleConnectionImplementationKind::feedback_ring
        || timeline_plan.persistent_allocation
            == detail::no_sample_persistent_allocation
        || timeline_plan.channel_layout != timeline.channel_layout
        || timeline.loop_extra_latency == 0
        || timeline.loop_extra_latency >= timeline_plan.frame_capacity) {
        return std::unexpected(
            "GraphJit sample feedback timeline is inconsistent with its ring representation");
    }
    if (timeline_plan.persistent_allocation
        >= physical.persistent_allocations.size()) {
        return std::unexpected(
            "GraphJit sample feedback timeline references a missing persistent ring");
    }
    auto const& allocation =
        physical.persistent_allocations[timeline_plan.persistent_allocation];
    if (allocation.kind != detail::SamplePersistentStorageKind::ring
        || allocation.representation_index != timeline.timeline_representation
        || allocation.retained_frames < timeline.retained_frames
        || !allocation.initialize_value
        || static_cast<float>(*allocation.initialize_value)
            != static_cast<float>(timeline.initial_value)) {
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
            physical,
            timeline.writer.composition_contributions,
            timeline.timeline_representation,
            timeline.channel_layout,
            0,
            true,
            storage_base,
            sample_index,
            block_size);
    case detail::SampleFeedbackTimelineWriterKind::copy:
        break;
    }

    if (!timeline.writer.composition_contributions.empty()
        || timeline.writer.source_representation >= physical.representations.size()) {
        return std::unexpected(
            "GraphJit copied sample feedback timeline has an invalid writer shape");
    }
    auto const& source_plan =
        physical.representations[timeline.writer.source_representation];
    if (timeline_plan.channel_layout != source_plan.channel_layout) {
        return std::unexpected(
            "GraphJit sample feedback copy changed channel layout without composition");
    }

    auto source = sample_storage_binding(
        physical, timeline.writer.source_representation);
    if (!source) return std::unexpected(std::move(source.error()));
    auto ring = sample_storage_binding(
        physical, timeline.timeline_representation);
    if (!ring) return std::unexpected(std::move(ring.error()));

    auto& context = builder.getContext();
    auto* function = builder.GetInsertBlock()->getParent();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* sample_type = llvm::Type::getFloatTy(context);
    auto* zero = llvm::ConstantInt::get(size_type, 0);
    auto* preheader = builder.GetInsertBlock();
    auto* loop = llvm::BasicBlock::Create(
        context, "sample.feedback.copy", function);
    auto* exit = llvm::BasicBlock::Create(
        context, "sample.feedback.copy.end", function);
    builder.CreateCondBr(
        builder.CreateICmpNE(
            block_size, zero, "sample.feedback.copy.nonempty"),
        loop,
        exit);

    builder.SetInsertPoint(loop);
    auto* frame_offset = builder.CreatePHI(
        size_type, 2, "sample.feedback.copy.frame");
    frame_offset->addIncoming(zero, preheader);
    auto* absolute_frame = builder.CreateAdd(
        sample_index, frame_offset, "sample.feedback.copy.absolute");

    auto const channels = channel_count(source_plan.channel_layout);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        auto* source_pointer = sample_element_pointer(
            builder,
            storage_base,
            *source,
            absolute_frame,
            channel,
            "sample.feedback.source." + std::to_string(channel));
        auto* value = builder.CreateLoad(
            sample_type,
            source_pointer,
            "sample.feedback.value." + std::to_string(channel));
        auto* ring_pointer = sample_element_pointer(
            builder,
            storage_base,
            *ring,
            absolute_frame,
            channel,
            "sample.feedback.ring." + std::to_string(channel));
        builder.CreateStore(value, ring_pointer);
    }

    auto* next = builder.CreateAdd(
        frame_offset,
        llvm::ConstantInt::get(size_type, 1),
        "sample.feedback.copy.next");
    auto* done = builder.CreateICmpUGE(
        next, block_size, "sample.feedback.copy.done");
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


std::expected<void, std::string> emit_event_carry_operation(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventCarryPlan const& carry,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    bool restore)
{
    if (carry.working_representation >= event_ports.representations.size()
        || carry.persistent_representation >= event_ports.representations.size()) {
        return std::unexpected(
            "GraphJit event carry references a missing representation");
    }
    auto const& working =
        event_ports.representations[carry.working_representation];
    auto const& persistent =
        event_ports.representations[carry.persistent_representation];
    if (!working.region.valid() || !persistent.region.valid()
        || working.persistent || !persistent.persistent
        || working.type != persistent.type) {
        return std::unexpected(
            "GraphJit event carry has inconsistent physical representations");
    }

    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* working_count_pointer = byte_offset_pointer(
        builder,
        storage_base,
        working.count_storage_offset,
        restore ? "event.carry.restore.working.count"
                : "event.carry.commit.working.count");
    auto* persistent_count_pointer = byte_offset_pointer(
        builder,
        storage_base,
        persistent.count_storage_offset,
        restore ? "event.carry.restore.persist.count"
                : "event.carry.commit.persist.count");
    auto* working_events = byte_offset_pointer(
        builder,
        storage_base,
        working.events_storage_offset,
        restore ? "event.carry.restore.working.events"
                : "event.carry.commit.working.events");
    auto* persistent_events = byte_offset_pointer(
        builder,
        storage_base,
        persistent.events_storage_offset,
        restore ? "event.carry.restore.persist.events"
                : "event.carry.commit.persist.events");

    if (restore) {
        auto* persistent_count = builder.CreateLoad(
            size_type,
            persistent_count_pointer,
            "event.carry.restore.persist.count.value");
        auto* helper_type = llvm::FunctionType::get(
            size_type,
            {pointer_type, size_type, pointer_type, size_type},
            false);
        auto* module = builder.GetInsertBlock()->getModule();
        auto helper = module->getOrInsertFunction(
            detail::event_carry_restore_symbol, helper_type);
        auto* restored_count = builder.CreateCall(
            helper,
            {persistent_events,
             persistent_count,
             working_events,
             llvm::ConstantInt::get(size_type, working.event_capacity)},
            "event.carry.restore.count");
        builder.CreateStore(restored_count, working_count_pointer);
        return {};
    }

    auto* working_count = builder.CreateLoad(
        size_type,
        working_count_pointer,
        "event.carry.commit.working.count.value");
    auto* helper_type = llvm::FunctionType::get(
        size_type,
        {pointer_type, size_type, size_type, size_type, size_type, size_type,
         pointer_type, size_type},
        false);
    auto* module = builder.GetInsertBlock()->getModule();
    auto helper = module->getOrInsertFunction(
        detail::event_carry_commit_symbol, helper_type);
    auto* committed_count = builder.CreateCall(
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
    builder.CreateStore(committed_count, persistent_count_pointer);
    return {};
}

std::expected<void, std::string> seed_event_feedback_cursors_after_carry_restore(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventCarryPlan const& carry,
    std::vector<llvm::Value*> const& event_feedback_cursors,
    llvm::Value* storage_base)
{
    if (carry.working_representation >= event_ports.representations.size()) {
        return std::unexpected(
            "GraphJit event carry feedback cursor references a missing working representation");
    }
    auto const& working =
        event_ports.representations[carry.working_representation];
    if (!working.region.valid() || working.persistent
        || working.persistent_ring) {
        return std::unexpected(
            "GraphJit event carry feedback cursor has invalid working storage");
    }

    bool needs_seed = false;
    for (std::size_t feedback_index = 0;
         feedback_index < event_ports.feedback_operations.size();
         ++feedback_index) {
        if (event_ports.feedback_operations[feedback_index].source_representation
            != carry.working_representation) {
            continue;
        }
        if (feedback_index >= event_feedback_cursors.size()
            || event_feedback_cursors[feedback_index] == nullptr) {
            return std::unexpected(
                "GraphJit event carry feedback cursor is missing root-call storage");
        }
        needs_seed = true;
    }
    if (!needs_seed) return {};

    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* working_count_pointer = byte_offset_pointer(
        builder,
        storage_base,
        working.count_storage_offset,
        "event.carry.feedback.seed.count");
    auto* restored_count = builder.CreateLoad(
        size_type, working_count_pointer, "event.carry.feedback.seed.value");
    for (std::size_t feedback_index = 0;
         feedback_index < event_ports.feedback_operations.size();
         ++feedback_index) {
        if (event_ports.feedback_operations[feedback_index].source_representation
            == carry.working_representation) {
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
    llvm::Value* storage_base,
    llvm::Value* sample_index)
{
    if (ring.representation >= event_ports.representations.size()) {
        return std::unexpected(
            "GraphJit persistent event ring references a missing representation");
    }
    auto const& representation = event_ports.representations[ring.representation];
    if (!representation.region.valid()
        || !representation.persistent
        || !representation.persistent_ring) {
        return std::unexpected(
            "GraphJit persistent event ring has inconsistent physical storage");
    }

    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* read_index_pointer = byte_offset_pointer(
        builder,
        storage_base,
        representation.read_index_storage_offset,
        "event.ring.prune.read");
    auto* write_index_pointer = byte_offset_pointer(
        builder,
        storage_base,
        representation.write_index_storage_offset,
        "event.ring.prune.write");
    auto* events = byte_offset_pointer(
        builder,
        storage_base,
        representation.events_storage_offset,
        "event.ring.prune.events");
    auto* read_index = builder.CreateLoad(
        size_type, read_index_pointer, "event.ring.prune.read.value");
    auto* write_index = builder.CreateLoad(
        size_type, write_index_pointer, "event.ring.prune.write.value");

    auto* helper_type = llvm::FunctionType::get(
        size_type,
        {pointer_type, size_type, size_type, size_type, size_type, size_type},
        false);
    auto* module = builder.GetInsertBlock()->getModule();
    auto helper = module->getOrInsertFunction(
        detail::event_persistent_ring_prune_symbol, helper_type);
    auto* pruned_read_index = builder.CreateCall(
        helper,
        {events,
         llvm::ConstantInt::get(size_type, representation.event_capacity),
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
    llvm::Value* storage_base,
    llvm::Value* sample_index)
{
    if (feedback.source_representation >= event_ports.representations.size()
        || feedback.ring_representation >= event_ports.representations.size()) {
        return std::unexpected(
            "GraphJit event feedback references a missing representation");
    }
    if (feedback_cursor_pointer == nullptr) {
        return std::unexpected(
            "GraphJit event feedback has no root-call source cursor");
    }
    auto const& source =
        event_ports.representations[feedback.source_representation];
    auto const& ring =
        event_ports.representations[feedback.ring_representation];
    if (!source.region.valid() || !ring.region.valid()
        || !ring.persistent || !ring.persistent_ring
        || source.type != ring.type) {
        return std::unexpected(
            "GraphJit exact-type event feedback has inconsistent physical storage");
    }

    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* source_begin = builder.CreateLoad(
        size_type,
        feedback_cursor_pointer,
        "event.feedback.source.cursor");
    llvm::Value* source_end = nullptr;
    if (source.persistent_ring) {
        auto* source_write_pointer = byte_offset_pointer(
            builder,
            storage_base,
            source.write_index_storage_offset,
            "event.feedback.source.write");
        source_end = builder.CreateLoad(
            size_type,
            source_write_pointer,
            "event.feedback.source.write.value");
    } else {
        auto* source_count_pointer = byte_offset_pointer(
            builder,
            storage_base,
            source.count_storage_offset,
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
        storage_base,
        source.events_storage_offset,
        "event.feedback.source.events");
    auto* ring_read_pointer = byte_offset_pointer(
        builder,
        storage_base,
        ring.read_index_storage_offset,
        "event.feedback.ring.read");
    auto* ring_write_pointer = byte_offset_pointer(
        builder,
        storage_base,
        ring.write_index_storage_offset,
        "event.feedback.ring.write");
    auto* ring_events = byte_offset_pointer(
        builder,
        storage_base,
        ring.events_storage_offset,
        "event.feedback.ring.events");

    auto* module = builder.GetInsertBlock()->getModule();
    llvm::FunctionCallee helper;
    if (source.persistent_ring) {
        auto* helper_type = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context),
            {pointer_type, size_type, size_type, size_type, size_type, size_type,
             pointer_type, size_type, pointer_type, pointer_type},
            false);
        helper = module->getOrInsertFunction(
            detail::event_feedback_append_ring_source_symbol, helper_type);
    } else {
        auto* helper_type = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context),
            {pointer_type, size_type, size_type, size_type, size_type,
             pointer_type, size_type, pointer_type, pointer_type},
            false);
        helper = module->getOrInsertFunction(
            detail::event_feedback_append_symbol, helper_type);
    }

    // Most event slices produce no events. Keep the audio-thread fast path to a
    // source-index load/compare and avoid the out-of-line feedback helper
    // entirely unless this producer appended a new suffix.
    builder.CreateStore(source_end, feedback_cursor_pointer);
    auto* has_new_events = builder.CreateICmpULT(
        source_begin, source_end, "event.feedback.has_new_events");
    auto* function = builder.GetInsertBlock()->getParent();
    auto* append_block = llvm::BasicBlock::Create(
        context, "event.feedback.append", function);
    auto* continue_block = llvm::BasicBlock::Create(
        context, "event.feedback.continue", function);
    builder.CreateCondBr(has_new_events, append_block, continue_block);

    builder.SetInsertPoint(append_block);
    if (source.persistent_ring) {
        builder.CreateCall(
            helper,
            {source_events,
             llvm::ConstantInt::get(size_type, source.event_capacity),
             source_begin,
             source_end,
             sample_index,
             llvm::ConstantInt::get(size_type, feedback.loop_extra_latency),
             ring_events,
             llvm::ConstantInt::get(size_type, ring.event_capacity),
             ring_read_pointer,
             ring_write_pointer});
    } else {
        builder.CreateCall(
            helper,
            {source_events,
             source_begin,
             source_end,
             sample_index,
             llvm::ConstantInt::get(size_type, feedback.loop_extra_latency),
             ring_events,
             llvm::ConstantInt::get(size_type, ring.event_capacity),
             ring_read_pointer,
             ring_write_pointer});
    }
    builder.CreateBr(continue_block);
    builder.SetInsertPoint(continue_block);
    return {};
}

std::expected<void, std::string> emit_event_sequence_reset(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    std::size_t representation_index,
    llvm::Value* storage_base)
{
    if (representation_index >= event_ports.representations.size()) {
        return std::unexpected(
            "GraphJit event sequence reset references a missing representation");
    }
    auto const& representation = event_ports.representations[representation_index];
    if (!representation.region.valid()) {
        return std::unexpected(
            "GraphJit event sequence reset references unfinalized storage");
    }
    auto* size_type = llvm::IntegerType::get(
        builder.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* count = byte_offset_pointer(
        builder,
        storage_base,
        representation.count_storage_offset,
        "event.reset.count");
    builder.CreateStore(llvm::ConstantInt::get(size_type, 0), count);
    return {};
}

std::expected<void, std::string> emit_event_merge(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventMergePlan const& merge,
    llvm::Value* storage_base)
{
    if (merge.target_representation >= event_ports.representations.size()
        || merge.source_representations.empty()) {
        return std::unexpected(
            "GraphJit event merge references a missing representation");
    }
    auto const& target =
        event_ports.representations[merge.target_representation];
    if (!target.region.valid()
        || (target.event_capacity != 0
            && !is_power_of_2(target.event_capacity))) {
        return std::unexpected(
            "GraphJit event merge target has invalid physical storage");
    }
    if (target.persistent_ring && !merge.preserve_existing_target) {
        return std::unexpected(
            "GraphJit persistent event merge must preserve retained target events");
    }
    if (merge.target_is_semantic_source
        && (target.persistent_ring || merge.preserve_existing_target)) {
        return std::unexpected(
            "GraphJit transient event producer-home merge has inconsistent target storage");
    }

    auto& context = builder.getContext();
    auto* size_type = llvm::IntegerType::get(
        context, static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* target_events = byte_offset_pointer(
        builder,
        storage_base,
        target.events_storage_offset,
        "event.merge.target.events");

    llvm::Value* target_read = llvm::ConstantInt::get(size_type, 0);
    llvm::Value* target_write = llvm::ConstantInt::get(size_type, 0);
    llvm::Value* target_count_or_write_pointer = nullptr;
    if (target.persistent_ring) {
        auto* read_pointer = byte_offset_pointer(
            builder,
            storage_base,
            target.read_index_storage_offset,
            "event.merge.target.read.ptr");
        auto* write_pointer = byte_offset_pointer(
            builder,
            storage_base,
            target.write_index_storage_offset,
            "event.merge.target.write.ptr");
        target_read = builder.CreateLoad(
            size_type, read_pointer, "event.merge.target.read");
        target_write = builder.CreateLoad(
            size_type, write_pointer, "event.merge.target.write");
        target_count_or_write_pointer = write_pointer;
    } else {
        auto* count_pointer = byte_offset_pointer(
            builder,
            storage_base,
            target.count_storage_offset,
            "event.merge.target.count.ptr");
        if (merge.preserve_existing_target || merge.target_is_semantic_source) {
            target_write = builder.CreateLoad(
                size_type, count_pointer, "event.merge.target.count");
        }
        target_count_or_write_pointer = count_pointer;
    }

    auto validate_source = [&](std::size_t representation_index)
        -> std::expected<detail::EventRepresentationPlan const*, std::string> {
        if (representation_index >= event_ports.representations.size()) {
            return std::unexpected(
                "GraphJit event merge references a missing source representation");
        }
        auto const& source = event_ports.representations[representation_index];
        if (!source.region.valid() || source.persistent_ring
            || source.type != target.type) {
            return std::unexpected(
                "GraphJit event merge source has inconsistent physical storage");
        }
        return &source;
    };

    if (merge.target_is_semantic_source) {
        auto const source_count = merge.source_representations.size();
        auto* source_event_pointers = builder.CreateAlloca(
            pointer_type,
            llvm::ConstantInt::get(size_type, source_count),
            "event.merge.sources.ptrs");
        auto* source_remaining = builder.CreateAlloca(
            size_type,
            llvm::ConstantInt::get(size_type, source_count),
            "event.merge.sources.remaining");

        for (std::size_t source_index = 0;
             source_index < source_count; ++source_index) {
            auto const representation_index =
                merge.source_representations[source_index];
            auto source = validate_source(representation_index);
            if (!source) return std::unexpected(std::move(source.error()));

            auto* source_count_pointer = byte_offset_pointer(
                builder,
                storage_base,
                (*source)->count_storage_offset,
                "event.merge.source.count.ptr." + std::to_string(source_index));
            auto* source_count_value = builder.CreateLoad(
                size_type,
                source_count_pointer,
                "event.merge.source.count." + std::to_string(source_index));
            auto* source_events = byte_offset_pointer(
                builder,
                storage_base,
                (*source)->events_storage_offset,
                "event.merge.source.events." + std::to_string(source_index));

            auto* source_pointer_slot = builder.CreateInBoundsGEP(
                pointer_type,
                source_event_pointers,
                llvm::ConstantInt::get(size_type, source_index),
                "event.merge.source.ptr.slot." + std::to_string(source_index));
            builder.CreateStore(source_events, source_pointer_slot);
            auto* source_count_slot = builder.CreateInBoundsGEP(
                size_type,
                source_remaining,
                llvm::ConstantInt::get(size_type, source_index),
                "event.merge.source.remaining.slot." + std::to_string(source_index));
            builder.CreateStore(source_count_value, source_count_slot);
        }

        auto* helper_type = llvm::FunctionType::get(
            size_type,
            {pointer_type, size_type, size_type,
             pointer_type, pointer_type, size_type},
            false);
        auto* module = builder.GetInsertBlock()->getModule();
        auto helper = module->getOrInsertFunction(
            detail::event_sequence_k_way_merge_symbol, helper_type);
        auto* merged_count = builder.CreateCall(
            helper,
            {target_events,
             llvm::ConstantInt::get(size_type, target.event_capacity),
             target_write,
             source_event_pointers,
             source_remaining,
             llvm::ConstantInt::get(size_type, source_count)},
            "event.merge.kway.count");
        builder.CreateStore(merged_count, target_count_or_write_pointer);
        return {};
    }

    auto* helper_type = llvm::FunctionType::get(
        size_type,
        {pointer_type, size_type, size_type, size_type,
         pointer_type, size_type, size_type},
        false);
    auto* module = builder.GetInsertBlock()->getModule();
    auto helper = module->getOrInsertFunction(
        detail::event_sequence_merge_symbol, helper_type);

    for (std::size_t source_index = 0;
         source_index < merge.source_representations.size(); ++source_index) {
        auto const representation_index =
            merge.source_representations[source_index];
        auto source = validate_source(representation_index);
        if (!source) return std::unexpected(std::move(source.error()));
        auto* source_count_pointer = byte_offset_pointer(
            builder,
            storage_base,
            (*source)->count_storage_offset,
            "event.merge.source.count.ptr." + std::to_string(source_index));
        auto* source_count = builder.CreateLoad(
            size_type,
            source_count_pointer,
            "event.merge.source.count." + std::to_string(source_index));
        auto* source_events = byte_offset_pointer(
            builder,
            storage_base,
            (*source)->events_storage_offset,
            "event.merge.source.events." + std::to_string(source_index));
        target_write = builder.CreateCall(
            helper,
            {target_events,
             llvm::ConstantInt::get(size_type, target.event_capacity),
             target_read,
             target_write,
             source_events,
             llvm::ConstantInt::get(size_type, (*source)->event_capacity),
             source_count},
            "event.merge.write." + std::to_string(source_index));
    }
    builder.CreateStore(target_write, target_count_or_write_pointer);
    return {};
}

std::expected<void, std::string> emit_event_materialization(
    llvm::IRBuilder<>& builder,
    detail::EventPortBindingPlan const& event_ports,
    detail::EventMaterializationPlan const& materialization,
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size)
{
    if (materialization.source_representation >= event_ports.representations.size()
        || materialization.target_representation >= event_ports.representations.size()) {
        return std::unexpected(
            "GraphJit event materialization references a missing representation");
    }
    auto const& source =
        event_ports.representations[materialization.source_representation];
    auto const& target =
        event_ports.representations[materialization.target_representation];
    if (!source.region.valid() || !target.region.valid()) {
        return std::unexpected(
            "GraphJit event materialization references unfinalized storage");
    }
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
    auto* target_count_pointer = byte_offset_pointer(
        builder,
        storage_base,
        target.count_storage_offset,
        "event.materialize.target.count");
    auto* source_events = byte_offset_pointer(
        builder,
        storage_base,
        source.events_storage_offset,
        "event.materialize.source.events");
    auto* target_events = byte_offset_pointer(
        builder,
        storage_base,
        target.events_storage_offset,
        "event.materialize.target.events");

    if (materialization.select_root_window || source.persistent_ring) {
        llvm::Value* source_read_index = llvm::ConstantInt::get(size_type, 0);
        llvm::Value* source_write_index = nullptr;
        if (source.persistent_ring) {
            auto* read_index_pointer = byte_offset_pointer(
                builder,
                storage_base,
                source.read_index_storage_offset,
                "event.materialize.source.read.index");
            auto* write_index_pointer = byte_offset_pointer(
                builder,
                storage_base,
                source.write_index_storage_offset,
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
                storage_base,
                source.count_storage_offset,
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
             plan_word_type, size_type, pointer_type, size_type, size_type,
             size_type, sample_index->getType(), size_type, size_type,
             pointer_type, size_type},
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
             llvm::ConstantInt::get(size_type, source.event_capacity),
             source_read_index,
             source_write_index,
             sample_index,
             block_size,
             llvm::ConstantInt::get(size_type, materialization.history_samples),
             target_events,
             llvm::ConstantInt::get(size_type, target.event_capacity)},
            "event.materialize.windowed.count");
        builder.CreateStore(materialized_count, target_count_pointer);
        return {};
    }

    auto* source_count_pointer = byte_offset_pointer(
        builder,
        storage_base,
        source.count_storage_offset,
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
        builder.CreateStore(bounded_count, target_count_pointer);
        return {};
    }

    auto* plan_word_type = llvm::Type::getInt32Ty(context);
    auto* pointer_type = llvm::PointerType::getUnqual(context);
    auto* helper_type = llvm::FunctionType::get(
        size_type,
        {plan_word_type, plan_word_type, plan_word_type, plan_word_type,
         plan_word_type, size_type, pointer_type, size_type, pointer_type,
         size_type},
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
         llvm::ConstantInt::get(size_type, target.event_capacity)},
        "event.materialize.converted.count");
    builder.CreateStore(converted_count, target_count_pointer);
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
    llvm::Value* storage_base,
    llvm::Value* sample_index,
    llvm::Value* block_size,
    bool skip,
    bool allow_primitive_slicing,
    bool emit_sequence_resets,
    bool emit_ring_prunes)
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

    if (emit_ring_prunes) {
        for (auto const ring_index : step.event_persistent_ring_prunes_before) {
            if (ring_index >= plan.event_ports.persistent_rings.size()) {
                return std::unexpected(
                    "GraphJit execution plan references a missing persistent event ring");
            }
            auto pruned = emit_event_persistent_ring_prune(
                builder,
                plan.event_ports,
                plan.event_ports.persistent_rings[ring_index],
                storage_base,
                sample_index);
            if (!pruned) {
                return std::unexpected(std::move(pruned.error()));
            }
        }
    }

    if (emit_sequence_resets) {
        for (auto const representation_index : step.event_sequence_resets_before) {
            auto reset = emit_event_sequence_reset(
                builder,
                plan.event_ports,
                representation_index,
                storage_base);
            if (!reset) {
                return std::unexpected(std::move(reset.error()));
            }
        }
    }

    for (auto const carry_index : step.event_carry_restores_before) {
        if (carry_index >= plan.event_ports.carry_operations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing event carry restore");
        }
        auto restored = emit_event_carry_operation(
            builder,
            plan.event_ports,
            plan.event_ports.carry_operations[carry_index],
            storage_base,
            sample_index,
            block_size,
            true);
        if (!restored) {
            return std::unexpected(std::move(restored.error()));
        }
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

    for (auto const materialization_index : step.sample_materializations_before) {
        if (materialization_index
            >= plan.sample_ports.physical.materializations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing pre-sample materialization");
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
    if (allow_primitive_slicing
        && step.maximum_block_size < plan.declarations.node_layout.max_block_size) {
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

    for (auto const feedback_index : step.event_feedback_appends_after) {
        if (feedback_index >= plan.event_ports.feedback_operations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing event feedback append");
        }
        if (feedback_index >= event_feedback_cursors.size()) {
            return std::unexpected(
                "GraphJit execution plan has no cursor for an event feedback append");
        }
        auto appended = emit_event_feedback_append(
            builder,
            plan.event_ports,
            plan.event_ports.feedback_operations[feedback_index],
            event_feedback_cursors[feedback_index],
            storage_base,
            sample_index);
        if (!appended) {
            return std::unexpected(std::move(appended.error()));
        }
    }

    for (auto const merge_index : step.event_merges_after) {
        if (merge_index >= plan.event_ports.merges.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing event merge");
        }
        auto merged = emit_event_merge(
            builder,
            plan.event_ports,
            plan.event_ports.merges[merge_index],
            storage_base);
        if (!merged) {
            return std::unexpected(std::move(merged.error()));
        }
    }

    for (auto const materialization_index : step.event_materializations_after) {
        if (materialization_index >= plan.event_ports.materializations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing event materialization");
        }
        auto materialized = emit_event_materialization(
            builder,
            plan.event_ports,
            plan.event_ports.materializations[materialization_index],
            storage_base,
            sample_index,
            block_size);
        if (!materialized) {
            return std::unexpected(std::move(materialized.error()));
        }
    }

    for (auto const carry_index : step.event_carry_commits_after) {
        if (carry_index >= plan.event_ports.carry_operations.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing event carry commit");
        }
        auto committed = emit_event_carry_operation(
            builder,
            plan.event_ports,
            plan.event_ports.carry_operations[carry_index],
            storage_base,
            sample_index,
            block_size,
            false);
        if (!committed) {
            return std::unexpected(std::move(committed.error()));
        }
    }

    for (auto const materialization_index : step.sample_materializations_after) {
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

    for (auto const timeline_index : step.sample_feedback_writes_after) {
        if (timeline_index >= plan.sample_ports.physical.feedback_timelines.size()) {
            return std::unexpected(
                "GraphJit execution plan references a missing sample feedback timeline");
        }
        auto written = emit_sample_feedback_timeline_write(
            builder,
            plan.sample_ports.physical,
            plan.sample_ports.physical.feedback_timelines[timeline_index],
            storage_base,
            sample_index,
            block_size);
        if (!written) {
            return std::unexpected(std::move(written.error()));
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

    auto* feedback_cursor_type = llvm::IntegerType::get(
        module.getContext(), static_cast<unsigned>(sizeof(std::size_t) * 8));
    auto* feedback_cursor_zero = llvm::ConstantInt::get(feedback_cursor_type, 0);
    std::vector<llvm::Value*> event_feedback_cursors;
    event_feedback_cursors.reserve(plan.event_ports.feedback_operations.size());
    for (std::size_t i = 0; i < plan.event_ports.feedback_operations.size(); ++i) {
        auto const& feedback = plan.event_ports.feedback_operations[i];
        if (feedback.source_representation
            >= plan.event_ports.representations.size()) {
            return std::unexpected(
                "GraphJit event feedback cursor references a missing source representation");
        }
        auto const& source =
            plan.event_ports.representations[feedback.source_representation];
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
                storage_base,
                source.write_index_storage_offset,
                "event.feedback.cursor.source.write." + std::to_string(i));
            initial_cursor = builder.CreateLoad(
                feedback_cursor_type,
                source_write_pointer,
                "event.feedback.cursor.initial." + std::to_string(i));
        }
        builder.CreateStore(initial_cursor, cursor);
        event_feedback_cursors.push_back(cursor);
    }

    for (std::size_t region_index = 0;
         region_index < plan.execution.regions.size(); ++region_index) {
        auto const& region = plan.execution.regions[region_index];
        if (!region.cyclic) {
            for (auto const step_index : region.primitive_steps) {
                if (step_index >= plan.execution.primitive_steps.size()) {
                    return std::unexpected(
                        "GraphJit execution region references a missing primitive step");
                }
                auto emitted = emit_execution_step(
                    module,
                    builder,
                    plan,
                    configurations,
                    sample_bindings,
                    event_bindings,
                    plan.execution.primitive_steps[step_index],
                    event_feedback_cursors,
                    storage_base,
                    sample_index,
                    block_size,
                    skip,
                    true,
                    true,
                    true);
                if (!emitted) {
                    return std::unexpected(std::move(emitted.error()));
                }
            }
            continue;
        }

        if (region.maximum_block_size == 0
            || region.scc_feedback_latency == 0) {
            return std::unexpected(
                "GraphJit cyclic execution region has invalid slice semantics");
        }

        // Retained event state owned by an SCC producer spans the complete root
        // invocation. Advance/restore it once before entering the slice-major
        // loop. Feedback rings remain separate: they retire old events in the
        // producer append operation after all same-slice consumers have run.
        for (auto const ring_index : region.event_persistent_ring_prunes_before) {
            if (ring_index >= plan.event_ports.persistent_rings.size()) {
                return std::unexpected(
                    "GraphJit cyclic execution references a missing persistent event ring");
            }
            auto pruned = emit_event_persistent_ring_prune(
                builder,
                plan.event_ports,
                plan.event_ports.persistent_rings[ring_index],
                storage_base,
                sample_index);
            if (!pruned) {
                return std::unexpected(std::move(pruned.error()));
            }
        }
        for (auto const carry_index : region.event_carry_restores_before) {
            if (carry_index >= plan.event_ports.carry_operations.size()) {
                return std::unexpected(
                    "GraphJit cyclic execution references a missing event carry restore");
            }
            auto const& carry = plan.event_ports.carry_operations[carry_index];
            auto restored = emit_event_carry_operation(
                builder,
                plan.event_ports,
                carry,
                storage_base,
                sample_index,
                block_size,
                true);
            if (!restored) {
                return std::unexpected(std::move(restored.error()));
            }
            // The restored prefix exists only to satisfy retained outbound
            // windows. Detached feedback branches must append only events
            // authored during this root call, not enqueue that history again.
            auto seeded = seed_event_feedback_cursors_after_carry_restore(
                builder,
                plan.event_ports,
                carry,
                event_feedback_cursors,
                storage_base);
            if (!seeded) {
                return std::unexpected(std::move(seeded.error()));
            }
        }

        // Aggregate transient event producer sequences also belong to the
        // complete root call, not an individual SCC slice. Clear each once
        // before entering the slice-major loop.
        for (auto const step_index : region.primitive_steps) {
            if (step_index >= plan.execution.primitive_steps.size()) {
                return std::unexpected(
                    "GraphJit cyclic execution region references a missing primitive step");
            }
            auto const& step = plan.execution.primitive_steps[step_index];
            for (auto const representation_index : step.event_sequence_resets_before) {
                auto reset = emit_event_sequence_reset(
                    builder,
                    plan.event_ports,
                    representation_index,
                    storage_base);
                if (!reset) {
                    return std::unexpected(std::move(reset.error()));
                }
            }
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
                sample_bindings,
                event_bindings,
                step,
                event_feedback_cursors,
                storage_base,
                slice_index,
                slice_size,
                skip,
                false,
                false,
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

        // A cyclic producer's aggregate event sequence represents the complete
        // root invocation. Materializations feeding downstream regions therefore
        // execute once here, with the root index/size, after all SCC slices have
        // appended to that aggregate.
        for (auto const materialization_index :
             region.event_materializations_after) {
            if (materialization_index >= plan.event_ports.materializations.size()) {
                return std::unexpected(
                    "GraphJit cyclic execution region references a missing event materialization");
            }
            auto materialized = emit_event_materialization(
                builder,
                plan.event_ports,
                plan.event_ports.materializations[materialization_index],
                storage_base,
                sample_index,
                block_size);
            if (!materialized) {
                return std::unexpected(std::move(materialized.error()));
            }
        }
        for (auto const carry_index : region.event_carry_commits_after) {
            if (carry_index >= plan.event_ports.carry_operations.size()) {
                return std::unexpected(
                    "GraphJit cyclic execution references a missing event carry commit");
            }
            auto committed = emit_event_carry_operation(
                builder,
                plan.event_ports,
                plan.event_ports.carry_operations[carry_index],
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

    return LoweringOutput{
        .node_layout = std::move(plan.declarations.node_layout),
        .root_symbols = {
            .tick_block = std::string(root_tick_block_symbol),
        },
    };
}
} // namespace

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput& input,
    llvm::Module& output_module)
{
    if (output_module.getNamedValue(root_tick_block_symbol)) {
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
