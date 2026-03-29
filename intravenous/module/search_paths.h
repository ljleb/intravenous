#pragma once

#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>

namespace iv {
    inline constexpr char module_search_path_separator()
    {
#if defined(_WIN32)
        return ';';
#else
        return ':';
#endif
    }

    inline std::vector<std::filesystem::path> parse_search_path_env()
    {
        std::vector<std::filesystem::path> roots;
        char const* value = std::getenv("IV_MODULE_SEARCH_PATH");
        if (!value || !*value) {
            return roots;
        }

        std::string_view remaining(value);
        while (!remaining.empty()) {
            size_t split = remaining.find(module_search_path_separator());
            std::string_view token = remaining.substr(0, split);
            if (!token.empty()) {
                roots.emplace_back(std::string(token));
            }
            if (split == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(split + 1);
        }

        return roots;
    }
}
