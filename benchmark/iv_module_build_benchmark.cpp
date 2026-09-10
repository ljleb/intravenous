#include <intravenous/module/loader.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct NinjaEdge {
    std::string output;
    std::int64_t duration_ms = 0;
};

struct PhaseResult {
    std::int64_t pipeline_ms = 0;
    std::int64_t pch_ms = 0;
    std::int64_t export_ms = 0;
    std::int64_t link_ms = 0;
    std::int64_t configure_us = 0;
    std::int64_t ninja_build_us = 0;
    std::int64_t generation_copy_us = 0;
    std::vector<std::pair<std::string, std::int64_t>> finalizer_timings;
    bool ninja_log_delta_available = true;
};


enum class SourceShape {
    empty,
    input,
    nodes,
    connected,
    full,
};

struct Options {
    std::filesystem::path workspace =
        std::filesystem::temp_directory_path() / "intravenous-module-build-benchmark";
    size_t voices = 1;
    bool keep_workspace = false;
    iv::ModuleCompileStage compile_stage = iv::ModuleCompileStage::full;
    iv::ModuleOptimization optimization = iv::ModuleOptimization::O3;
    bool source_introspection = true;
    bool precompiled_header = true;
    bool clang_time_trace = false;
    SourceShape source_shape = SourceShape::full;
    std::optional<std::filesystem::path> source_module;
    std::optional<std::filesystem::path> c_compiler;
    std::optional<std::filesystem::path> cxx_compiler;
};

std::string_view source_shape_name(SourceShape shape)
{
    switch (shape) {
    case SourceShape::empty: return "empty";
    case SourceShape::input: return "input";
    case SourceShape::nodes: return "nodes";
    case SourceShape::connected: return "connected";
    case SourceShape::full: return "full";
    }
    throw std::logic_error("invalid source shape");
}

SourceShape parse_source_shape(std::string_view value)
{
    if (value == "empty") return SourceShape::empty;
    if (value == "input") return SourceShape::input;
    if (value == "nodes") return SourceShape::nodes;
    if (value == "connected") return SourceShape::connected;
    if (value == "full") return SourceShape::full;
    throw std::runtime_error("invalid source shape '" + std::string(value) + "'");
}

std::string_view compile_stage_name(iv::ModuleCompileStage stage)
{
    switch (stage) {
    case iv::ModuleCompileStage::full: return "full";
    case iv::ModuleCompileStage::authoring: return "authoring";
    case iv::ModuleCompileStage::lowering_topology:
        return "lowering-topology";
    case iv::ModuleCompileStage::lowering_materialization:
        return "lowering-materialization";
    case iv::ModuleCompileStage::lowering_normalization:
        return "lowering-normalization";
    case iv::ModuleCompileStage::lowering: return "lowering";
    case iv::ModuleCompileStage::compilation: return "compilation";
    case iv::ModuleCompileStage::static_metadata: return "static-metadata";
    }
    throw std::logic_error("invalid module compile stage");
}

iv::ModuleCompileStage parse_compile_stage(std::string_view value)
{
    if (value == "full") return iv::ModuleCompileStage::full;
    if (value == "authoring") return iv::ModuleCompileStage::authoring;
    if (value == "lowering-topology")
        return iv::ModuleCompileStage::lowering_topology;
    if (value == "lowering-materialization")
        return iv::ModuleCompileStage::lowering_materialization;
    if (value == "lowering-normalization")
        return iv::ModuleCompileStage::lowering_normalization;
    if (value == "lowering") return iv::ModuleCompileStage::lowering;
    if (value == "compilation") return iv::ModuleCompileStage::compilation;
    if (value == "static-metadata") return iv::ModuleCompileStage::static_metadata;
    throw std::runtime_error("invalid compile stage '" + std::string(value) + "'");
}

std::string_view optimization_name(iv::ModuleOptimization optimization)
{
    switch (optimization) {
    case iv::ModuleOptimization::O0: return "O0";
    case iv::ModuleOptimization::O3: return "O3";
    }
    throw std::logic_error("invalid module optimization");
}

iv::ModuleOptimization parse_optimization(std::string_view value)
{
    if (value == "O0") return iv::ModuleOptimization::O0;
    if (value == "O3") return iv::ModuleOptimization::O3;
    throw std::runtime_error("invalid optimization '" + std::string(value) + "'");
}

std::string read(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to read '" + path.string() + "'");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(std::filesystem::path const& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("failed to write '" + path.string() + "'");
    output << text;
}

