#pragma once

#include <intravenous/graph/runtime_bindings.h>

#include <memory>

namespace iv {

std::shared_ptr<GraphRuntimeBindings> make_graph_runtime_bindings();

} // namespace iv
