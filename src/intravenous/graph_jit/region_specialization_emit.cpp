#include <intravenous/graph_jit/region_specialization_emit.h>

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/package_callback_abi.h>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalIFunc.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/KnownBits.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <tuple>

namespace iv::graph_jit {
namespace {

constexpr auto input_binding_size = sizeof(ReflectedContinuousInputPortBinding);
constexpr auto output_binding_size = sizeof(ReflectedContinuousOutputPortBinding);
constexpr auto channel_binding_size = sizeof(ReflectedContinuousChannelStorageBinding);

// Both binding records begin with their ContinuousPortStorageBinding, whose
// channel array comes first. The fields after the channel array are the tail.
constexpr auto input_storage_offset =
    offsetof(ReflectedContinuousInputPortBinding, storage);
constexpr auto output_storage_offset =
    offsetof(ReflectedContinuousOutputPortBinding, storage);
constexpr auto channels_offset = offsetof(ContinuousPortStorageBinding, channels);
constexpr auto input_tail_offset =
    input_storage_offset + offsetof(ContinuousPortStorageBinding, geometry);
constexpr auto output_tail_offset =
    output_storage_offset + offsetof(ContinuousPortStorageBinding, geometry);

constexpr auto span_pointer_offset = offsetof(ReflectedSpan<std::byte>, pointer);
constexpr unsigned region_specialization_workspace_argument = 5;

std::size_t channel_field(
    std::size_t storage_offset,
    std::size_t channel,
    std::size_t field) noexcept
{
    return storage_offset + channels_offset + channel * channel_binding_size + field;
}

template<class T>
T read_bytes(std::span<std::byte const> bytes, std::size_t offset)
{
    T record{};
    std::memcpy(&record, bytes.data() + offset, sizeof(T));
    return record;
}

// size bytes of value repeated from its first byte.
std::vector<std::byte> repeated(std::span<std::byte const> value, std::size_t size)
{
    std::vector<std::byte> result(size);
    for (std::size_t offset = 0; offset < size; ++offset) {
        result[offset] = value[offset % value.size()];
    }
    return result;
}

class SpecializationEmitter {
    llvm::Module& module_;
    llvm::LLVMContext& context_;
    llvm::IRBuilder<> builder_;
    llvm::Type* i8_;
    llvm::Type* i64_;
    llvm::PointerType* pointer_;

public:
    explicit SpecializationEmitter(llvm::Module& module)
        : module_(module)
        , context_(module.getContext())
        , builder_(module.getContext())
        , i8_(llvm::Type::getInt8Ty(module.getContext()))
        , i64_(llvm::Type::getInt64Ty(module.getContext()))
        , pointer_(llvm::PointerType::getUnqual(module.getContext()))
    {}

    llvm::IRBuilder<>& builder() noexcept { return builder_; }
    llvm::Type* i64() const noexcept { return i64_; }
    llvm::PointerType* pointer() const noexcept { return pointer_; }

    llvm::Value* at(llvm::Value* base, std::size_t offset)
    {
        return offset == 0
            ? base
            : builder_.CreateConstInBoundsGEP1_64(i8_, base, offset);
    }

    llvm::Value* load_pointer(llvm::Value* base, std::size_t offset)
    {
        return builder_.CreateLoad(pointer_, at(base, offset));
    }

    void store(llvm::Value* value, llvm::Value* base, std::size_t offset)
    {
        builder_.CreateStore(value, at(base, offset));
    }

    llvm::GlobalVariable* constant_bytes(
        std::span<std::byte const> bytes,
        llvm::StringRef name)
    {
        auto* array = llvm::ConstantDataArray::get(
            context_,
            llvm::ArrayRef<std::uint8_t>(
                reinterpret_cast<std::uint8_t const*>(bytes.data()), bytes.size()));
        auto* global = new llvm::GlobalVariable(
            module_,
            array->getType(),
            true,
            llvm::GlobalValue::PrivateLinkage,
            array,
            name);
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        global->setAlignment(llvm::Align(alignof(std::max_align_t)));
        return global;
    }

    void copy(
        llvm::Value* destination,
        llvm::Value* source,
        std::size_t offset,
        std::size_t size)
    {
        if (size == 0) return;
        builder_.CreateMemCpy(
            at(destination, offset), llvm::MaybeAlign(1),
            at(source, offset), llvm::MaybeAlign(1),
            size);
    }
};

// Mirrors what the emitted entry block writes into the workspace.
class WorkspaceRecorder {
    RegionSpecializationWorkspaceContents& contents_;

    void forget_pointers(std::size_t offset, std::size_t size)
    {
        std::erase_if(contents_.pointers, [&](auto const& field) {
            return field.offset < offset + size && offset < field.offset + sizeof(void*);
        });
    }

public:
    RegionSpecializationWorkspaceContents& contents() noexcept { return contents_; }

    WorkspaceRecorder(RegionSpecializationWorkspaceContents& contents, std::size_t size)
        : contents_(contents)
    {
        contents_.bytes.assign(size, std::byte{});
        contents_.known.assign(size, 0);
        contents_.pointers.clear();
    }

