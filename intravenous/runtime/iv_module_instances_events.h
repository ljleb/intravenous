#pragma once

#include "runtime/iv_module_instance_types.h"
#include "linker_event.h"
#include "runtime/lane_ref.h"
#include "graph/types.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {
struct RuntimeIvModuleRequiredDefinitionsChanged;
using RuntimeIvModuleRequiredDefinitionsChangedEvent =
    void (*)(RuntimeIvModuleRequiredDefinitionsChanged const &);
struct RuntimeIvModuleInstancesChanged;
using RuntimeIvModuleInstancesChangedEvent =
    void (*)(RuntimeIvModuleInstancesChanged const &);
using RuntimeIvModuleInstancesListChangedEvent =
    void (*)(std::vector<RuntimeIvModuleInstanceInfo> const &);

struct RuntimeIvModuleSampleInputResolutionRequest {
    std::string logical_node_id{};
    size_t member_ordinal = 0;
    size_t input_ordinal = 0;
    std::string input_name{};
    Sample default_value = 0.0f;
};

class RuntimeIvModuleSampleInputResolutionBuilder {
    bool resolved_ = false;
    std::optional<RealtimeLaneRef> lane_ref_{};

public:
    void succeed(RealtimeLaneRef lane_ref)
    {
        resolved_ = true;
        lane_ref_ = std::move(lane_ref);
    }

    RealtimeLaneRef build() const
    {
        if (!resolved_ || !lane_ref_.has_value()) {
            throw std::runtime_error(
                "iv-module sample input resolution request was not satisfied");
        }
        return *lane_ref_;
    }
};

using RuntimeIvModuleSampleInputResolutionRequestedEvent =
    void (*)(
        RuntimeIvModuleSampleInputResolutionRequest const &,
        RuntimeIvModuleSampleInputResolutionBuilder &);

IV_DECLARE_LINKER_EVENT(
    RuntimeIvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeIvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeIvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event);
IV_DECLARE_LINKER_EVENT(
    RuntimeIvModuleSampleInputResolutionRequestedEvent,
    iv_runtime_iv_module_sample_input_resolution_requested_event);
} // namespace iv
