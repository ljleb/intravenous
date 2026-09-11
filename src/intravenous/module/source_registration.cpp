#include <intravenous/module/source_registration.h>

#include <intravenous/graph/builder.h>
#include <intravenous/node/compiler_record.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv::details {
namespace {
struct SourceDefinitions {
    std::mutex mutex;
    std::vector<SourceRegistrationView> registrations;
};

SourceDefinitions& source_definitions()
{
    static SourceDefinitions definitions;
    return definitions;
}

void validate(SourceRegistrationView const& registration)
{
    if (!registration.id || registration.id_size == 0) {
        throw std::invalid_argument("IV source registration has an empty stable ID");
    }
    if (std::string_view(registration.id, registration.id_size).find('\0')
        != std::string_view::npos) {
        throw std::invalid_argument("IV source registration ID contains a null byte");
    }
    if (!registration.source_file || registration.source_file_size == 0) {
        throw std::invalid_argument("IV source registration has no source file");
    }
    if (registration.kind == SourceRegistrationKind::module
        && !registration.module_build) {
        throw std::invalid_argument("IV module registration has no build function");
    }
    if (registration.kind == SourceRegistrationKind::node
        && (!registration.node_build || !registration.node_compiler_record)) {
        throw std::invalid_argument("IV node registration is incomplete");
    }
}

std::filesystem::path normalize_path(std::filesystem::path const& path)
{
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error) return canonical.lexically_normal();
    auto absolute = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : absolute.lexically_normal();
}

bool belongs_to_source_root(
    SourceRegistrationView const& registration,
    std::string_view source_root) noexcept
{
    try {
        auto const root = normalize_path(std::filesystem::path(std::string(source_root)));
        auto const source_file = normalize_path(std::filesystem::path(std::string(
            registration.source_file, registration.source_file_size)));
        auto root_part = root.begin();
        auto source_part = source_file.begin();
        for (; root_part != root.end(); ++root_part, ++source_part) {
            if (source_part == source_file.end() || *source_part != *root_part) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace

void register_source_definition(SourceRegistrationView registration)
{
    validate(registration);
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    auto const id = std::string_view(registration.id, registration.id_size);
    auto const duplicate = std::find_if(
        definitions.registrations.begin(), definitions.registrations.end(),
        [&](SourceRegistrationView const& existing) {
            return std::string_view(existing.id, existing.id_size) == id
                && std::string_view(existing.source_file, existing.source_file_size)
                    == std::string_view(registration.source_file, registration.source_file_size);
        });
    if (duplicate != definitions.registrations.end()) {
        throw std::logic_error(
            "duplicate stable IV source registration in one translation unit: "
            + std::string(id));
    }
    definitions.registrations.push_back(registration);
}

std::size_t source_definition_count() noexcept
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    return definitions.registrations.size();
}

SourceRegistrationView source_definition_at(std::size_t index)
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    if (index >= definitions.registrations.size()) {
        throw std::out_of_range("IV source registration index is out of range");
    }
    return definitions.registrations[index];
}

std::size_t source_module_count(std::string_view source_root) noexcept
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    return static_cast<std::size_t>(std::count_if(
        definitions.registrations.begin(), definitions.registrations.end(),
        [&](SourceRegistrationView const& registration) {
            return registration.kind == SourceRegistrationKind::module
                && belongs_to_source_root(registration, source_root);
        }));
}

SourceRegistrationView source_module_at(std::string_view source_root, std::size_t index)
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    for (auto const& registration : definitions.registrations) {
        if (registration.kind != SourceRegistrationKind::module
            || !belongs_to_source_root(registration, source_root)) continue;
        if (index == 0) return registration;
        --index;
    }
    throw std::out_of_range("IV source module registration index is out of range");
}

std::size_t source_node_count(std::string_view source_root) noexcept
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    return static_cast<std::size_t>(std::count_if(
        definitions.registrations.begin(), definitions.registrations.end(),
        [&](SourceRegistrationView const& registration) {
            return registration.kind == SourceRegistrationKind::node
                && belongs_to_source_root(registration, source_root);
        }));
}

SourceRegistrationView source_node_at(std::string_view source_root, std::size_t index)
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    for (auto const& registration : definitions.registrations) {
        if (registration.kind != SourceRegistrationKind::node
            || !belongs_to_source_root(registration, source_root)) continue;
        if (index == 0) return registration;
        --index;
    }
    throw std::out_of_range("IV source node registration index is out of range");
}

NodeCodeKey source_node_code_key(std::string_view source_root, std::size_t index)
{
    auto const registration = source_node_at(source_root, index);
    auto const* record = static_cast<NodeCompilerRecord const*>(
        registration.node_compiler_record);
    if (!record) {
        throw std::logic_error("IV source node registration has no compiler record");
    }
    return record->code_key;
}

NodeRef author_registered_source_definition(GraphBuilder& builder, std::string_view id)
{
    // Do not execute a provider builder here.  An ID reference is preserved in
    // the authored graph and resolved against source candidates only after the
    // providers have independently loaded.  This keeps source implementation
    // code out of consumers and makes a primitive/module replacement private.
    return builder.registered_node(id);
}

void clear_source_definitions() noexcept
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    definitions.registrations.clear();
}

SourceModuleRegistration::SourceModuleRegistration(
    char const* id,
    std::size_t id_size,
    SourceModuleBuildFunction build,
    char const* source_file,
    std::size_t source_file_size)
{
    register_source_definition({
        .kind = SourceRegistrationKind::module,
        .id = id,
        .id_size = id_size,
        .source_file = source_file,
        .source_file_size = source_file_size,
        .module_build = build,
    });
}

SourceNodeRegistration::SourceNodeRegistration(
    char const* id,
    std::size_t id_size,
    SourceNodeBuildFunction build,
    void const* node_compiler_record,
    char const* source_file,
    std::size_t source_file_size)
{
    register_source_definition({
        .kind = SourceRegistrationKind::node,
        .id = id,
        .id_size = id_size,
        .source_file = source_file,
        .source_file_size = source_file_size,
        .node_build = build,
        .node_compiler_record = node_compiler_record,
    });
}
} // namespace iv::details