    void bytes(std::size_t offset, std::span<std::byte const> source)
    {
        forget_pointers(offset, source.size());
        std::ranges::copy(source, contents_.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        std::fill_n(contents_.known.begin() + static_cast<std::ptrdiff_t>(offset),
            source.size(), std::uint8_t{1});
    }

    void null_pointer(std::size_t offset)
    {
        std::array<std::byte, sizeof(void*)> zero{};
        bytes(offset, zero);
    }

    void unknown(std::size_t offset, std::size_t size)
    {
        forget_pointers(offset, size);
        std::fill_n(contents_.known.begin() + static_cast<std::ptrdiff_t>(offset),
            size, std::uint8_t{0});
    }

    void workspace_pointer(std::size_t offset, std::size_t target)
    {
        unknown(offset, sizeof(void*));
        contents_.pointers.push_back({.offset = offset, .workspace = true, .target = target});
    }

    void constant_pointer(std::size_t offset, std::span<std::byte const> value)
    {
        unknown(offset, sizeof(void*));
        contents_.pointers.push_back({
            .offset = offset,
            .constant = std::vector<std::byte>(value.begin(), value.end()),
        });
    }
};

struct LocalStep {
    llvm::Value* node_data = nullptr;
    llvm::Value* context = nullptr;
    llvm::Value* inputs = nullptr;
    llvm::Value* outputs = nullptr;
    std::size_t inputs_offset = 0;
    std::size_t outputs_offset = 0;
    std::size_t context_offset = 0;
    llvm::Value* operations_before = nullptr;
    llvm::Value* operations_after = nullptr;
    llvm::Value* primitive_index = nullptr;
    llvm::FunctionCallee tick_block{};
    // Per begin index of the step, its pace cursor value for this call.
    std::vector<llvm::Value*> begin_bases{};
};

} // namespace

std::expected<RegionSpecializationWorkspaceContents, std::string>
emit_region_specialization(
    llvm::Module& module,
    RegionSpecializationRequest const& request,
    std::string const& function_name,
    std::span<std::string const> tick_blocks)
{
    if (!region_specialization_request_is_complete(request)
        || tick_blocks.size() != request.steps.size()) {
        return std::unexpected("GraphJit region specialization request is incomplete");
    }
    auto const required_workspace = region_specialization_workspace_size(request);
    if (!required_workspace) {
        return std::unexpected("GraphJit region specialization workspace overflows");
    }
    if (module.getNamedValue(function_name)) {
        return std::unexpected("GraphJit region specialization name collides");
    }
    auto& context = module.getContext();
    SpecializationEmitter emit(module);
    auto& builder = emit.builder();
    auto* i8 = llvm::Type::getInt8Ty(context);
    auto* i64 = emit.i64();
    auto* pointer = emit.pointer();

    auto* type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {pointer, pointer, pointer, pointer, i64, pointer, i64, i64,
         pointer, pointer, pointer, pointer},
        false);
    auto* operations_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {pointer, pointer, i64, i64, i64, i64},
        false);
    auto* manual_controls_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context),
        {pointer, i64, i64, i64, i64, i64},
        false);
    auto* mark_sample_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {pointer, i64}, false);
    auto* function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, function_name, module);
    function->addFnAttr(llvm::Attribute::NoUnwind);
    auto* steps = function->getArg(0);
    auto* begin_bases = function->getArg(1);
    auto* sample_representations = function->getArg(2);
    auto* sample_representation_indices = function->getArg(3);
    auto* project_begin_index = function->getArg(4);
    auto* workspace = function->getArg(
        region_specialization_workspace_argument);
    auto* root_index = function->getArg(6);
    auto* block_size = function->getArg(7);
    auto* frame = function->getArg(8);
    auto* run_operations = function->getArg(9);
    auto* stage_manual_controls = function->getArg(10);
    auto* mark_sample = function->getArg(11);
    steps->setName("steps");
    begin_bases->setName("begin_bases");
    sample_representations->setName("sample_representations");
    sample_representation_indices->setName("sample_representation_indices");
    project_begin_index->setName("project_begin_index");
    workspace->setName("workspace");
    root_index->setName("root_index");
    block_size->setName("block_size");
    frame->setName("frame");
    run_operations->setName("run_operations");
    stage_manual_controls->setName("stage_manual_controls");
    mark_sample->setName("mark_sample");

    auto* entry = llvm::BasicBlock::Create(context, "entry", function);
    builder.SetInsertPoint(entry);

    std::vector<LocalStep> locals;
    locals.reserve(request.steps.size());
    std::size_t begin_index = 0;
    std::size_t workspace_offset = 0;
    RegionSpecializationWorkspaceContents contents;
    WorkspaceRecorder written(contents, *required_workspace);
    auto local_bytes = [&](std::size_t size) -> std::pair<llvm::Value*, std::size_t> {
        auto const alignment = alignof(std::max_align_t);
        auto const remainder = workspace_offset % alignment;
        if (remainder != 0) workspace_offset += alignment - remainder;
        auto const offset = workspace_offset;
        workspace_offset += std::max<std::size_t>(size, 1);
        return {emit.at(workspace, offset), offset};
    };
    for (std::size_t index = 0; index < request.steps.size(); ++index) {
        auto const& step = request.steps[index];
        auto const input_count = step.input_channel_counts.size();
        auto const output_count = step.output_channel_counts.size();
        LocalStep local;

        auto* record = emit.at(steps, index * sizeof(RegionSpecializationStep));
        local.node_data = emit.load_pointer(
            record, offsetof(RegionSpecializationStep, node_data));
        auto* prepared = emit.load_pointer(
            record, offsetof(RegionSpecializationStep, context));
        if (!step.operations_before.empty()) {
            local.operations_before = emit.load_pointer(
                record, offsetof(RegionSpecializationStep, operations_before));
        }
        if (!step.operations_after.empty()) {
            local.operations_after = emit.load_pointer(
                record, offsetof(RegionSpecializationStep, operations_after));
        }
        if (step.stages_manual_controls) {
            local.primitive_index = builder.CreateLoad(
                i64, emit.at(record, offsetof(RegionSpecializationStep, primitive_index)));
        }
        auto* prepared_inputs = emit.load_pointer(
            prepared,
            offsetof(ReflectedNodeTickContext, continuous_input_bindings)
                + span_pointer_offset);
        auto* prepared_outputs = emit.load_pointer(
            prepared,
            offsetof(ReflectedNodeTickContext, continuous_output_bindings)
                + span_pointer_offset);
        auto* prepared_state = emit.load_pointer(
            prepared, offsetof(ReflectedNodeTickContext, state) + span_pointer_offset);

        auto* input_template = emit.constant_bytes(step.inputs, "iv.region.inputs");
        auto* output_template = emit.constant_bytes(step.outputs, "iv.region.outputs");
        auto* context_template = emit.constant_bytes(step.context, "iv.region.context");
        std::tie(local.inputs, local.inputs_offset) = local_bytes(step.inputs.size());
        std::tie(local.outputs, local.outputs_offset) = local_bytes(step.outputs.size());
        std::tie(local.context, local.context_offset) = local_bytes(step.context.size());
        auto const inputs_bytes = std::span<std::byte const>{step.inputs};
        auto const outputs_bytes = std::span<std::byte const>{step.outputs};

        // Only the bound channels and the fields after the channel array are
        // copied; node code reads no other channel entry.
        for (std::size_t input = 0; input < input_count; ++input) {
            auto const base = input * input_binding_size;
            auto const channels = step.input_channel_counts[input];
            emit.copy(local.inputs, input_template, base, channels * channel_binding_size);
            emit.copy(
                local.inputs,
                input_template,
                base + input_tail_offset,
                input_binding_size - input_tail_offset);
            written.bytes(
                local.inputs_offset + base,
                inputs_bytes.subspan(base, channels * channel_binding_size));
            written.bytes(
                local.inputs_offset + base + input_tail_offset,
                inputs_bytes.subspan(
                    base + input_tail_offset, input_binding_size - input_tail_offset));
            for (std::size_t channel = 0; channel < channels; ++channel) {
                for (auto const offset : {
                         offsetof(ContinuousChannelStorageBinding, storage),
                         offsetof(ContinuousChannelStorageBinding, contiguous_scratch)}) {
                    auto const field = base + channel_field(input_storage_offset, channel, offset);
                    emit.store(
                        emit.load_pointer(prepared_inputs, field), local.inputs, field);
                    written.unknown(local.inputs_offset + field, sizeof(void*));
                }
            }
            auto const immutable = std::ranges::find(
                step.immutable_inputs,
                input,
                &RegionSpecializationImmutableInput::binding);
            if (immutable == step.immutable_inputs.end()) continue;
            // The input keeps its interpreter binding: ring, capacity, delay,
            // and contiguous scratch. Each channel's ring holds the one value
            // at every position, which only the recorded contents state.
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const storage_field = base + channel_field(
                    input_storage_offset,
                    channel,
                    offsetof(ContinuousChannelStorageBinding, storage));
                auto const ring = read_bytes<ContinuousChannelStorageBinding>(
                    inputs_bytes,
                    base + channel_field(input_storage_offset, channel, 0));
                written.constant_pointer(
                    local.inputs_offset + storage_field,
                    repeated(immutable->value, ring.value_capacity * ring.value_stride_bytes));
            }
        }
        for (std::size_t output = 0; output < output_count; ++output) {
            auto const base = output * output_binding_size;
            auto const channels = step.output_channel_counts[output];
            emit.copy(local.outputs, output_template, base, channels * channel_binding_size);
            emit.copy(
                local.outputs,
                output_template,
                base + output_tail_offset,
                output_binding_size - output_tail_offset);
            written.bytes(
                local.outputs_offset + base,
                outputs_bytes.subspan(base, channels * channel_binding_size));
            written.bytes(
                local.outputs_offset + base + output_tail_offset,
                outputs_bytes.subspan(
                    base + output_tail_offset, output_binding_size - output_tail_offset));
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const field = base + channel_field(
                    output_storage_offset,
                    channel,
                    offsetof(ContinuousChannelStorageBinding, storage));
                emit.store(emit.load_pointer(prepared_outputs, field), local.outputs, field);
                written.unknown(local.outputs_offset + field, sizeof(void*));
            }
        }
        emit.copy(local.context, context_template, 0, step.context.size());
        written.bytes(local.context_offset, step.context);
        auto span_pointer = [&](std::size_t field, llvm::Value* target, std::size_t target_offset) {
            auto const offset = field + span_pointer_offset;
            emit.store(
                target ? target
                       : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(pointer)),
                local.context,
                offset);
            if (target) {
                written.workspace_pointer(local.context_offset + offset, target_offset);
            } else {
                written.null_pointer(local.context_offset + offset);
            }
        };
        span_pointer(
            offsetof(ReflectedNodeTickContext, continuous_input_bindings),
            input_count != 0 ? local.inputs : nullptr,
            local.inputs_offset);
        span_pointer(
            offsetof(ReflectedNodeTickContext, continuous_output_bindings),
            output_count != 0 ? local.outputs : nullptr,
            local.outputs_offset);
        emit.store(
            prepared_state,
            local.context,
            offsetof(ReflectedNodeTickContext, state) + span_pointer_offset);
        written.unknown(
            local.context_offset + offsetof(ReflectedNodeTickContext, state)
                + span_pointer_offset,
            sizeof(void*));

        // The reflected ABI's mutable fields change during the call.
        for (std::size_t input = 0; input < input_count; ++input) {
            auto const base = local.inputs_offset + input * input_binding_size;
            for (auto const [offset, size] : {
                     std::pair{offsetof(ReflectedContinuousInputPortBinding, sequential_begin_index),
                         sizeof(std::uint64_t)},
                     std::pair{offsetof(ReflectedContinuousInputPortBinding, contiguous_begin_index),
                         sizeof(std::uint64_t)},
                     std::pair{offsetof(ReflectedContinuousInputPortBinding, contiguous_value_count),
                         sizeof(std::size_t)}}) {
                written.unknown(base + offset, size);
            }
            for (std::size_t channel = 0; channel < step.input_channel_counts[input]; ++channel) {
                written.unknown(
                    base + channel_field(
                        input_storage_offset,
                        channel,
                        offsetof(ContinuousChannelStorageBinding, contiguous_storage)),
                    sizeof(void*));
            }
        }
        for (std::size_t output = 0; output < output_count; ++output) {
            auto const base = local.outputs_offset + output * output_binding_size;
            written.unknown(
                base + offsetof(ReflectedContinuousOutputPortBinding, sequential_begin_index),
                sizeof(std::uint64_t));
            written.unknown(
                base + offsetof(ReflectedContinuousOutputPortBinding, disposition),
                sizeof(OutputDisposition));
        }

        for (std::size_t update = 0; update < step.begin_indices.size(); ++update) {
            local.begin_bases.push_back(builder.CreateLoad(
                i64, emit.at(begin_bases, begin_index++ * sizeof(std::uint64_t))));
        }
        local.tick_block = module.getOrInsertFunction(
            tick_blocks[index],
            package_callback_type(context, CallbackImportAbi::block));
        locals.push_back(std::move(local));
    }
    if (workspace_offset != *required_workspace) {
        return std::unexpected(
            "GraphJit region specialization workspace layout disagrees");
    }

    std::vector<llvm::Value*> local_sample_representations;
    std::vector<llvm::Value*> local_sample_representation_indices;
    local_sample_representations.reserve(request.sample_representation_count);
    local_sample_representation_indices.reserve(
        request.sample_representation_count);
    for (std::size_t representation = 0;
         representation < request.sample_representation_count;
         ++representation) {
        local_sample_representations.push_back(emit.load_pointer(
            sample_representations, representation * sizeof(void*)));
        local_sample_representation_indices.push_back(builder.CreateLoad(
            i64,
            emit.at(
                sample_representation_indices,
                representation * sizeof(std::size_t))));
    }

    auto* float_type = llvm::Type::getFloatTy(context);
    auto sample_value = [&](RegionSpecializationSampleAddress const& address,
                            llvm::Value* frame_index,
                            std::size_t channel,
                            std::size_t lane) {
        auto* base = local_sample_representations[address.representation];
        auto* slot = builder.CreateAnd(
            frame_index,
            llvm::ConstantInt::get(i64, address.frame_capacity - 1));
        llvm::Value* value_index = nullptr;
        if (address.planar) {
            value_index = builder.CreateAdd(
                slot,
                llvm::ConstantInt::get(
                    i64, channel * address.frame_capacity));
        } else {
            value_index = builder.CreateAdd(
                builder.CreateMul(
                    slot,
                    llvm::ConstantInt::get(i64, address.channel_count)),
                llvm::ConstantInt::get(i64, channel));
        }
        auto* byte_offset = builder.CreateAdd(
            builder.CreateMul(
                value_index,
                llvm::ConstantInt::get(i64, address.value_size_bytes)),
            llvm::ConstantInt::get(i64, lane * sizeof(float)));
        return builder.CreateInBoundsGEP(i8, base, byte_offset);
    };
    auto emit_composition = [&](RegionSpecializationSampleComposition const& composition,
                                llvm::Value* begin,
                                llvm::Value* count) {
        auto* preheader = builder.GetInsertBlock();
        auto* loop = llvm::BasicBlock::Create(context, "composition", function);
        auto* body = llvm::BasicBlock::Create(context, "composition.body", function);
        auto* done = llvm::BasicBlock::Create(context, "composition.done", function);
        builder.CreateBr(loop);

        builder.SetInsertPoint(loop);
        auto* offset = builder.CreatePHI(i64, 2, "composition.offset");
        offset->addIncoming(llvm::ConstantInt::get(i64, 0), preheader);
        auto* total = builder.CreateAdd(
            count,
            llvm::ConstantInt::get(i64, composition.target_history));
        builder.CreateCondBr(builder.CreateICmpULT(offset, total), body, done);

        builder.SetInsertPoint(body);
        auto* first = builder.CreateSub(
            begin,
            llvm::ConstantInt::get(i64, composition.target_history));
        auto* target_frame = builder.CreateAdd(first, offset);
        for (auto const& contribution : composition.contributions) {
            for (std::size_t lane = 0;
                 lane < composition.scalar_lane_count;
                 ++lane) {
                std::vector<llvm::Value*> sources;
                sources.reserve(contribution.sources.size());
                for (auto const& source : contribution.sources) {
                    auto* source_frame = builder.CreateSub(
                        target_frame,
                        llvm::ConstantInt::get(i64, source.read_latency));
                    auto* load = builder.CreateLoad(
                        float_type,
                        sample_value(
                            source.address,
                            source_frame,
                            source.channel,
                            lane));
                    load->setAlignment(llvm::Align(alignof(float)));
                    sources.push_back(load);
                }
                for (std::size_t converted = 0;
                     converted < contribution.target_channels.size();
                     ++converted) {
                    auto const term_begin = contribution.term_begins[converted];
                    auto const term_end = contribution.term_begins[converted + 1];
                    llvm::Value* mixed = llvm::ConstantFP::get(float_type, 0.0);
                    for (auto term = term_begin; term < term_end; ++term) {
                        auto const& selected = contribution.terms[term];
                        auto* value = sources[selected.source];
                        if (selected.coefficient != 1.0f) {
                            value = builder.CreateFMul(
                                value,
                                llvm::ConstantFP::get(
                                    float_type, selected.coefficient));
                        }
                        mixed = term == term_begin
                            ? value
                            : builder.CreateFAdd(mixed, value);
                    }
                    auto* store = builder.CreateStore(
                        mixed,
                        sample_value(
                            composition.target,
                            target_frame,
                            contribution.target_channels[converted],
                            lane));
                    store->setAlignment(llvm::Align(alignof(float)));
                }
            }
        }
        auto* next = builder.CreateAdd(offset, llvm::ConstantInt::get(i64, 1));
        offset->addIncoming(next, body);
        builder.CreateBr(loop);

        builder.SetInsertPoint(done);
        auto* mark = builder.CreateCall(
            mark_sample_type,
            mark_sample,
            {frame,
             local_sample_representation_indices[
                 composition.target.representation]});
        mark->setDoesNotThrow();
    };
    auto emit_operations = [&](std::span<RegionSpecializationOperation const> actions,
                               llvm::Value* root_operations,
                               llvm::Value* index,
                               llvm::Value* begin,
                               llvm::Value* count) {
        for (auto const& action : actions) {
            if (action.kind
                == RegionSpecializationOperationKind::root_operations) {
                auto* operation_call = builder.CreateCall(
                    operations_type,
                    run_operations,
                    {frame,
                     root_operations,
                     llvm::ConstantInt::get(i64, action.operation_begin),
                     llvm::ConstantInt::get(i64, action.operation_count),
                     index,
                     count});
                operation_call->setDoesNotThrow();
            } else {
                emit_composition(action.composition, begin, count);
            }
        }
    };

    if (!request.operations_per_slice) {
        auto const& step = request.steps.front();
        emit_operations(
            step.operations_before,
            locals.front().operations_before,
            root_index,
            project_begin_index,
            block_size);
    }

    auto* loop = llvm::BasicBlock::Create(context, "slice", function);
    auto* body = llvm::BasicBlock::Create(context, "slice.body", function);
    auto* done = llvm::BasicBlock::Create(context, "done", function);
    auto* slice_preheader = builder.GetInsertBlock();
    builder.CreateBr(loop);

    builder.SetInsertPoint(loop);
    auto* offset = builder.CreatePHI(i64, 2, "offset");
    offset->addIncoming(llvm::ConstantInt::get(i64, 0), slice_preheader);
    builder.CreateCondBr(builder.CreateICmpULT(offset, block_size), body, done);

    builder.SetInsertPoint(body);
    auto* quantum = llvm::ConstantInt::get(i64, request.quantum);
    auto* remaining = builder.CreateSub(block_size, offset, "remaining");
    auto* count = builder.CreateSelect(
        builder.CreateICmpULT(remaining, quantum), remaining, quantum, "count");
    auto* slice = builder.CreateAdd(root_index, offset, "slice");
    for (std::size_t index = 0; index < request.steps.size(); ++index) {
        auto const& step = request.steps[index];
        auto& local = locals[index];
        // A project-rate binding's slice begins at its cursor plus the
        // slice's offset in the root call (projected_prefix() at pace 0).
        for (std::size_t update = 0; update < step.begin_indices.size(); ++update) {
            auto const& binding = step.begin_indices[update];
            auto const field = binding.output
                ? binding.binding * output_binding_size
                    + offsetof(ReflectedContinuousOutputPortBinding, sequential_begin_index)
                : binding.binding * input_binding_size
                    + offsetof(ReflectedContinuousInputPortBinding, sequential_begin_index);
            emit.store(
                builder.CreateAdd(local.begin_bases[update], offset),
                binding.output ? local.outputs : local.inputs,
                field);
        }
        if (request.operations_per_slice) {
            emit_operations(
                step.operations_before,
                local.operations_before,
                slice,
                builder.CreateAdd(project_begin_index, offset),
                count);
        }
        if (step.stages_manual_controls) {
            auto* manual_controls_call = builder.CreateCall(
                manual_controls_type,
                stage_manual_controls,
                {frame,
                 local.primitive_index,
                 slice,
                 count,
                 root_index,
                 block_size});
            manual_controls_call->setDoesNotThrow();
        }
        auto* call = builder.CreateCall(
            local.tick_block, {local.node_data, local.context, slice, count});
        call->setDoesNotThrow();
        if (request.operations_per_slice) {
            emit_operations(
                step.operations_after,
                local.operations_after,
                slice,
                builder.CreateAdd(project_begin_index, offset),
                count);
        }
    }
    auto* next = builder.CreateAdd(offset, quantum, "next");
    offset->addIncoming(next, builder.GetInsertBlock());
    builder.CreateBr(loop);

    builder.SetInsertPoint(done);
    if (!request.operations_per_slice) {
        auto const& step = request.steps.front();
        emit_operations(
            step.operations_after,
            locals.front().operations_after,
            root_index,
            project_begin_index,
            block_size);
    }
    builder.CreateRetVoid();
    return contents;
}

