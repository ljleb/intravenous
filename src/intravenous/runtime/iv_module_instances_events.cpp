#include <intravenous/runtime/iv_module_instances_events.h>

#include <utility>

namespace iv {
void IvModuleInstanceBuildersAckBuilder::set_prerequisite_lanes(
    std::string instance_id,
    std::vector<LaneId> prerequisite_lanes)
{
    prerequisite_lanes_by_instance_id_[std::move(instance_id)] =
        std::move(prerequisite_lanes);
}

std::optional<std::vector<LaneId>>
IvModuleInstanceBuildersAckBuilder::prerequisite_lanes_for(
    std::string const &instance_id) const
{
    auto const it = prerequisite_lanes_by_instance_id_.find(instance_id);
    if (it == prerequisite_lanes_by_instance_id_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void IvModuleInstanceBuildersAckBuilder::set_version_index(std::uint64_t version_index)
{
    version_index_ = version_index;
}

std::optional<std::uint64_t> IvModuleInstanceBuildersAckBuilder::version_index() const
{
    return version_index_;
}

void IvModuleInstanceBuildersAckBuilder::set_public_ports(
    GraphInputPublicPortsSnapshot public_ports)
{
    public_ports_ = std::move(public_ports);
}

std::optional<GraphInputPublicPortsSnapshot>
IvModuleInstanceBuildersAckBuilder::take_public_ports()
{
    return std::exchange(public_ports_, std::nullopt);
}

IV_DEFINE_LINKER_EVENT(
    IvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleInstanceBuildersChangedEvent,
    iv_runtime_iv_module_instance_builders_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleInstancesConfiguredEvent,
    iv_runtime_iv_module_instances_configured_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event);
} // namespace iv
