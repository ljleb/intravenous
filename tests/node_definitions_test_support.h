#pragma once

#include "event_source_test_support.h"

#include <intravenous/filesystem_paths.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/package_pipeline_types.h>

#include <memory>
#include <string>
#include <utility>

namespace iv::test {
// Publishes one loaded module definition through the edge PackageDefinitions
// uses: one accepted package revision in a one-package snapshot, handed to
// NodeDefinitions::handle_package_definitions_publication as one event source
// entry.
inline void publish_loaded_definition(
    NodeDefinitions& definitions,
    PackageModuleDefinition loaded_definition)
{
    if (loaded_definition.package_id.empty()) {
        loaded_definition.package_id = loaded_definition.package_root.empty()
            ? loaded_definition.definition_id
            : normalize_path(loaded_definition.package_root).generic_string();
    }
    auto const package_id = loaded_definition.package_id;
    auto revision = std::make_shared<PackageRevision const>(PackageRevision{
        .package_id = package_id,
        .package_root = loaded_definition.package_root,
        .revision = 1,
        .module_definitions = {std::move(loaded_definition)},
    });
    auto snapshot = std::make_shared<PackageDefinitionsSnapshot>();
    snapshot->generation = 1;
    snapshot->by_package_id.emplace(package_id, std::move(revision));
    PackageDefinitionsPublicationRequest request{.snapshot = std::move(snapshot)};
    in_event_source([&] { definitions.handle_package_definitions_publication(request); });
}
} // namespace iv::test