std::expected<std::vector<std::pair<NodeCodeKey, std::string>>, std::string>
package_tick_block_symbols(llvm::Module& package)
{
    // The record layout that iv_package_finalize validates: operand 0 is the
    // NodeCodeKey, operand 1 the operations, whose operand 1 is tick_block.
    std::vector<std::pair<NodeCodeKey, std::string>> result;
    for (auto& global : package.globals()) {
        auto const section = global.getSection();
        if (section != "iv_node_types" && !section.ends_with("__iv_node_types")) {
            continue;
        }
        if (auto error = global.materialize()) {
            return std::unexpected(
                "cannot materialize iv_node_types record: "
                + llvm::toString(std::move(error)));
        }
        if (!global.hasInitializer()) {
            return std::unexpected("iv_node_types record has no initializer");
        }
        auto const* record = llvm::dyn_cast<llvm::ConstantStruct>(global.getInitializer());
        if (!record || record->getNumOperands() != 11) {
            return std::unexpected("malformed iv_node_types record in package bitcode");
        }
        auto const* key = llvm::dyn_cast<llvm::ConstantStruct>(record->getOperand(0));
        auto const* operations =
            llvm::dyn_cast<llvm::ConstantStruct>(record->getOperand(1));
        if (!key || key->getNumOperands() != 2 || !operations
            || operations->getNumOperands() != 13) {
            return std::unexpected("malformed iv_node_types record in package bitcode");
        }
        auto const* low = llvm::dyn_cast<llvm::ConstantInt>(key->getOperand(0));
        auto const* high = llvm::dyn_cast<llvm::ConstantInt>(key->getOperand(1));
        if (!low || !high) {
            return std::unexpected("malformed iv_node_types key in package bitcode");
        }
        auto const* callback = operations->getOperand(1);
        if (llvm::isa<llvm::ConstantPointerNull>(callback)) continue;
        auto const* tick_block = llvm::dyn_cast<llvm::Function>(
            callback->stripPointerCasts());
        if (!tick_block
            || (tick_block->isDeclaration() && !tick_block->isMaterializable())
            || !package_callback_type_matches(
                *tick_block, CallbackImportAbi::block)) {
            return std::unexpected(
                "iv_node_types tick_block has the wrong LLVM ABI");
        }
        result.emplace_back(
            NodeCodeKey{.low = low->getZExtValue(), .high = high->getZExtValue()},
            tick_block->getName().str());
    }
    std::ranges::sort(result, {}, &std::pair<NodeCodeKey, std::string>::first);
    if (std::ranges::adjacent_find(
            result, {}, &std::pair<NodeCodeKey, std::string>::first)
        != result.end()) {
        return std::unexpected("duplicate NodeCodeKey in iv_node_types records");
    }
    return result;
}

