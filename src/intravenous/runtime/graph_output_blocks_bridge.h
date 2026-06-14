#pragma once

namespace iv {
class GraphOutputBlocks;

void bind_graph_output_blocks_bridge(GraphOutputBlocks &blocks);
void unbind_graph_output_blocks_bridge(GraphOutputBlocks const &blocks);
} // namespace iv
