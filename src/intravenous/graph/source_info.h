#pragma once

#include <cstdint>
#include <string>

namespace iv {

struct SourceSpan {
  std::string file_path{};
  uint32_t begin = 0;
  uint32_t end = 0;

  bool operator==(SourceSpan const&) const = default;
};

struct SourceInfo {
  std::string declaration_identity{};
  SourceSpan span{};
  std::string declaration_name{};
  // One-based line when the span identifies the declaration.
  uint32_t declaration_line = 0;

  bool operator==(SourceInfo const&) const = default;
};

} // namespace iv