std::expected<std::vector<PackageRetainedGlobalDefinition>, std::string>
package_retained_global_definitions(llvm::Module& package)
{
    auto* accessor = package.getFunction("iv_package_retained_globals");
    if (!accessor
        || (accessor->isDeclaration() && !accessor->isMaterializable())) {
        return std::unexpected(
            "finalized package bitcode has no retained-global accessor");
    }
    if (auto error = accessor->materialize()) {
        return std::unexpected(
            "cannot materialize retained-global accessor in package bitcode: "
            + llvm::toString(std::move(error)));
    }

    llvm::Constant* data = nullptr;
    std::size_t extent = 0;
    bool found = false;
    for (auto& block : *accessor) {
        auto* returned = llvm::dyn_cast_or_null<llvm::ReturnInst>(
            block.getTerminator());
        if (!returned) continue;
        auto* view = llvm::dyn_cast_or_null<llvm::ConstantStruct>(
            returned->getReturnValue());
        auto* extent_value = view && view->getNumOperands() == 2
            ? llvm::dyn_cast<llvm::ConstantInt>(view->getOperand(1))
            : nullptr;
        if (!view || !extent_value || found
            || extent_value->getValue().getActiveBits() > 64
            || extent_value->getZExtValue()
                > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(
                "malformed retained-global accessor in package bitcode");
        }
        data = view->getOperand(0);
        extent = static_cast<std::size_t>(extent_value->getZExtValue());
        found = true;
    }
    if (!found || !data) {
        return std::unexpected(
            "malformed retained-global accessor in package bitcode");
    }
    if (llvm::isa<llvm::ConstantPointerNull>(data)) {
        if (extent != 0) {
            return std::unexpected(
                "empty retained-global accessor has a non-zero extent");
        }
        return std::vector<PackageRetainedGlobalDefinition>{};
    }

    auto* table = llvm::dyn_cast<llvm::GlobalVariable>(
        data->stripPointerCasts());
    if (table) {
        if (auto error = table->materialize()) {
            return std::unexpected(
                "cannot materialize retained-global table in package bitcode: "
                + llvm::toString(std::move(error)));
        }
    }
    auto const* entries = table && table->hasInitializer()
        ? llvm::dyn_cast<llvm::ConstantArray>(table->getInitializer())
        : nullptr;
    if (!entries) {
        return std::unexpected(
            "malformed retained-global table in package bitcode");
    }
    auto const entry_size = package.getDataLayout().getTypeAllocSize(
        entries->getType()->getElementType());
    if (entry_size.isScalable()
        || entries->getNumOperands() > std::numeric_limits<std::size_t>::max()
            / entry_size.getFixedValue()
        || extent != entries->getNumOperands() * entry_size.getFixedValue()) {
        return std::unexpected(
            "retained-global table extent disagrees with its package data");
    }

    std::vector<PackageRetainedGlobalDefinition> result(entries->getNumOperands());
    std::vector<bool> assigned(entries->getNumOperands());
    for (unsigned position = 0; position < entries->getNumOperands(); ++position) {
        auto const* entry = llvm::dyn_cast<llvm::ConstantStruct>(
            entries->getOperand(position));
        auto const* size = entry && entry->getNumOperands() == 3
            ? llvm::dyn_cast<llvm::ConstantInt>(entry->getOperand(1))
            : nullptr;
        auto const* index = entry && entry->getNumOperands() == 3
            ? llvm::dyn_cast<llvm::ConstantInt>(entry->getOperand(2))
            : nullptr;
        if (!entry || !size || !index
            || size->getValue().getActiveBits() > 64
            || index->getValue().getActiveBits() > 64
            || size->getZExtValue() > std::numeric_limits<std::size_t>::max()
            || index->getZExtValue() >= result.size()) {
            return std::unexpected(
                "malformed retained-global record in package bitcode");
        }
        auto const result_index = static_cast<std::size_t>(index->getZExtValue());
        if (assigned[result_index]) {
            return std::unexpected(
                "duplicate retained-global index in package bitcode");
        }
        assigned[result_index] = true;
        auto const* global = llvm::dyn_cast<llvm::GlobalVariable>(
            entry->getOperand(0)->stripPointerCasts());
        result[result_index] = PackageRetainedGlobalDefinition{
            .symbol = global ? global->getName().str() : std::string{},
            .size = static_cast<std::size_t>(size->getZExtValue()),
        };
    }
    if (!std::ranges::all_of(assigned, [](bool value) { return value; })) {
        return std::unexpected(
            "retained-global table has a missing index in package bitcode");
    }
    return result;
}

