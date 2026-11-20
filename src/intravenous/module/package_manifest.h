#pragma once

#include <string_view>

namespace iv {

// `iv_package.json` names the independently built and watched loader package.
inline constexpr std::string_view IV_PACKAGE_MANIFEST_FILE = "iv_package.json";

constexpr bool is_iv_package_manifest_file(std::string_view filename)
{
    return filename == IV_PACKAGE_MANIFEST_FILE;
}

// A namespace owns its exact ID and dotted descendants. Lookup compares
// complete components, so iv.filters never owns iv.filters_extra.
constexpr bool package_namespace_owns(std::string_view package_namespace, std::string_view definition)
{
    return !package_namespace.empty() && (definition == package_namespace || (definition.size() > package_namespace.size()
        && definition.starts_with(package_namespace) && definition[package_namespace.size()] == '.'));
}

// The executable's builtin package is the default import, not an authored
// dependency that every manifest must repeat.
constexpr bool is_implicit_package_dependency(std::string_view owner, std::string_view dependency)
{
    return owner != "builtin" && dependency == "builtin";
}

} // namespace iv
