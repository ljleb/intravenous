#pragma once

#include "scc_wrapper.h"
#include "types.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace iv {
    struct GraphBuildArtifact {
        std::vector<GraphSccWrapper> scc_wrappers;
        std::unordered_set<GraphEdge> edges;
        std::vector<DetachedInfo> detached;
        GraphExecutionPlan execution_plan;
        std::vector<InputConfig> public_inputs;
        std::vector<OutputConfig> public_outputs;
        std::vector<size_t> public_output_sample_sizes;
        size_t internal_latency = 0;
        std::vector<std::string> node_ids;
    };
}