void internalize_region_specialization(
    llvm::Module& module,
    std::string const& function_name)
{
    for (auto const* name : {"llvm.global_ctors", "llvm.global_dtors", "llvm.used",
             "llvm.compiler.used"}) {
        if (auto* list = module.getNamedGlobal(name)) list->eraseFromParent();
    }
    for (auto& function : module) {
        if (function.isDeclaration() || function.getName() == function_name) continue;
        function.setLinkage(llvm::GlobalValue::InternalLinkage);
        function.setVisibility(llvm::GlobalValue::DefaultVisibility);
        function.setComdat(nullptr);
        if (!function.hasFnAttribute(llvm::Attribute::NoInline)) {
            function.addFnAttr(llvm::Attribute::AlwaysInline);
        }
    }
    for (auto& global : module.globals()) {
        if (global.isDeclaration()) continue;
        global.setLinkage(llvm::GlobalValue::InternalLinkage);
        global.setVisibility(llvm::GlobalValue::DefaultVisibility);
        global.setComdat(nullptr);
    }
    for (auto& alias : module.aliases()) {
        alias.setLinkage(llvm::GlobalValue::InternalLinkage);
        alias.setVisibility(llvm::GlobalValue::DefaultVisibility);
    }
    for (auto& ifunc : module.ifuncs()) {
        ifunc.setLinkage(llvm::GlobalValue::InternalLinkage);
        ifunc.setVisibility(llvm::GlobalValue::DefaultVisibility);
    }
}

