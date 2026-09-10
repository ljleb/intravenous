#pragma once

#include <string_view>

namespace iv {

// `iv_source.json` names the independently built and watched loader package.
inline constexpr std::string_view IV_SOURCE_MANIFEST_FILE = "iv_source.json";

constexpr bool is_iv_source_manifest_file(std::string_view filename)
{
    return filename == IV_SOURCE_MANIFEST_FILE;
}

} // namespace iv
