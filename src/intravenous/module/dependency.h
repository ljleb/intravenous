#pragma once

#include <filesystem>
#include <string>

namespace iv {
    inline bool is_module_dependency_source_path(std::filesystem::path const& path)
    {
        auto const filename = path.filename();
        return filename != ".intravenous";
    }

    struct ModuleDependency {
        std::string id;
        std::filesystem::path module_dir;
        std::filesystem::path entry_file;
        std::filesystem::file_time_type source_stamp {};
    };
}
