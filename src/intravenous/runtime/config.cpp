#include <intravenous/runtime/config.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace iv {
namespace {
std::filesystem::path normalize_path(std::filesystem::path const &path)
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical.lexically_normal();
    }

    return std::filesystem::absolute(path).lexically_normal();
}

std::string trim(std::string value)
{
    auto const is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::optional<std::filesystem::path> parse_path_value(
    std::string const &value,
    std::filesystem::path const &base_dir)
{
    if (value.empty()) {
        return std::nullopt;
    }
    std::filesystem::path candidate = value;
    if (!candidate.is_absolute()) {
        candidate = base_dir / candidate;
    }
    return normalize_path(candidate);
}

std::optional<size_t> parse_size_t(std::string const &value)
{
    if (value.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        auto const wide = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        if (wide > std::numeric_limits<size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<size_t>(wide);
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace

ProjectConfig load_runtime_installation_config(std::filesystem::path const &workspace_root)
{
    auto const normalized_workspace = normalize_path(workspace_root);

    ProjectConfig config{
        .workspace_root = normalized_workspace,
    };

    if (char const *env_dir = std::getenv("INTRAVENOUS_DIR"); env_dir && *env_dir != '\0') {
        std::filesystem::path install_dir = env_dir;
        if (!install_dir.is_absolute()) {
            throw std::runtime_error("INTRAVENOUS_DIR must be absolute: " + install_dir.string());
        }

        auto const defaults_path = normalize_path(install_dir) / ".intravenous_defaults";
        if (!std::filesystem::exists(defaults_path)) {
            return config;
        }

        std::ifstream in(defaults_path);
        if (!in) {
            throw std::runtime_error("failed to open " + defaults_path.string());
        }

        for (std::string line; std::getline(in, line);) {
            line = trim(std::move(line));
            if (line.empty() || line.starts_with('#')) {
                continue;
            }

            auto const equals = line.find('=');
            if (equals == std::string::npos) {
                throw std::runtime_error("invalid config line in " + defaults_path.string() + ": " + line);
            }

            auto const key = trim(line.substr(0, equals));
            auto const value = trim(line.substr(equals + 1));

            auto assign_path = [&](std::optional<std::filesystem::path> &target) {
                target = parse_path_value(value, defaults_path.parent_path());
            };
            auto assign_size = [&](size_t &target) {
                auto parsed = parse_size_t(value);
                if (!parsed) {
                    throw std::runtime_error(
                        "invalid numeric config value in " + defaults_path.string() + ": " + key + "=" + value);
                }
                target = *parsed;
            };

            if (key == "c_compiler") {
                assign_path(config.toolchain.c_compiler);
            } else if (key == "cxx_compiler") {
                assign_path(config.toolchain.cxx_compiler);
            } else if (key == "cmake_program") {
                assign_path(config.toolchain.cmake_program);
            } else if (key == "cmake_generator") {
                config.toolchain.cmake_generator =
                    value.empty() ? std::nullopt : std::optional(value);
            } else if (key == "make_program") {
                assign_path(config.toolchain.make_program);
            } else if (key == "juce_dir") {
                assign_path(config.toolchain.juce_dir);
            } else if (key == "sample_rate") {
                assign_size(config.execution.sample_rate);
            } else if (key == "block_size") {
                assign_size(config.execution.block_size);
            } else if (key == "compiled_sample_cache_chunk_size_multiplier") {
                assign_size(config.execution.compiled_sample_cache_chunk_size_multiplier);
            } else if (key == "output_device_id") {
                config.output_device_id =
                    value.empty() ? std::nullopt : std::optional(value);
            } else if (key == "input_device_id") {
                config.input_device_id =
                    value.empty() ? std::nullopt : std::optional(value);
            } else {
                throw std::runtime_error("unknown config key in " + defaults_path.string() + ": " + key);
            }
        }
    }

    return config;
}
} // namespace iv
