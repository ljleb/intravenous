#include <intravenous/runtime/config.h>

#include <intravenous/compat.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace iv {
    namespace {
        std::filesystem::path normalize_path(std::filesystem::path const& path)
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
            auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
                value.erase(value.begin());
            }
            while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
                value.pop_back();
            }
            return value;
        }

        std::optional<std::filesystem::path> path_value(
            std::string const& value,
            std::filesystem::path const& base_dir
        )
        {
            if (value.empty()) {
                return std::nullopt;
            }

            std::filesystem::path path = value;
            if (!path.is_absolute()) {
                path = base_dir / path;
            }
            return normalize_path(path);
        }

        std::optional<size_t> parse_size_t(std::string const& value)
        {
            if (value.empty()) {
                return std::nullopt;
            }
            size_t parsed = 0;
            try {
                std::size_t consumed = 0;
                auto const wide = std::stoull(value, &consumed, 10);
                if (consumed != value.size()) {
                    return std::nullopt;
                }
                if (wide > std::numeric_limits<size_t>::max()) {
                    return std::nullopt;
                }
                parsed = static_cast<size_t>(wide);
            } catch (...) {
                return std::nullopt;
            }
            return parsed;
        }

        struct ParsedExecutionConfig {
            std::optional<size_t> sample_rate;
            std::optional<size_t> block_size;
        };

        struct ParsedConfig {
            ModuleLoader::ToolchainConfig toolchain;
            ParsedExecutionConfig execution;
        };

        ParsedConfig parse_config_file(
            std::filesystem::path const& path,
            std::filesystem::path const& base_dir
        )
        {
            std::ifstream in(path);
            if (!in) {
                throw std::runtime_error("failed to open " + path.string());
            }

            ParsedConfig parsed;
            for (std::string line; std::getline(in, line);) {
                line = trim(std::move(line));
                if (line.empty() || line.starts_with('#')) {
                    continue;
                }

                auto const equals = line.find('=');
                if (equals == std::string::npos) {
                    throw std::runtime_error("invalid config line in " + path.string() + ": " + line);
                }

                auto const key = trim(line.substr(0, equals));
                auto const value = trim(line.substr(equals + 1));

                auto assign_path = [&](std::optional<std::filesystem::path>& target) {
                    target = path_value(value, base_dir);
                };
                auto assign_size = [&](std::optional<size_t>& target) {
                    auto parsed = parse_size_t(value);
                    if (!parsed) {
                        throw std::runtime_error("invalid numeric config value in " + path.string() + ": " + key + "=" + value);
                    }
                    target = parsed;
                };

                if (key == "c_compiler") {
                    assign_path(parsed.toolchain.c_compiler);
                } else if (key == "cxx_compiler") {
                    assign_path(parsed.toolchain.cxx_compiler);
                } else if (key == "cmake_program") {
                    assign_path(parsed.toolchain.cmake_program);
                } else if (key == "cmake_generator") {
                    parsed.toolchain.cmake_generator = value.empty() ? std::nullopt : std::optional(value);
                } else if (key == "make_program") {
                    assign_path(parsed.toolchain.make_program);
                } else if (key == "juce_dir") {
                    assign_path(parsed.toolchain.juce_dir);
                } else if (key == "sample_rate") {
                    assign_size(parsed.execution.sample_rate);
                } else if (key == "block_size") {
                    assign_size(parsed.execution.block_size);
                } else {
                    throw std::runtime_error("unknown config key in " + path.string() + ": " + key);
                }
            }

            return parsed;
        }

        void overlay_toolchain(
            ModuleLoader::ToolchainConfig& base,
            ModuleLoader::ToolchainConfig const& overlay
        )
        {
            if (overlay.c_compiler) {
                base.c_compiler = overlay.c_compiler;
            }
            if (overlay.cxx_compiler) {
                base.cxx_compiler = overlay.cxx_compiler;
            }
            if (overlay.cmake_program) {
                base.cmake_program = overlay.cmake_program;
            }
            if (overlay.cmake_generator) {
                base.cmake_generator = overlay.cmake_generator;
            }
            if (overlay.make_program) {
                base.make_program = overlay.make_program;
            }
            if (overlay.juce_dir) {
                base.juce_dir = overlay.juce_dir;
            }
        }

        void overlay_execution(
            RuntimeExecutionConfig& base,
            ParsedExecutionConfig const& overlay)
        {
            if (overlay.sample_rate) {
                base.sample_rate = *overlay.sample_rate;
            }
            if (overlay.block_size) {
                base.block_size = *overlay.block_size;
            }
        }
    }

    ProjectConfig load_runtime_project_config(std::filesystem::path const& workspace_root)
    {
        auto const normalized_workspace = normalize_path(workspace_root);
        auto const marker_path = normalized_workspace / ".intravenous";
        if (!std::filesystem::exists(marker_path)) {
            throw std::runtime_error("missing .intravenous marker at " + marker_path.string());
        }

        ProjectConfig config {
            .workspace_root = normalized_workspace,
        };

        if (char const* env_dir = std::getenv("INTRAVENOUS_DIR"); env_dir && *env_dir != '\0') {
            std::filesystem::path install_dir = env_dir;
            if (!install_dir.is_absolute()) {
                throw std::runtime_error("INTRAVENOUS_DIR must be absolute: " + install_dir.string());
            }

            auto const defaults_path = normalize_path(install_dir) / ".intravenous_defaults";
            if (std::filesystem::exists(defaults_path)) {
                auto const parsed_defaults =
                    parse_config_file(defaults_path, defaults_path.parent_path());
                overlay_toolchain(config.toolchain, parsed_defaults.toolchain);
                overlay_execution(config.execution, parsed_defaults.execution);
            }
        }

        auto const project_config = parse_config_file(marker_path, normalized_workspace);
        overlay_toolchain(config.toolchain, project_config.toolchain);
        overlay_execution(config.execution, project_config.execution);

        return config;
    }
}
