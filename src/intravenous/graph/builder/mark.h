#pragma once

#include <cstddef>

namespace iv {
// Table sizes when a registered-definition invocation began. Bundles,
// connections, and virtual records are appended in construction order, so an
// invocation's contents are the entries after these sizes. Nothing is ever
// truncated: a failed construction fails its whole configuration pass.
struct GraphBuilderMark {
  std::size_t bundles = 0;
  std::size_t sample_connections = 0;
  std::size_t event_connections = 0;
  std::size_t virtual_records = 0;
};
} // namespace iv
