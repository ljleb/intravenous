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
struct IvModuleRequiredDefinitionsChanged;
using IvModuleRequiredDefinitionsChangedEvent =
    void (*)(IvModuleRequiredDefinitionsChanged const &);
struct IvModuleInstancesChanged;
using IvModuleInstancesChangedEvent =
    void (*)(IvModuleInstancesChanged const &);
using IvModuleInstancesListChangedEvent =
    void (*)(std::vector<IvModuleInstanceInfo> const &);

struct IvModuleSampleInputResolutionRequest {
    std::string logical_node_id{};
    size_t member_ordinal = 0;
    size_t input_ordinal = 0;
    std::string input_name{};
    Sample default_value = 0.0f;
};

class IvModuleSampleInputResolutionBuilder {
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

using IvModuleSampleInputResolutionRequestedEvent =
    void (*)(
        IvModuleSampleInputResolutionRequest const &,
        IvModuleSampleInputResolutionBuilder &);

IV_DECLARE_LINKER_EVENT(
    IvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleSampleInputResolutionRequestedEvent,
    iv_runtime_iv_module_sample_input_resolution_requested_event);
} // namespace iv
