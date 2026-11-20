#include "cache.h"

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
namespace cache = iv::build_cache;

void log(std::string const& message)
{
    std::cerr << "ivctl: " << message << '\n';
}

void print_report(fs::path const& root, cache::Report const& report, cache::Mode mode)
{
    bool const status = mode == cache::Mode::status;
    bool const dry_run = mode == cache::Mode::dry_run;
    std::cout << "Build cache at " << root.string() << '\n';
    auto section = [&](std::string_view category, std::vector<cache::Decision> const& decisions) {
        std::vector<cache::Decision const*> removed, kept;
        for (auto const& decision : decisions) {
            (decision.remove ? removed : kept).push_back(&decision);
        }
        std::cout << category;
        if (status) std::cout << " (present " << kept.size() << ")\n";
        else std::cout << (dry_run ? " (would remove " : " (removed ") << removed.size()
                      << (dry_run ? ", would keep " : ", kept ") << kept.size() << ")\n";
        auto print = [&](std::string_view action, auto const& rows) {
            for (auto const* row : rows) {
                auto const relative = row->path.lexically_relative(root).string();
                std::cout << "  " << action << ' ';
                if (row->source.empty()) std::cout << relative;
                else std::cout << row->source << " [" << relative << ']';
                std::cout << " (" << row->reason << ")\n";
            }
        };
        print(dry_run ? "Would remove" : "Removed", removed);
        print(status ? "Present" : (dry_run ? "Would keep" : "Kept"), kept);
    };
    section("Build workspaces", report.workspaces);
    section("Package artifact sets", report.artifacts);
    section("Scratch directories", report.scratch);

    constexpr double gib = 1024.0 * 1024 * 1024;
    std::cout << std::fixed << std::setprecision(2);
    if (dry_run) {
        std::cout << "Managed cache uses " << report.bytes / gib << " GiB now and would use about "
                  << report.remaining_bytes / gib << " GiB after cleanup. Estimated free space after cleanup is "
                  << report.predicted_free_bytes / gib << " GiB.\n";
    } else {
        std::cout << "Managed cache now uses " << report.remaining_bytes / gib << " GiB. Filesystem has "
                  << report.free_bytes / gib << " GiB free.\n";
    }
}

void help()
{
    std::cout << "Intravenous command-line tools\n"
              << "  ivctl cache path                    Print the build cache directory\n"
              << "  ivctl cache status                  Inspect the cache\n"
              << "  ivctl cache gc [--dry-run]           Collect the cache or preview cleanup\n"
              << "  ivctl cache config [-h|-H]           Show the effective cache policy\n"
              << "  ivctl cache config set KEY=VALUE ...  Set cache policy values\n"
              << "  ivctl cache config reset [KEY ...]   Reset selected keys or the entire policy\n"
              << "  -h displays binary units (K, M, G, T). -H displays decimal units (kB, MB, GB, TB).\n"
              << "  Keys: thresholdBytes, targetBytes, minimumFreeBytes, maxIdleDays\n"
              << "  Size examples: 10.5G or 10.5GiB (binary), 10.5GB (decimal), 4096 (bytes)\n";
}

void configure(std::span<char* const> arguments)
{
    if (!arguments.empty()) {
        auto const action = std::string_view(arguments.front());
        arguments = arguments.subspan(1);
        if (action == "-h" || action == "-H") {
            if (!arguments.empty()) {
                throw std::invalid_argument("Use -h or -H only when displaying the cache policy");
            }
            auto const units = action == "-h" ? cache::SizeUnits::binary : cache::SizeUnits::decimal;
            std::cout << cache::config_table(units);
            return;
        }
        if (action == "--help" && arguments.empty()) {
            help();
            return;
        }
        if ((action != "set" && action != "reset") || (action == "set" && arguments.empty())) {
            throw std::invalid_argument("Expected config set KEY=VALUE ... or config reset [KEY ...]");
        }
        std::vector<cache::ConfigEdit> edits;
        for (auto const* argument : arguments) {
            auto const text = std::string_view(argument);
            if (action == "reset") {
                edits.push_back({text, std::nullopt});
                continue;
            }
            auto const separator = text.find('=');
            if (separator == text.npos || separator == 0 || separator + 1 == text.size()) {
                throw std::invalid_argument("Expected KEY=VALUE for " + std::string(text));
            }
            edits.push_back({text.substr(0, separator), text.substr(separator + 1)});
        }
        cache::edit_config(edits);
    }
    std::cout << cache::config_table(cache::SizeUnits::bytes);
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")
            || (argc == 3 && std::string_view(argv[1]) == "cache" && std::string_view(argv[2]) == "--help")) {
            help();
            return 0;
        }
        if (argc < 3 || std::string_view(argv[1]) != "cache") {
            throw std::invalid_argument("Expected ivctl cache COMMAND");
        }
        auto const command = std::string_view(argv[2]);
        if (command == "config") {
            configure({argv + 3, static_cast<std::size_t>(argc - 3)});
            return 0;
        }
        auto const root = cache::root();
        if (command == "path" && argc == 3) {
            std::cout << root.string() << '\n';
        } else if (command == "status" && argc == 3) {
            print_report(root, cache::sweep(root, cache::Mode::status), cache::Mode::status);
        } else if (command == "gc" && (argc == 3 || (argc == 4 && std::string_view(argv[3]) == "--dry-run"))) {
            auto const mode = argc == 3 ? cache::Mode::collect : cache::Mode::dry_run;
            print_report(root, cache::sweep(root, mode, log), mode);
        } else {
            throw std::invalid_argument("Invalid cache command. See ivctl cache --help");
        }
        return 0;
    } catch (std::exception const& error) {
        log(error.what());
        return 1;
    }
}
