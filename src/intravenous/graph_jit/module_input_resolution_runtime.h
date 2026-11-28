#pragma once

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/node/context.h>
#include <intravenous/node/layout.h>

#include <cstring>

namespace iv::graph_jit::detail {

// Intrinsics use the same type-owned initialization/combination functions as
// authored ports. Public boundary metadata supplies scalar declarative values;
// owning objects and package function pointers are never copied into this stage.
template<class Extension>
auto module_input_options(ContinuousInputConfig const* config)
{
    typename Extension::SequentialInputOptions options{};
    if (config) {
        for (auto const& item : sequential_input_properties(*config).value_metadata) {
            auto const* metadata = std::get_if<typename Extension::SequentialInputMetadata>(&item);
            if (!metadata) continue;
            if constexpr (requires { options.default_value = metadata->default_value; })
                options.default_value = metadata->default_value;
            if constexpr (requires { options.neutral_value = metadata->neutral_value; })
                options.neutral_value = metadata->neutral_value;
            if constexpr (requires { options.enabled_by_default = metadata->enabled_by_default; })
                options.enabled_by_default = metadata->enabled_by_default;
        }
    }
    return options;
}

inline bool initialize_module_input_value(
    ContinuousInputConfig const* config,
    ContinuousValueTypeId type,
    iv::details::SequentialInputInitialization initialization,
    std::byte* destination,
    std::size_t bytes,
    std::size_t elements)
{
    switch (type) {
#define IV_INITIALIZE_MODULE_INPUT(key, Extension) \
    case ContinuousValueTypeId::key: { \
        using Element = typename Extension::storage_element; \
        if (bytes % sizeof(Element)) return false; \
        auto output = std::span{reinterpret_cast<Element*>(destination), bytes / sizeof(Element)}; \
        auto options = module_input_options<Extension>(config); \
        return initialization == iv::details::SequentialInputInitialization::neutral \
            ? Extension::initialize_sequential_neutral(options, output, elements) \
            : Extension::initialize_sequential_disconnected(options, output, elements); \
    }
        IV_CONTINUOUS_VALUE_TYPES(IV_INITIALIZE_MODULE_INPUT)
#undef IV_INITIALIZE_MODULE_INPUT
    case ContinuousValueTypeId::count:
        return false;
    }
    return false;
}

inline bool combine_module_input_values(
    std::size_t port, ContinuousValueTypeId type,
    std::byte* destination, std::byte const* source,
    std::size_t bytes, std::size_t elements)
{
    if (port != 0) return false;
    switch (type) {
#define IV_COMBINE_MODULE_INPUT(key, Extension) \
    case ContinuousValueTypeId::key: { \
        using Element = typename Extension::storage_element; \
        if (bytes % sizeof(Element)) return false; \
        return Extension::combine_sequential_values( \
            typename Extension::SequentialInputOptions{}, \
            std::span{reinterpret_cast<Element*>(destination), bytes / sizeof(Element)}, \
            std::span{reinterpret_cast<Element const*>(source), bytes / sizeof(Element)}, elements); \
    }
        IV_CONTINUOUS_VALUE_TYPES(IV_COMBINE_MODULE_INPUT)
#undef IV_COMBINE_MODULE_INPUT
    case ContinuousValueTypeId::count:
        return false;
    }
    return false;
}

inline std::size_t declare_module_input_resolution(
    void const*, NodeStateStructures const*, NodeLayoutBuilder& builder,
    ReflectedNodePortRealization const&)
{
    return iv::details::register_node(builder, {
        .node_type_name = "Sequential module input",
    });
}

inline std::size_t module_input_value_count(
    GlobalIndex index, std::size_t count, std::int32_t pace) noexcept
{
    if (pace == 0) return count;
    auto const projected = FractionalGlobalIndexRange::from_global_range({index, count})
        .try_project_onto_lattice(pace);
    IV_ASSERT(projected.has_value(), "module-input resolution has an invalid value lattice");
    return projected->count;
}

inline void tick_module_input_resolution(
    void const*, ReflectedNodeTickContext const& context,
    GlobalIndex index, std::size_t count)
{
    if (!context.continuous_input_bindings.empty()) {
        auto const& input = context.continuous_input_bindings.data()[0];
        auto const& output = context.continuous_output_bindings.data()[0];
        auto const values = module_input_value_count(index, count, output.absolute_pace_exponent);
        for (std::size_t channel = 0; channel < channel_count(output.storage.channel_layout); ++channel) {
            auto const& source = input.storage.channels[channel];
            auto const& target = output.storage.channels[channel];
            for (std::size_t value = 0; value < values; ++value) {
                auto const from = (input.sequential_begin_index + value
                    - input.read_latency - source.value_delay) & (source.value_capacity - 1);
                auto const to = (output.sequential_begin_index + value
                    - target.value_delay) & (target.value_capacity - 1);
                std::memcpy(target.storage + to * target.value_stride_bytes,
                    source.storage + from * source.value_stride_bytes,
                    output.storage.geometry.value_size_bytes);
            }
        }
        return;
    }
    auto const& input = context.event_input_bindings.data()[0].storage;
    auto const& output = context.event_output_bindings.data()[0];
    auto read = [](std::byte* storage, std::size_t offset) {
        std::size_t value;
        std::memcpy(&value, storage + offset, sizeof(value));
        return value;
    };
    auto const first = input.persistent_ring ? read(input.storage, input.read_index_offset) : 0;
    auto const last = read(input.storage,
        input.persistent_ring ? input.write_index_offset : input.count_offset);
    EventSharedPortData source{
        {reinterpret_cast<TimedEvent*>(input.storage + input.events_offset), input.event_capacity},
        first, last, input.type};
    auto const& storage = output.storage;
    auto const target_first = storage.persistent_ring ? read(storage.storage, storage.read_index_offset) : 0;
    auto const write_offset = storage.persistent_ring ? storage.write_index_offset : storage.count_offset;
    auto const target_last = output.append_existing ? read(storage.storage, write_offset) : 0;
    EventSharedPortData target{
        {reinterpret_cast<TimedEvent*>(storage.storage + storage.events_offset), storage.event_capacity},
        target_first, target_last, storage.type};
    EventOutputPort sink{target, output.source_type, output.history, output.latency, output.overflow_count};
    sink.begin_block(index, count);
    sink.push_block(EventInputPort{source}.get_block(index, count));
    std::memcpy(storage.storage + write_offset, &target.write_index, sizeof(target.write_index));
}

inline void batch_module_input_resolution(ReflectedNodeTickBatchContext const& batch)
{
    for (auto const& lane : std::span<ReflectedNodeTickBatchLane const>(batch.lanes))
        tick_module_input_resolution(lane.node_data, lane.context, lane.index, lane.block_size);
}

inline iv::details::NodeCompilerOperations module_input_resolution_operations()
{
    return {
        .declare_node = &declare_module_input_resolution,
        .tick_block = &tick_module_input_resolution,
        .tick_block_batch = &batch_module_input_resolution,
        .skip_block = &tick_module_input_resolution,
        .skip_block_batch = &batch_module_input_resolution,
        .combine_sequential_input_values = &combine_module_input_values,
    };
}

} // namespace iv::graph_jit::detail
