#pragma once

#include <intravenous/graph/generated_node_spec.h>
#include <intravenous/graph/runtime_binding_nodes.h>
#include <intravenous/graph/static_storage.hpp>

#include <stdexcept>
#include <vector>

namespace iv::details {
consteval StaticRuntimeInputConfig freeze_runtime_input_config(
    InputConfig const& config)
{
    return {
        .name = define_static_string(config.name),
        .channel_layout = config.channel_layout,
        .history = config.history,
        .default_value = config.default_value,
        .min = config.min,
        .max = config.max,
    };
}

consteval StaticRuntimeOutputConfig freeze_runtime_output_config(
    OutputConfig const& config)
{
    return {
        .name = define_static_string(config.name),
        .channel_layout = config.channel_layout,
        .latency = config.latency,
        .history = config.history,
    };
}

consteval RuntimeSampleInputNode freeze_generated_node(
    RuntimeSampleInputNodeSpec const& spec)
{
    return {
        .output = freeze_runtime_output_config(spec.output),
        .default_value = spec.default_value,
        .binding_id = define_static_string(spec.binding_id),
    };
}

consteval RuntimeEventInputNode freeze_generated_node(
    RuntimeEventInputNodeSpec const& spec)
{
    return {
        .type = spec.type,
        .binding_id = define_static_string(spec.binding_id),
    };
}

consteval RuntimeSampleOutputNode freeze_generated_node(
    RuntimeSampleOutputNodeSpec const& spec)
{
    return {
        .input = freeze_runtime_input_config(spec.input),
        .binding_id = define_static_string(spec.binding_id),
    };
}

consteval RuntimeEventOutputNode freeze_generated_node(
    RuntimeEventOutputNodeSpec const& spec)
{
    return {
        .type = spec.type,
        .binding_id = define_static_string(spec.binding_id),
    };
}

consteval RuntimeSampleOutputFamilyNode freeze_generated_node(
    RuntimeSampleOutputFamilyNodeSpec const& spec)
{
    if (spec.input_configs.empty()
        || spec.input_configs.size() != spec.member_binding_ids.size()) {
        throw std::logic_error(
            "runtime sample output family requires one binding per member");
    }
    auto const layout = spec.input_configs.front().channel_layout;
    std::vector<StaticRuntimeInputConfig> inputs;
    inputs.reserve(spec.input_configs.size());
    for (auto const& input : spec.input_configs) {
        if (input.channel_layout != layout) {
            throw std::logic_error(
                "runtime sample output family member layouts differ");
        }
        inputs.push_back(freeze_runtime_input_config(input));
    }
    std::vector<StaticString> member_binding_ids;
    member_binding_ids.reserve(spec.member_binding_ids.size());
    for (auto const& id : spec.member_binding_ids)
        member_binding_ids.push_back(define_static_string(id));
    return {
        .input_configs = define_static_span(inputs),
        .member_binding_ids = define_static_span(member_binding_ids),
        .aggregate_binding_id =
            define_static_string(spec.aggregate_binding_id),
        .layout = layout,
    };
}

consteval RuntimeEventOutputFamilyNode freeze_generated_node(
    RuntimeEventOutputFamilyNodeSpec const& spec)
{
    if (spec.member_count == 0
        || spec.member_count != spec.member_binding_ids.size()) {
        throw std::logic_error(
            "runtime event output family requires one binding per member");
    }
    std::vector<StaticString> member_binding_ids;
    member_binding_ids.reserve(spec.member_binding_ids.size());
    for (auto const& id : spec.member_binding_ids)
        member_binding_ids.push_back(define_static_string(id));
    return {
        .type = spec.type,
        .member_count = spec.member_count,
        .member_binding_ids = define_static_span(member_binding_ids),
        .aggregate_binding_id =
            define_static_string(spec.aggregate_binding_id),
    };
}
}