std::string benchmark_source(size_t voices, SourceShape shape)
{
    std::ostringstream source;
    source << "#include <intravenous/dsl.h>\n";
    if (shape == SourceShape::nodes
        || shape == SourceShape::connected
        || shape == SourceShape::full) {
        source << "#include <intravenous/basic_nodes/shaping.h>\n";
    }
    source << "\nvoid module_main(iv::GraphBuilder& g)\n"
           << "{\n";
    if (shape == SourceShape::empty) {
        source << "    (void)g;\n"
               << "}\n";
        return source.str();
    }
    source << "    using namespace iv;\n"
           << "    auto const frequency = g.input<\"frequency\">(220.0f);\n";
    if (shape == SourceShape::input) {
        source << "}\n";
        return source.str();
    }
    for (size_t voice = 0; voice < voices; ++voice) {
        source << "    auto const osc" << voice << " = g.node<SawOscillator>();\n";
    }
    if (shape == SourceShape::nodes) {
        source << "}\n";
        return source.str();
    }
    for (size_t voice = 0; voice < voices; ++voice) {
        source << "    osc" << voice << "(\"frequency\"_P = frequency + "
               << std::fixed << std::setprecision(3)
               << static_cast<float>(voice) * 0.125f << "f);\n";
    }
    if (shape == SourceShape::connected) {
        source << "}\n";
        return source.str();
    }
    source << "    g.outputs(\"main\"_P = ";
    if (voices == 0) {
        source << "frequency";
    } else {
        source << "osc0";
        for (size_t voice = 1; voice < voices; ++voice) {
            source << " + osc" << voice;
        }
    }
    source << ");\n}\n";
    return source.str();
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto require_value = [&](std::string_view name) -> std::string_view {
            if (++i == argc) throw std::runtime_error("missing value for " + std::string(name));
            return argv[i];
        };
        if (arg == "--workspace") {
            options.workspace = require_value(arg);
        } else if (arg == "--voices") {
            options.voices = std::stoull(std::string(require_value(arg)));
        } else if (arg == "--keep") {
            options.keep_workspace = true;
        } else if (arg == "--stage") {
            options.compile_stage = parse_compile_stage(require_value(arg));
        } else if (arg == "--optimization") {
            options.optimization = parse_optimization(require_value(arg));
        } else if (arg == "--no-source-introspection") {
            options.source_introspection = false;
        } else if (arg == "--no-pch") {
            options.precompiled_header = false;
        } else if (arg == "--clang-time-trace") {
            options.clang_time_trace = true;
        } else if (arg == "--source-shape") {
            options.source_shape = parse_source_shape(require_value(arg));
        } else if (arg == "--module") {
            options.source_module = require_value(arg);
        } else if (arg == "--c-compiler") {
            options.c_compiler = require_value(arg);
        } else if (arg == "--cxx-compiler") {
            options.cxx_compiler = require_value(arg);
        } else if (arg == "--help") {
            std::cout
                << "Usage: iv_module_build_benchmark [--voices N] [--workspace PATH]"
                << " [--stage full|authoring|lowering-topology|lowering-materialization|lowering-normalization|lowering|compilation|static-metadata]"
                << " [--optimization O0|O3]"
                << " [--source-shape empty|input|nodes|connected|full]"
                << " [--module PATH]"
                << " [--c-compiler PATH] [--cxx-compiler PATH]"
                << " [--no-source-introspection] [--no-pch] [--clang-time-trace]"
                << " [--keep]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument '" + std::string(arg) + "'");
        }
    }
    if (options.c_compiler.has_value() != options.cxx_compiler.has_value()) {
        throw std::runtime_error(
            "--c-compiler and --cxx-compiler must be provided together");
    }
    return options;
}

std::filesystem::path module_directory(std::filesystem::path path)
{
    path = std::filesystem::absolute(path).lexically_normal();
    if (std::filesystem::is_regular_file(path)) {
        if (path.filename() != "iv_source.json") {
            throw std::runtime_error(
                "source path must be a directory or iv_source.json: '" +
                path.string() + "'");
        }
        path = path.parent_path();
    }
    if (!std::filesystem::is_directory(path)
        || !std::filesystem::exists(path / "iv_source.json")) {
        throw std::runtime_error(
            "source path is missing iv_source.json: '" + path.string() + "'");
    }
    return path;
}

