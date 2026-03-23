#pragma once

#include <filesystem>
#include <string>

namespace iv {
    struct ModuleDependency {
        std::string id;
        std::filesystem::path module_dir;
        std::filesystem::path entry_file;
        std::filesystem::file_time_type source_stamp {};
    };
}