std::size_t retarget_region_specialization_functions(
    llvm::Module& module,
    std::string_view target_cpu,
    std::string_view target_features)
{
    std::size_t updated = 0;
    std::string const cpu(target_cpu);
    for (auto& function : module) {
        if (function.isDeclaration()) continue;

        std::string features(target_features);
        auto const existing_features = function.getFnAttribute("target-features");
        // Match iv_package_finalize's host-target operation. The GraphJit
        // target comes first; a function's own explicit requirements remain
        // after it and therefore keep their precedence.
        if (existing_features.isValid()
            && !existing_features.getValueAsString().empty()) {
            if (!features.empty()) features += ',';
            features += existing_features.getValueAsString();
        }

        function.addFnAttr("target-cpu", cpu);
        if (features.empty()) {
            function.removeFnAttr("target-features");
        } else {
            function.addFnAttr("target-features", features);
        }
        function.removeFnAttr("tune-cpu");
        ++updated;
    }
    return updated;
}


namespace {

// The workspace offset that pointer addresses: the workspace argument plus a
// constant offset, or a pointer loaded from a known workspace pointer field
// plus a constant offset.
std::optional<std::size_t> workspace_offset(
    llvm::Value* pointer,
    llvm::Value const* workspace,
    llvm::DataLayout const& layout,
    std::map<std::size_t, RegionSpecializationWorkspaceContents::PointerField const*> const&
        pointers,
    unsigned depth = 0)
{
    if (depth > 16 || !pointer->getType()->isPointerTy()) return std::nullopt;
    llvm::APInt offset(layout.getIndexTypeSizeInBits(pointer->getType()), 0);
    auto* base = pointer->stripAndAccumulateConstantOffsets(layout, offset, true);
    if (offset.isNegative()) return std::nullopt;
    auto const added = static_cast<std::size_t>(offset.getZExtValue());
    if (base == workspace) return added;
    auto* load = llvm::dyn_cast<llvm::LoadInst>(base);
    if (!load || !load->isSimple() || !load->getType()->isPointerTy()) return std::nullopt;
    auto const field = workspace_offset(
        load->getPointerOperand(), workspace, layout, pointers, depth + 1);
    if (!field) return std::nullopt;
    auto const found = pointers.find(*field);
    if (found == pointers.end() || !found->second->workspace) return std::nullopt;
    return found->second->target + added;
}

// The value that a load of type reads from known workspace bytes.
llvm::Constant* known_value(
    llvm::Type* type,
    std::span<std::byte const> bytes)
{
    if (type->isIntegerTy()) {
        auto const bits = type->getIntegerBitWidth();
        if (bits > 64 || bytes.size() * 8 < bits) return nullptr;
        std::uint64_t value = 0;
        std::memcpy(&value, bytes.data(), std::min(bytes.size(), sizeof(value)));
        if (bits < 64) value &= (std::uint64_t{1} << bits) - 1;
        return llvm::ConstantInt::get(type, llvm::APInt(bits, value));
    }
    if (type->isFloatTy() || type->isDoubleTy() || type->isHalfTy() || type->isBFloatTy()) {
        auto const bits = type->getPrimitiveSizeInBits().getFixedValue();
        std::uint64_t value = 0;
        std::memcpy(&value, bytes.data(), std::min(bytes.size(), sizeof(value)));
        return llvm::ConstantFP::get(
            type, llvm::APFloat(type->getFltSemantics(), llvm::APInt(bits, value)));
    }
    if (type->isPointerTy()
        && std::ranges::all_of(bytes, [](std::byte byte) { return byte == std::byte{}; })) {
        return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(type));
    }
    return nullptr;
}

// The least p with bytes[i] == bytes[i + p] for every i, so that
// bytes[i] == bytes[i % p].
std::size_t byte_period(llvm::StringRef bytes)
{
    if (bytes.empty()) return 0;
    std::vector<std::size_t> border(bytes.size(), 0);
    std::size_t matched = 0;
    for (std::size_t index = 1; index < bytes.size(); ++index) {
        while (matched > 0 && bytes[index] != bytes[matched]) matched = border[matched - 1];
        if (bytes[index] == bytes[matched]) ++matched;
        border[index] = matched;
    }
    return bytes.size() - border.back();
}

// The value a load reads from a constant global whose bytes repeat with
// period p, when its address is the global plus a constant offset and
// variable terms that are each a multiple of p. A defined load reads inside
// the global, where all positions of one residue modulo p hold the same
// bytes. An immutable input's ring is such a global: a slot index computed at
// run time reads the one value. Address arithmetic wraps modulo 2^64, which
// preserves the residue when p is a power of two; otherwise every address
// step must be nusw, so that the terms add without wrapping.
llvm::Constant* periodic_constant_value(
    llvm::LoadInst& load,
    llvm::DataLayout const& layout,
    std::map<llvm::GlobalVariable const*, std::size_t>& periods)
{
    auto* type = load.getType();
    auto const size = layout.getTypeStoreSize(type);
    if (size.isScalable() || size.getFixedValue() == 0) return nullptr;
    auto const bits = layout.getIndexTypeSizeInBits(load.getPointerOperand()->getType());
    llvm::APInt constant(bits, 0);
    llvm::SmallVector<std::pair<llvm::Value*, llvm::APInt>, 4> terms;
    auto no_wrap = true;
    auto* pointer = load.getPointerOperand();
    for (unsigned depth = 0; depth < 16; ++depth) {
        auto* step = llvm::dyn_cast<llvm::GEPOperator>(pointer);
        if (!step) break;
        llvm::SmallMapVector<llvm::Value*, llvm::APInt, 4> variables;
        llvm::APInt offset(bits, 0);
        if (!step->collectOffset(layout, bits, variables, offset)) return nullptr;
        constant += offset;
        for (auto const& [value, scale] : variables) terms.emplace_back(value, scale);
        no_wrap = no_wrap && step->hasNoUnsignedSignedWrap();
        pointer = step->getPointerOperand();
    }
    // LLVM folds a load at a constant offset itself.
    if (terms.empty()) return nullptr;
    auto* global = llvm::dyn_cast<llvm::GlobalVariable>(pointer);
    if (!global || !global->isConstant() || !global->hasDefinitiveInitializer()) {
        return nullptr;
    }
    // An all-zero value's ring is a zero initializer, of period one.
    if (auto* uniform = llvm::ConstantFoldLoadFromUniformValue(
            global->getInitializer(), type, layout)) {
        return uniform;
    }
    auto const* data = llvm::dyn_cast<llvm::ConstantDataSequential>(global->getInitializer());
    if (!data || !data->getElementType()->isIntegerTy(8)) return nullptr;
    auto const bytes = data->getRawDataValues();
    auto const extent = static_cast<std::size_t>(size.getFixedValue());
    if (extent > bytes.size()) return nullptr;
    auto [found, inserted] = periods.try_emplace(global, 0);
    if (inserted) found->second = byte_period(bytes);
    auto const period = found->second;
    if (period == 0 || (!std::has_single_bit(period) && !no_wrap)) return nullptr;
    for (auto const& [value, scale] : terms) {
        auto const known = llvm::computeKnownBits(value, layout);
        if (known.isZero()) continue;
        auto const multiple = scale.abs().zext(2 * bits) << known.countMinTrailingZeros();
        if (multiple.urem(period) != 0) return nullptr;
    }
    auto const divisor = llvm::APInt(bits, period);
    auto residue = constant.srem(divisor);
    if (residue.isNegative()) residue += divisor;
    std::vector<std::byte> value(extent);
    for (std::size_t offset = 0; offset < extent; ++offset) {
        value[offset] = static_cast<std::byte>(
            bytes[(static_cast<std::size_t>(residue.getZExtValue()) + offset) % period]);
    }
    return known_value(type, value);
}