std::filesystem::path project_directory(std::filesystem::path module)
{
    for (;;) {
        if (std::filesystem::exists(module / "iv_project.jsonl")) return module;
        auto const parent = module.parent_path();
        if (parent == module) break;
        module = parent;
    }
    throw std::runtime_error(
        "module is not contained in an IV project (iv_project.jsonl not found)");
}

bool excluded_project_directory(std::filesystem::path const& path)
{
    auto const name = path.filename();
    return name == "build" || name == "out" || name == ".git";
}

void copy_project_source(
    std::filesystem::path const& source,
    std::filesystem::path const& destination)
{
    std::filesystem::create_directories(destination);
    for (std::filesystem::recursive_directory_iterator it(source), end;
         it != end;
         ++it) {
        auto const& entry = *it;
        auto const relative = entry.path().lexically_relative(source);
        auto const target = destination / relative;
        if (entry.is_directory()) {
            if (excluded_project_directory(entry.path())) {
                it.disable_recursion_pending();
            } else {
                std::filesystem::create_directories(target);
            }
        } else if (entry.is_symlink()) {
            std::filesystem::copy_symlink(entry.path(), target);
        } else if (entry.is_regular_file()) {
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::copy_file(
                entry.path(), target, std::filesystem::copy_options::overwrite_existing);
        }
    }
}

std::filesystem::path find_ninja_log(std::filesystem::path const& workspace)
{
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::recursive_directory_iterator it(workspace), end; it != end; ++it) {
        if (it->is_regular_file() && it->path().filename() == ".ninja_log") {
            candidates.push_back(it->path());
        }
    }
    if (candidates.size() != 1) {
        throw std::runtime_error(
            "expected one module Ninja log below '" + workspace.string() + "'");
    }
    return candidates.front();
}

std::filesystem::path find_finalizer_timings(std::filesystem::path const& workspace)
{
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::recursive_directory_iterator it(workspace), end; it != end; ++it) {
        if (it->is_regular_file()
            && it->path().filename() == "iv-module-finalizer-timings.txt") {
            candidates.push_back(it->path());
        }
    }
    if (candidates.size() != 1) {
        throw std::runtime_error(
            "expected one finalizer timing report below '" + workspace.string() + "'");
    }
    return candidates.front();
}

std::vector<std::filesystem::path> clang_time_traces(
    std::filesystem::path const& cmake_build_directory)
{
    std::vector<std::filesystem::path> result;
    for (std::filesystem::recursive_directory_iterator it(cmake_build_directory), end;
         it != end;
         ++it) {
        if (!it->is_regular_file() || it->path().extension() != ".json") continue;
        auto const relative = it->path().lexically_relative(cmake_build_directory);
        for (auto const& component : relative) {
            if (component == "CMakeFiles"
                && read(it->path()).contains("\"traceEvents\"")) {
                result.push_back(it->path());
                break;
            }
        }
    }
    return result;
}

using ClangTimeTraceSnapshot = std::vector<
    std::pair<std::filesystem::path, std::filesystem::file_time_type>>;

ClangTimeTraceSnapshot clang_time_trace_snapshot(
    std::filesystem::path const& cmake_build_directory)
{
    ClangTimeTraceSnapshot result;
    for (auto const& trace : clang_time_traces(cmake_build_directory)) {
        result.emplace_back(trace, std::filesystem::last_write_time(trace));
    }
    return result;
}

void retain_clang_time_traces(
    std::filesystem::path const& cmake_build_directory,
    std::filesystem::path const& destination,
    ClangTimeTraceSnapshot const& previous = {})
{
    std::vector<std::filesystem::path> traces;
    for (auto const& trace : clang_time_traces(cmake_build_directory)) {
        auto const before = std::find_if(
            previous.begin(), previous.end(),
            [&](auto const& entry) { return entry.first == trace; });
        if (before == previous.end()
            || before->second != std::filesystem::last_write_time(trace)) {
            traces.push_back(trace);
        }
    }
    if (traces.empty()) {
        throw std::runtime_error(
            "Clang time tracing was requested, but no trace JSON was produced below '" +
            cmake_build_directory.string() + "'");
    }
    std::filesystem::remove_all(destination);
    for (auto const& trace : traces) {
        auto const relative = trace.lexically_relative(cmake_build_directory);
        auto const copy = destination / relative;
        std::filesystem::create_directories(copy.parent_path());
        std::filesystem::copy_file(
            trace, copy, std::filesystem::copy_options::overwrite_existing);
    }
}

