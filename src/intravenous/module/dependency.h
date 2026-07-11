#pragma once

#include <filesystem>
#include <string>

namespace iv {
    inline bool is_module_dependency_source_path(std::filesystem::path const& path)
    {
        auto const filename = path.filename();
        return filename != "iv_project.jsonl"
            && filename != "iv_project.jsonl.tmp"
            && filename != "compile_commands.json"
            && path.parent_path().filename() != ".intravenous-tooling";
    }

    struct ModuleDependency {
        std::string id;
        std::filesystem::path module_dir;
        std::filesystem::path entry_file;
        std::filesystem::file_time_type source_stamp {};
    };
}