// At most this many integers per variable address term, and this many
// addresses per load, are enumerated.
constexpr std::size_t maximum_term_candidates = 8;
constexpr std::size_t maximum_address_candidates = 64;

// Every integer that value can hold: the value of an integer constant, the
// union of what the operands of a phi or select of such values can hold, or
// an integer extension or truncation of such a value. A phi always holds one
// of its incoming values and a select one of its arms, so the set is exact.
// Nothing when an operand is anything else or the set exceeds
// maximum_term_candidates.
std::optional<llvm::SmallVector<llvm::APInt, 8>> integer_candidates(
    llvm::Value* value,
    unsigned depth = 0)
{
    llvm::SmallVector<llvm::APInt, 8> result;
    if (auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        result.push_back(constant->getValue());
        return result;
    }
    if (depth >= 4) return std::nullopt;
    if (auto* cast = llvm::dyn_cast<llvm::CastInst>(value);
        cast && cast->getType()->isIntegerTy()) {
        auto const bits = cast->getType()->getIntegerBitWidth();
        auto const opcode = cast->getOpcode();
        if (opcode != llvm::Instruction::SExt && opcode != llvm::Instruction::ZExt
            && opcode != llvm::Instruction::Trunc) {
            return std::nullopt;
        }
        auto nested = integer_candidates(cast->getOperand(0), depth + 1);
        if (!nested) return std::nullopt;
        for (auto const& candidate : *nested) {
            auto converted = opcode == llvm::Instruction::SExt
                ? candidate.sext(bits)
                : opcode == llvm::Instruction::ZExt ? candidate.zext(bits)
                                                     : candidate.trunc(bits);
            if (std::ranges::find(result, converted) == result.end()) {
                result.push_back(std::move(converted));
            }
        }
        return result;
    }
    llvm::SmallVector<llvm::Value*, 4> operands;
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
        for (auto& incoming : phi->incoming_values()) operands.push_back(incoming.get());
    } else if (auto* select = llvm::dyn_cast<llvm::SelectInst>(value)) {
        operands.push_back(select->getTrueValue());
        operands.push_back(select->getFalseValue());
    } else {
        return std::nullopt;
    }
    for (auto* operand : operands) {
        // A phi that keeps its own value along a back edge adds nothing.
        if (operand == value) continue;
        auto nested = integer_candidates(operand, depth + 1);
        if (!nested) return std::nullopt;
        for (auto const& candidate : *nested) {
            if (std::ranges::find(result, candidate) != result.end()) continue;
            if (result.size() == maximum_term_candidates) return std::nullopt;
            result.push_back(candidate);
        }
    }
    if (result.empty()) return std::nullopt;
    return result;
}

// A load address whose base resolves to a workspace offset, as
// workspace_offset() resolves it, plus a constant and variable GEP index
// terms, each with its scale in bytes.
struct VariableWorkspaceAddress {
    std::size_t base = 0;
    llvm::APInt constant{};
    llvm::SmallVector<std::pair<llvm::Value*, llvm::APInt>, 4> terms{};
};

std::optional<VariableWorkspaceAddress> variable_workspace_address(
    llvm::Value* pointer,
    llvm::Value const* workspace,
    llvm::DataLayout const& layout,
    std::map<std::size_t, RegionSpecializationWorkspaceContents::PointerField const*> const&
        pointers)
{
    auto const bits = layout.getIndexTypeSizeInBits(pointer->getType());
    VariableWorkspaceAddress result{.constant = llvm::APInt(bits, 0)};
    for (unsigned depth = 0; depth < 16; ++depth) {
        auto* step = llvm::dyn_cast<llvm::GEPOperator>(pointer);
        if (!step) break;
        llvm::SmallMapVector<llvm::Value*, llvm::APInt, 4> variables;
        llvm::APInt offset(bits, 0);
        if (!step->collectOffset(layout, bits, variables, offset)) return std::nullopt;
        result.constant += offset;
        for (auto const& [value, scale] : variables) {
            auto const existing = std::ranges::find(
                result.terms, value, &std::pair<llvm::Value*, llvm::APInt>::first);
            if (existing != result.terms.end()) {
                existing->second += scale;
            } else {
                result.terms.emplace_back(value, scale);
            }
        }
        pointer = step->getPointerOperand();
    }
    if (result.terms.empty()) return std::nullopt;
    auto const base = workspace_offset(pointer, workspace, layout, pointers);
    if (!base) return std::nullopt;
    result.base = *base;
    return result;
}