std::vector<std::pair<std::string, std::int64_t>> finalizer_timings(
    std::filesystem::path const& path)
{
    std::istringstream lines(read(path));
    std::string line;
    if (!std::getline(lines, line) || line != "version=1") {
        throw std::runtime_error("invalid finalizer timing report '" + path.string() + "'");
    }

    std::vector<std::pair<std::string, std::int64_t>> result;
    for (; std::getline(lines, line);) {
        auto const separator = line.find('=');
        if (separator == std::string::npos
            || !line.substr(0, separator).ends_with("_us")) {
            throw std::runtime_error("invalid finalizer timing entry in '" + path.string() + "'");
        }
        result.emplace_back(
            line.substr(0, separator - std::string_view("_us").size()),
            std::stoll(line.substr(separator + 1)));
    }
    return result;
}

std::vector<NinjaEdge> ninja_edges(std::string_view log)
{
    std::vector<NinjaEdge> edges;
    std::istringstream lines{std::string(log)};
    for (std::string line; std::getline(lines, line);) {
        if (line.empty() || line.starts_with('#')) continue;
        auto const first = line.find('\t');
        auto const second = first == std::string::npos ? first : line.find('\t', first + 1);
        auto const third = second == std::string::npos ? second : line.find('\t', second + 1);
        auto const fourth = third == std::string::npos ? third : line.find('\t', third + 1);
        if (fourth == std::string::npos) continue;
        auto const start = std::stoll(line.substr(0, first));
        auto const end = std::stoll(line.substr(first + 1, second - first - 1));
        edges.push_back({
            .output = line.substr(third + 1, fourth - third - 1),
            .duration_ms = end - start,
        });
    }
    return edges;
}

std::optional<std::vector<NinjaEdge>> appended_ninja_edges(
    std::string_view before,
    std::string_view after)
{
    if (!after.starts_with(before)) return std::nullopt;
    return ninja_edges(after.substr(before.size()));
}

PhaseResult summarize(
    Clock::duration elapsed,
    std::optional<std::vector<NinjaEdge>> const& edges,
    std::vector<std::string> const& loader_log,
    std::vector<std::pair<std::string, std::int64_t>> finalizer_timing)
{
    PhaseResult result{
        .pipeline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
        .finalizer_timings = std::move(finalizer_timing),
        .ninja_log_delta_available = edges.has_value(),
    };
    for (auto const& entry : loader_log) {
        auto const parse = [&](std::string_view phase, std::int64_t& output) {
            auto const prefix = "[" + std::string(phase) + "] elapsed_us=";
            if (entry.starts_with(prefix)) output = std::stoll(entry.substr(prefix.size()));
        };
        parse("configure", result.configure_us);
        parse("build", result.ninja_build_us);
        parse("generation-copy", result.generation_copy_us);
    }
    if (edges) {
        for (auto const& edge : *edges) {
            if (edge.output.ends_with("cmake_pch.hxx.gch")) result.pch_ms += edge.duration_ms;
            if (edge.output.ends_with("root_export.cpp.o")) result.export_ms += edge.duration_ms;
            if (edge.output.ends_with(".so") || edge.output.ends_with(".dylib")
                || edge.output.ends_with(".dll")) result.link_ms += edge.duration_ms;
        }
    }
    return result;
}

void print(
    std::string_view phase,
    std::string_view workload,
    iv::ModuleCompileStage stage,
    iv::ModuleOptimization optimization,
    SourceShape shape,
    bool source_introspection,
    bool precompiled_header,
    PhaseResult const& result)
{
    std::cout << "iv-module-build-benchmark"
              << " phase=" << phase
              << " workload=" << workload
              << " stage=" << compile_stage_name(stage)
              << " optimization=" << optimization_name(optimization)
              << " source_shape=" << source_shape_name(shape)
              << " source_introspection=" << source_introspection
              << " pch=" << precompiled_header
              << " pipeline_ms=" << result.pipeline_ms
              << " pch_ms=" << result.pch_ms
              << " export_ms=" << result.export_ms
              << " link_ms=" << result.link_ms
              << " configure_us=" << result.configure_us
              << " ninja_build_us=" << result.ninja_build_us
              << " generation_copy_us=" << result.generation_copy_us
              << " ninja_log_delta=" << result.ninja_log_delta_available << '\n';
    for (auto const& [name, elapsed_us] : result.finalizer_timings) {
        std::cout << "iv-module-build-benchmark"
                  << " phase=" << phase
                  << " finalizer_" << name << "_us=" << elapsed_us << '\n';
    }
}

