#include <intravenous/graph_jit/region_specialization.h>

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace iv::graph_jit {
namespace {

class KeyWriter {
    std::string key_{};

public:
    template<class T>
        requires std::is_trivially_copyable_v<T> && (!std::is_pointer_v<T>)
    void value(T const& field)
    {
        key_.append(reinterpret_cast<char const*>(&field), sizeof(field));
    }

    void bytes(std::span<std::byte const> data)
    {
        value(data.size());
        key_.append(reinterpret_cast<char const*>(data.data()), data.size());
    }

    void text(std::string_view data)
    {
        value(data.size());
        key_.append(data);
    }

    [[nodiscard]] std::string take() && { return std::move(key_); }
};

template<class T>
T read_record(std::span<std::byte const> bytes, std::size_t index)
{
    T record{};
    std::memcpy(&record, bytes.data() + index * sizeof(T), sizeof(T));
    return record;
}

void write_geometry(KeyWriter& key, ContinuousValueStorageGeometry const& geometry)
{
    key.value(geometry.value_type);
    key.value(geometry.elements_per_value);
    key.value(geometry.element_size);
    key.value(geometry.element_alignment);
    key.value(geometry.value_size_bytes);
    key.value(geometry.channel_scalar_lane_count);
    key.value(geometry.channel_capability);
}

void write_storage(
    KeyWriter& key,
    ContinuousPortStorageBinding const& storage,
    std::size_t channel_count)
{
    key.value(channel_count);
    for (std::size_t channel = 0; channel < channel_count; ++channel) {
        auto const& binding = storage.channels[channel];
        key.value(binding.value_capacity);
        key.value(binding.value_stride_bytes);
        key.value(binding.value_delay);
    }
    write_geometry(key, storage.geometry);
    key.value(storage.value_capacity);
    key.value(storage.channel_layout.channel_type);
    key.value(storage.channel_layout.sample_layout);
}

} // namespace

bool region_specialization_request_is_complete(
    RegionSpecializationRequest const& request) noexcept
{
    if (request.quantum == 0 || request.steps.empty()) return false;
    for (auto const& step : request.steps) {
        if (!step.revision
            || step.inputs.size() != step.input_channel_counts.size()
                * sizeof(ReflectedContinuousInputPortBinding)
            || step.outputs.size() != step.output_channel_counts.size()
                * sizeof(ReflectedContinuousOutputPortBinding)
            || step.context.size() != sizeof(ReflectedNodeTickContext)) {
            return false;
        }
        for (auto const count : step.input_channel_counts) {
            if (count == 0 || count > maximum_supported_channel_count) return false;
        }
        for (auto const count : step.output_channel_counts) {
            if (count == 0 || count > maximum_supported_channel_count) return false;
        }
        auto const context = read_record<ReflectedNodeTickContext>(step.context, 0);
        if (context.continuous_input_bindings.extent
                != step.input_channel_counts.size()
            || context.continuous_output_bindings.extent
                != step.output_channel_counts.size()
            || context.event_input_bindings.extent != 0
            || context.event_output_bindings.extent != 0
            || context.random_access_inputs.extent != 0
            || context.random_access_event_inputs.extent != 0) {
            return false;
        }
        std::vector<bool> immutable_inputs(step.input_channel_counts.size());
        for (auto const& input : step.immutable_inputs) {
            if (input.binding >= step.input_channel_counts.size()
                || input.value.empty() || immutable_inputs[input.binding]) {
                return false;
            }
            immutable_inputs[input.binding] = true;
            auto const binding = read_record<ReflectedContinuousInputPortBinding>(
                std::span<std::byte const>{step.inputs}, input.binding);
            auto const value_size = input.value.size();
            if (value_size != binding.storage.geometry.value_size_bytes) {
                return false;
            }
            // Each channel's ring is the value repeated at its stride.
            for (std::size_t channel = 0;
                 channel < step.input_channel_counts[input.binding]; ++channel) {
                auto const& ring = binding.storage.channels[channel];
                if (ring.value_capacity == 0
                    || !std::has_single_bit(ring.value_capacity)
                    || ring.value_stride_bytes < value_size
                    || ring.value_stride_bytes % value_size != 0
                    || ring.value_capacity
                        > std::numeric_limits<std::size_t>::max()
                            / ring.value_stride_bytes) {
                    return false;
                }
            }
        }
        std::vector<bool> input_begin_indices(step.input_channel_counts.size());
        std::vector<bool> output_begin_indices(step.output_channel_counts.size());
        for (auto const& update : step.begin_indices) {
            if (update.binding >= (update.output
                    ? step.output_channel_counts.size()
                    : step.input_channel_counts.size())) {
                return false;
            }
            auto& seen = update.output
                ? output_begin_indices
                : input_begin_indices;
            if (seen[update.binding]) return false;
            seen[update.binding] = true;
        }
    }
    return true;
}

namespace {

std::string make_region_specialization_key(
    RegionSpecializationRequest const& request,
    bool include_immutable_inputs)
{
    KeyWriter key;
    key.value(request.quantum);
    key.value(request.steps.size());
    for (auto const& step : request.steps) {
        // The finalized bitcode's file name carries its artifact signature, so
        // a package rebuild names different node code.
        key.text(step.revision
            ? step.revision->compiler_artifact.bitcode_path.string()
            : std::string{});
        key.value(step.revision->node_code_retained_globals.size());
        for (auto const& global : step.revision->node_code_retained_globals) {
            key.value(reinterpret_cast<std::uintptr_t>(global.address));
            key.value(global.size);
            key.value(global.alignment);
        }
        key.value(step.code_key.low);
        key.value(step.code_key.high);
        key.value(step.provider_version);

        auto const inputs = std::span<std::byte const>{step.inputs};
        key.value(step.input_channel_counts.size());
        for (std::size_t input = 0; input < step.input_channel_counts.size(); ++input) {
            auto const binding =
                read_record<ReflectedContinuousInputPortBinding>(inputs, input);
            write_storage(key, binding.storage, step.input_channel_counts[input]);
            key.value(binding.history);
            key.value(binding.read_latency);
            key.value(binding.absolute_pace_exponent);
            key.value(binding.contiguous_value_capacity);
        }
        auto const outputs = std::span<std::byte const>{step.outputs};
        key.value(step.output_channel_counts.size());
        for (std::size_t output = 0; output < step.output_channel_counts.size(); ++output) {
            auto const binding =
                read_record<ReflectedContinuousOutputPortBinding>(outputs, output);
            write_storage(key, binding.storage, step.output_channel_counts[output]);
            key.value(binding.history);
            key.value(binding.latency);
            key.value(binding.absolute_pace_exponent);
        }
        auto const context = read_record<ReflectedNodeTickContext>(step.context, 0);
        key.value(context.continuous_input_bindings.extent);
        key.value(context.continuous_output_bindings.extent);
        key.value(context.event_input_bindings.extent);
        key.value(context.event_output_bindings.extent);
        key.value(context.random_access_inputs.extent);
        key.value(context.random_access_event_inputs.extent);
        key.value(context.sample_rate);
        key.value(context.scc_feedback_latency);
        key.value(context.state.extent);

        if (include_immutable_inputs) {
            key.value(step.immutable_inputs.size());
            for (auto const& input : step.immutable_inputs) {
                key.value(input.binding);
                key.bytes(input.value);
            }
        } else {
            key.value(std::size_t{0});
        }
        key.value(step.begin_indices.size());
        for (auto const& update : step.begin_indices) {
            key.value(update.output);
            key.value(update.binding);
        }
        key.value(step.operations_before);
        key.value(step.operations_after);
        key.value(step.stages_manual_controls);
    }
    return std::move(key).take();
}

} // namespace

std::string region_specialization_key(RegionSpecializationRequest const& request)
{
    if (!region_specialization_request_is_complete(request)) return {};
    return make_region_specialization_key(request, true);
}

std::string region_specialization_structure_key(
    RegionSpecializationRequest const& request)
{
    if (!region_specialization_request_is_complete(request)) return {};
    return make_region_specialization_key(request, false);
}

std::optional<std::size_t> region_specialization_workspace_size(
    RegionSpecializationRequest const& request) noexcept
{
    std::size_t size = 0;
    auto take = [&](std::size_t bytes) {
        auto const alignment = alignof(std::max_align_t);
        auto const remainder = size % alignment;
        auto const padding = remainder == 0 ? 0 : alignment - remainder;
        bytes = std::max<std::size_t>(bytes, 1);
        if (padding > std::numeric_limits<std::size_t>::max() - size
            || bytes > std::numeric_limits<std::size_t>::max() - size - padding) {
            return false;
        }
        size += padding + bytes;
        return true;
    };
    for (auto const& step : request.steps) {
        if (!take(step.inputs.size()) || !take(step.outputs.size())
            || !take(step.context.size())) {
            return std::nullopt;
        }
    }
    return size;
}

} // namespace iv::graph_jit