// The value that load reads when its workspace address has variable index
// terms that each hold one of a few known integers, such as a loop over a
// step's bindings that unrolling has not removed. Each address the terms can
// form must lie in the workspace and name a known, non-pointer field; known
// fields do not change during a call. When every address holds one value,
// that value replaces the load. When the one variable term is a phi or
// select of integer constants, a phi or select of the corresponding field
// values does: it holds the field of the address that the original selects.
llvm::Value* variable_known_value(
    llvm::LoadInst& load,
    llvm::Value const* workspace,
    llvm::DataLayout const& layout,
    std::map<std::size_t, RegionSpecializationWorkspaceContents::PointerField const*> const&
        pointers,
    RegionSpecializationWorkspaceContents const& contents)
{
    auto* type = load.getType();
    if (type->isPointerTy()) return nullptr;
    auto const size = layout.getTypeStoreSize(type);
    if (size.isScalable() || size.getFixedValue() == 0) return nullptr;
    auto const extent = static_cast<std::size_t>(size.getFixedValue());
    auto const address = variable_workspace_address(
        load.getPointerOperand(), workspace, layout, pointers);
    if (!address) return nullptr;
    auto const bits = address->constant.getBitWidth();

    llvm::SmallVector<llvm::SmallVector<llvm::APInt, 8>, 4> candidates;
    std::size_t combinations = 1;
    for (auto const& [value, scale] : address->terms) {
        (void)scale;
        auto values = integer_candidates(value);
        if (!values) return nullptr;
        // GEP indices are sign-extended to the index width.
        for (auto& candidate : *values) candidate = candidate.sextOrTrunc(bits);
        combinations *= values->size();
        if (combinations > maximum_address_candidates) return nullptr;
        candidates.push_back(std::move(*values));
    }

    // The field value at the address that one choice of term values forms.
    auto value_at = [&](std::span<llvm::APInt const> choice) -> llvm::Constant* {
        auto offset = llvm::APInt(bits, address->base) + address->constant;
        for (std::size_t term = 0; term < choice.size(); ++term) {
            offset += address->terms[term].second * choice[term];
        }
        if (offset.getActiveBits() > 64) return nullptr;
        auto const begin = offset.getZExtValue();
        if (begin > contents.bytes.size() || extent > contents.bytes.size() - begin) {
            return nullptr;
        }
        // Pointer fields are never marked known, so this also excludes them.
        auto const first = static_cast<std::size_t>(begin);
        auto const known = std::span<std::uint8_t const>{contents.known}.subspan(first, extent);
        if (!std::ranges::all_of(known, [](std::uint8_t byte) { return byte != 0; })) {
            return nullptr;
        }
        return known_value(
            type, std::span<std::byte const>{contents.bytes}.subspan(first, extent));
    };

    llvm::Constant* uniform = nullptr;
    auto agree = true;
    llvm::SmallVector<llvm::APInt, 4> choice(address->terms.size(), llvm::APInt(bits, 0));
    for (std::size_t combination = 0; combination < combinations; ++combination) {
        auto remaining = combination;
        for (std::size_t term = 0; term < candidates.size(); ++term) {
            choice[term] = candidates[term][remaining % candidates[term].size()];
            remaining /= candidates[term].size();
        }
        auto* value = value_at(choice);
        if (!value) return nullptr;
        if (!uniform) {
            uniform = value;
        } else if (value != uniform) {
            agree = false;
        }
    }
    if (agree) return uniform;

    if (address->terms.size() != 1) return nullptr;
    auto* variable = address->terms.front().first;
    auto field_for = [&](llvm::Value* index) -> llvm::Constant* {
        auto* constant = llvm::dyn_cast<llvm::ConstantInt>(index);
        if (!constant) return nullptr;
        llvm::APInt const chosen = constant->getValue().sextOrTrunc(bits);
        return value_at(std::span<llvm::APInt const>{&chosen, 1});
    };
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(variable)) {
        llvm::SmallVector<llvm::Constant*, 8> values;
        for (auto& incoming : phi->incoming_values()) {
            auto* value = field_for(incoming.get());
            if (!value) return nullptr;
            values.push_back(value);
        }
        // The new phi sits in the variable's block, beside it, so it holds
        // the corresponding field wherever the variable is defined.
        auto* known = llvm::PHINode::Create(
            type, phi->getNumIncomingValues(), "iv.region.known", phi->getIterator());
        for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
            known->addIncoming(values[index], phi->getIncomingBlock(index));
        }
        return known;
    }
    if (auto* select = llvm::dyn_cast<llvm::SelectInst>(variable)) {
        auto* when_true = field_for(select->getTrueValue());
        auto* when_false = field_for(select->getFalseValue());
        if (!when_true || !when_false) return nullptr;
        return llvm::SelectInst::Create(
            select->getCondition(), when_true, when_false, "iv.region.known",
            load.getIterator());
    }
    return nullptr;
}

} // namespace

std::size_t substitute_region_workspace_values(
    llvm::Module& module,
    std::string const& function_name,
    RegionSpecializationWorkspaceContents const& contents)
{
    auto* function = module.getFunction(function_name);
    if (!function || function->isDeclaration()
        || function->arg_size() <= region_specialization_workspace_argument) {
        return 0;
    }
    auto* workspace = function->getArg(
        region_specialization_workspace_argument);
    auto const& layout = module.getDataLayout();
    auto& context = module.getContext();
    std::map<std::size_t, RegionSpecializationWorkspaceContents::PointerField const*> pointers;
    for (auto const& field : contents.pointers) pointers.emplace(field.offset, &field);
    // One constant per immutable value that a pointer field addresses.
    std::map<std::size_t, llvm::GlobalVariable*> constants;
    auto constant_for = [&](RegionSpecializationWorkspaceContents::PointerField const& field) {
        auto [found, inserted] = constants.try_emplace(field.offset, nullptr);
        if (inserted) {
            auto* array = llvm::ConstantDataArray::get(
                context,
                llvm::ArrayRef<std::uint8_t>(
                    reinterpret_cast<std::uint8_t const*>(field.constant.data()),
                    field.constant.size()));
            found->second = new llvm::GlobalVariable(
                module,
                array->getType(),
                true,
                llvm::GlobalValue::PrivateLinkage,
                array,
                "iv.region.immutable.value");
            found->second->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            found->second->setAlignment(llvm::Align(alignof(std::max_align_t)));
        }
        return found->second;
    };

    // Per constant global that a load reads, its byte period.
    std::map<llvm::GlobalVariable const*, std::size_t> periods;

    std::size_t replaced = 0;
    // A replaced pointer load can make the loads based on it resolvable.
    for (unsigned sweep = 0; sweep < 8; ++sweep) {
        std::vector<std::pair<llvm::LoadInst*, llvm::Value*>> replacements;
        for (auto& block : *function) {
            for (auto& instruction : block) {
                auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
                if (!load || !load->isSimple()) continue;
                auto const offset = workspace_offset(
                    load->getPointerOperand(), workspace, layout, pointers);
                if (!offset) {
                    if (auto* value = periodic_constant_value(*load, layout, periods)) {
                        replacements.emplace_back(load, value);
                    } else if (auto* known = variable_known_value(
                                   *load, workspace, layout, pointers, contents)) {
                        replacements.emplace_back(load, known);
                    }
                    continue;
                }
                auto* type = load->getType();
                auto const size = layout.getTypeStoreSize(type);
                if (size.isScalable() || *offset > contents.bytes.size()
                    || size.getFixedValue() > contents.bytes.size() - *offset) {
                    continue;
                }
                auto const extent = static_cast<std::size_t>(size.getFixedValue());
                llvm::Value* value = nullptr;
                if (auto const field = pointers.find(*offset);
                    field != pointers.end() && type->isPointerTy()
                    && extent == sizeof(void*)) {
                    if (field->second->workspace) {
                        llvm::IRBuilder<> builder(load);
                        value = builder.CreateConstInBoundsGEP1_64(
                            llvm::Type::getInt8Ty(context), workspace, field->second->target);
                    } else {
                        value = constant_for(*field->second);
                    }
                } else if (std::all_of(
                               contents.known.begin() + static_cast<std::ptrdiff_t>(*offset),
                               contents.known.begin()
                                   + static_cast<std::ptrdiff_t>(*offset + extent),
                               [](std::uint8_t known) { return known != 0; })) {
                    value = known_value(
                        type, std::span<std::byte const>{contents.bytes}.subspan(*offset, extent));
                }
                if (value) replacements.emplace_back(load, value);
            }
        }
        if (replacements.empty()) break;
        for (auto const& [load, value] : replacements) {
            load->replaceAllUsesWith(value);
            load->eraseFromParent();
            ++replaced;
        }
    }
    return replaced;
}

} // namespace iv::graph_jit