void run(Options const& options)
{
    auto const marker = options.workspace / ".iv-module-build-benchmark";
    if (std::filesystem::exists(options.workspace)) {
        if (!std::filesystem::exists(marker)) {
            throw std::runtime_error(
                "refusing to clear benchmark workspace without marker '" +
                marker.string() + "'");
        }
        std::filesystem::remove_all(options.workspace);
    }
    write(marker, "managed by iv_module_build_benchmark\n");

    std::filesystem::path module;
    std::filesystem::path hot_source;
    std::string workload = "synthetic";
    if (options.source_module) {
        auto const source_module = module_directory(*options.source_module);
        auto const source_project = project_directory(source_module);
        auto const copied_project = options.workspace / "project";
        copy_project_source(source_project, copied_project);
        module = copied_project / source_module.lexically_relative(source_project);
        hot_source = module / "module.cpp";
        if (!std::filesystem::exists(hot_source)) {
            throw std::runtime_error(
                "benchmark snapshots currently require module.cpp as the entry file: '" +
                module.string() + "'");
        }
        workload = "project-module";
    } else {
        module = options.workspace / "modules" / "compile_benchmark";
        hot_source = module / "module.cpp";
        write(options.workspace / "iv_project.jsonl", "");
        write(module / "iv_source.json", R"({"schema":1,"id":"iv.benchmark.compile","entry":"module.cpp","main":"module_main"})");
        write(hot_source, benchmark_source(options.voices, options.source_shape));
    }
    auto source = read(hot_source);

    {
        std::vector<std::string> loader_log;
        iv::ModuleLoader loader(
            std::filesystem::current_path(), {},
            iv::ModuleLoaderToolchainConfig{
                .c_compiler = options.c_compiler,
                .cxx_compiler = options.cxx_compiler,
                .compile_stage = options.compile_stage,
                .optimization = options.optimization,
                .source_introspection = options.source_introspection,
                .precompiled_header = options.precompiled_header,
                .clang_time_trace = options.clang_time_trace,
            },
            [&](std::string const& entry) { loader_log.push_back(entry); });

        auto const cold_start = Clock::now();
        (void)loader.compile_root_definition(module);
        auto const cold_elapsed = Clock::now() - cold_start;
        auto const ninja_log = find_ninja_log(options.workspace);
        auto const cold_log = read(ninja_log);
        auto const cold_finalizer_timings = finalizer_timings(
            find_finalizer_timings(options.workspace));
        if (options.clang_time_trace) {
            retain_clang_time_traces(
                ninja_log.parent_path(), options.workspace / "clang-time-traces" / "cold");
        }
        auto const traces_before_hot = options.clang_time_trace
            ? clang_time_trace_snapshot(ninja_log.parent_path())
            : ClangTimeTraceSnapshot{};
        print(
            "cold", workload, options.compile_stage, options.optimization,
            options.source_shape,
            options.source_introspection, options.precompiled_header,
            summarize(
                cold_elapsed,
                ninja_edges(cold_log),
                loader_log,
                cold_finalizer_timings));

        source += "// Hot-reload marker.\n";
        write(hot_source, source);
        loader_log.clear();
        auto const hot_start = Clock::now();
        (void)loader.compile_root_definition(module);
        auto const hot_elapsed = Clock::now() - hot_start;
        auto const hot_log = read(ninja_log);
        auto const hot_finalizer_timings = finalizer_timings(
            find_finalizer_timings(options.workspace));
        if (options.clang_time_trace) {
            retain_clang_time_traces(
                ninja_log.parent_path(), options.workspace / "clang-time-traces" / "hot",
                traces_before_hot);
        }
        print(
            "hot", workload, options.compile_stage, options.optimization,
            options.source_shape,
            options.source_introspection, options.precompiled_header,
            summarize(
                hot_elapsed,
                appended_ninja_edges(cold_log, hot_log),
                loader_log,
                hot_finalizer_timings));

    }

    if (options.keep_workspace) {
        std::cout << "iv-module-build-benchmark workspace="
                  << options.workspace.string() << '\n';
    } else {
        std::filesystem::remove_all(options.workspace);
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        run(parse_options(argc, argv));
    } catch (std::exception const& error) {
        std::cerr << "iv-module-build-benchmark: " << error.what() << '\n';
        return 1;
    }
}
