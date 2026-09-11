#pragma once

#include <string_view>

namespace iv {

// `iv_package.json` names the independently built and watched loader package.
inline constexpr std::string_view IV_PACKAGE_MANIFEST_FILE = "iv_package.json";

constexpr bool is_iv_package_manifest_file(std::string_view filename)
{
    return filename == IV_PACKAGE_MANIFEST_FILE;
}

} // namespace iv
