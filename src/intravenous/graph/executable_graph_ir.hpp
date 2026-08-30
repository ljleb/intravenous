#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/compiler.h>

#include <string>
#include <vector>

namespace iv {

// A closed executable graph. All semantic node/edge synthesis and authored
// provenance transfer is complete before this value is constructed.
struct ExecutableGraphIR {
  std::string graph_id{};
  details::PreparedGraph graph{};
  std::vector<LoweredSubgraphSpec> scopes{};
  std::vector<InputConfig> public_inputs{};
  std::vector<OutputConfig> public_outputs{};
  std::vector<EventInputConfig> public_event_inputs{};
  std::vector<EventOutputConfig> public_event_outputs{};
  GraphIntrospectionMetadata introspection{};
};

} // namespace iv
