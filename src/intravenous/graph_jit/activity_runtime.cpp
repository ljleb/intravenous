#include <intravenous/graph_jit/activity_runtime.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace iv::graph_jit::detail {
namespace {

std::byte const* continuous_value_at(
    ContinuousPortStorageBinding const& storage,
    std::size_t channel,
    std::uint64_t ordinal) noexcept
{
    auto const channels = channel_count(storage.channel_layout);
    if (channel >= channels || channel >= storage.channels.size()) return nullptr;
    auto const& selected = storage.channels[channel];
    if (selected.storage == nullptr || selected.value_capacity == 0
        || !std::has_single_bit(selected.value_capacity)
        || selected.value_stride_bytes < storage.geometry.value_size_bytes) {
        return nullptr;
    }
    auto const slot = static_cast<std::size_t>(ordinal - selected.value_delay)
        & (selected.value_capacity - 1);
    if (slot != 0
        && selected.value_stride_bytes
            > std::numeric_limits<std::size_t>::max() / slot) {
        return nullptr;
    }
    return selected.storage + slot * selected.value_stride_bytes;
}

bool valid_continuous_storage(
    ContinuousPortStorageBinding const& storage) noexcept
{
    auto const& geometry = storage.geometry;
    if (!is_registered_continuous_value_type(geometry.value_type)
        || geometry.elements_per_value == 0
        || geometry.element_size == 0
        || geometry.value_size_bytes == 0
        || geometry.elements_per_value
            > std::numeric_limits<std::size_t>::max() / geometry.element_size
        || geometry.value_size_bytes
            != geometry.elements_per_value * geometry.element_size) {
        return false;
    }
    auto const& type = continuous_value_type_registry[
        static_cast<std::size_t>(geometry.value_type)];
    return type.storage_element_size == geometry.element_size
        && type.storage_element_alignment == geometry.element_alignment
        && type.values_equal != nullptr
        && type.value_is_clear != nullptr;
}

std::size_t load_size(std::byte const* storage, std::size_t offset) noexcept
{
    std::size_t value = 0;
    std::memcpy(&value, storage + offset, sizeof(value));
    return value;
}

template<class Binding>
bool event_binding_is_empty(
    Binding const* binding,
    GlobalIndex block_index,
    std::size_t block_size) noexcept
{
    if (binding == nullptr || block_size == 0) return true;
    auto const& storage = binding->storage;
    if (storage.storage == nullptr || storage.event_capacity == 0) return true;

    auto const* base = storage.storage;
    auto const* events = reinterpret_cast<TimedEvent const*>(
        base + storage.events_offset);
    std::size_t read = 0;
    std::size_t write = 0;
    if (storage.persistent_ring) {
        if (!std::has_single_bit(storage.event_capacity)) return false;
        read = load_size(base, storage.read_index_offset);
        write = load_size(base, storage.write_index_offset);
        if (write < read) return false;
    } else {
        write = std::min(
            load_size(base, storage.count_offset), storage.event_capacity);
    }

    auto const available = std::min(write - read, storage.event_capacity);
    auto const mask = storage.event_capacity - 1;
    for (std::size_t offset = 0; offset < available; ++offset) {
        auto const index = storage.persistent_ring
            ? (read + offset) & mask
            : offset;
        if (block_index.forward_distance_to(events[index].time) < block_size) {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" std::uint32_t iv_graph_jit_continuous_input_activity(
    ReflectedContinuousInputPortBinding const* binding,
    std::size_t channel,
    std::size_t value_count,
    std::uint8_t* remembered_valid,
    void* remembered_value) noexcept
{
    if (value_count == 0) {
        return continuous_input_constant | continuous_input_unchanged;
    }
    if (binding == nullptr || remembered_valid == nullptr
        || remembered_value == nullptr
        || !valid_continuous_storage(binding->storage)) {
        if (remembered_valid != nullptr) *remembered_valid = 0;
        return 0;
    }

    auto const& geometry = binding->storage.geometry;
    auto const& type = continuous_value_type_registry[
        static_cast<std::size_t>(geometry.value_type)];
    auto const begin = binding->sequential_begin_ordinal
        - binding->read_latency;
    auto const* first = continuous_value_at(binding->storage, channel, begin);
    if (first == nullptr) {
        *remembered_valid = 0;
        return 0;
    }

    auto const matches_previous = *remembered_valid != 0
        && type.values_equal(
            first, remembered_value, geometry.elements_per_value);
    for (std::size_t offset = 1; offset < value_count; ++offset) {
        auto const* value = continuous_value_at(
            binding->storage, channel, begin + offset);
        if (value == nullptr
            || !type.values_equal(
                first, value, geometry.elements_per_value)) {
            *remembered_valid = 0;
            return 0;
        }
    }

    std::memcpy(remembered_value, first, geometry.value_size_bytes);
    *remembered_valid = 1;
    return continuous_input_constant
        | (matches_previous ? continuous_input_unchanged : 0u);
}

extern "C" bool iv_graph_jit_continuous_output_is_clear(
    ReflectedContinuousOutputPortBinding const* binding,
    std::size_t channel,
    std::size_t value_count) noexcept
{
    // A slow lattice can have no value in this project-rate slice. That is no
    // evidence that the node's next authored value will be clear, so do not
    // advance its TTL from an empty continuous-output window.
    if (value_count == 0) return false;
    if (binding == nullptr || !valid_continuous_storage(binding->storage)) {
        return false;
    }
    auto const& geometry = binding->storage.geometry;
    auto const& type = continuous_value_type_registry[
        static_cast<std::size_t>(geometry.value_type)];
    auto const begin = binding->sequential_begin_ordinal
        + binding->storage.storage_latency;
    for (std::size_t offset = 0; offset < value_count; ++offset) {
        auto const* value = continuous_value_at(
            binding->storage, channel, begin + offset);
        if (value == nullptr
            || !type.value_is_clear(value, geometry.elements_per_value)) {
            return false;
        }
    }
    return true;
}

extern "C" bool iv_graph_jit_event_input_is_empty(
    ReflectedEventInputPortBinding const* binding,
    GlobalIndex block_index,
    std::size_t block_size) noexcept
{
    return event_binding_is_empty(binding, block_index, block_size);
}

extern "C" bool iv_graph_jit_event_output_is_empty(
    ReflectedEventOutputPortBinding const* binding,
    GlobalIndex block_index,
    std::size_t block_size) noexcept
{
    return event_binding_is_empty(binding, block_index, block_size);
}

} // namespace iv::graph_jit::detail
