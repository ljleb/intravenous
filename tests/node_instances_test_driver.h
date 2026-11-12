#pragma once

#include "event_source_test_support.h"

#include <intravenous/graph/builder.h>
#include <intravenous/runtime/node_instances.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace iv::test {
// Drives NodeInstances the way ProjectGraph does: it pins one definitions
// snapshot and applies every mutation as one handle_project_graph_transaction,
// which also realizes the complete desired set into a fresh root builder. Each
// call is one event source entry.
class NodeInstancesDriver {
    NodeInstances* instances_ = nullptr;
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot_ =
        std::make_shared<NodeDefinitionsSnapshot const>();

public:
    explicit NodeInstancesDriver(NodeInstances& instances)
        : instances_(&instances)
    {}

    [[nodiscard]] std::shared_ptr<NodeDefinitionsSnapshot const> const& snapshot() const noexcept
    {
        return snapshot_;
    }

    // Applies `mutation` against the pinned snapshot and returns the completed
    // transaction record. Its root builder does not outlive the call.
    NodeInstancesProjectGraphRequest run(NodeInstancesMutation mutation = std::monostate{})
    {
        GraphBuilder root;
        NodeInstancesProjectGraphRequest request{
            .snapshot = snapshot_,
            .root_builder = &root,
            .mutation = std::move(mutation),
        };
        in_event_source([&] { instances_->handle_project_graph_transaction(request); });
        request.root_builder = nullptr;
        return request;
    }

    // Pins `snapshot` and installs it with a transaction that changes no
    // desired state, as ProjectGraph's next rebuild would.
    NodeInstancesProjectGraphRequest install(
        std::shared_ptr<NodeDefinitionsSnapshot const> snapshot)
    {
        snapshot_ = std::move(snapshot);
        return run();
    }

    std::string create(
        std::string definition_id,
        std::optional<std::filesystem::path> package_root = std::nullopt,
        std::optional<std::string> instance_id = std::nullopt,
        std::optional<std::string> display_name = std::nullopt,
        std::string configuration_source = {})
    {
        auto request = run(NodeInstanceCreateMutation{
            .instance_id = std::move(instance_id),
            .definition_id = std::move(definition_id),
            .package_root = std::move(package_root),
            .display_name = std::move(display_name),
            .configuration_source = std::move(configuration_source),
        });
        if (request.created_instance_ids.size() != 1) {
            throw std::logic_error("NodeInstances did not return one created instance id");
        }
        return std::move(request.created_instance_ids.front());
    }

    void remove(std::string instance_id)
    {
        (void)run(NodeInstanceDeleteMutation{.instance_id = std::move(instance_id)});
    }

    void update(std::vector<NodeInstanceDisplayNameUpdate> updates)
    {
        (void)run(NodeInstanceUpdateMutation{.updates = std::move(updates)});
    }
};
} // namespace iv::test
