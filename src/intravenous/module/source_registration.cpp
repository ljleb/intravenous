#include <intravenous/module/source_registration.h>

#include <intravenous/graph/builder.h>
#include <intravenous/node/compiler_record.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <span>
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
    if (!registration.source_root || registration.source_root_size == 0) {
        throw std::invalid_argument("IV source registration has no source package root");
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
        auto const registration_root = normalize_path(std::filesystem::path(std::string(
            registration.source_root, registration.source_root_size)));
        return root == registration_root;
    } catch (...) {
        return false;
    }
}

std::string source_registration_id(SourceRegistrationView const& registration)
{
    return std::string(registration.id, registration.id_size);
}

SourceRegistrationView find_registered_definition(std::string_view id)
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    std::vector<SourceRegistrationView> matches;
    for (auto const& registration : definitions.registrations) {
        if (std::string_view(registration.id, registration.id_size) == id) {
            matches.push_back(registration);
        }
    }
    if (matches.empty()) {
        throw std::runtime_error(
            "registered IV definition '" + std::string(id)
            + "' is unavailable in the current authoring generation");
    }
    if (matches.size() != 1) {
        throw std::runtime_error(
            "registered IV definition '" + std::string(id)
            + "' has multiple providers in the current authoring generation");
    }
    return matches.front();
}

std::string authoring_cycle_message(
    std::span<std::string const> stack, std::string_view requested)
{
    std::ostringstream message;
    message << "registered IV module authoring cycle: ";
    auto const begin = std::ranges::find(stack, requested);
    bool first = true;
    for (auto it = begin; it != stack.end(); ++it) {
        if (!first) message << " -> ";
        message << *it;
        first = false;
    }
    if (!first) message << " -> ";
    message << requested;
    return std::move(message).str();
}

class AuthoringStackEntry {
    std::vector<std::string>& _stack;
public:
    AuthoringStackEntry(std::vector<std::string>& stack, std::string id)
        : _stack(stack)
    {
        _stack.push_back(std::move(id));
    }
    ~AuthoringStackEntry() { _stack.pop_back(); }
};
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
    auto const registration = find_registered_definition(id);
    if (registration.kind == SourceRegistrationKind::node) {
        return registration.node_build(builder);
    }

    static thread_local std::vector<std::string> authoring_stack;
    if (std::ranges::find(authoring_stack, id) != authoring_stack.end()) {
        throw std::runtime_error(authoring_cycle_message(authoring_stack, id));
    }
    AuthoringStackEntry const entry(authoring_stack, source_registration_id(registration));
    GraphBuilder child;
    registration.module_build(child);
    return builder.embed_child(child, "Registered IV module");
}

void clear_source_definitions() noexcept
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    definitions.registrations.clear();
}

void clear_source_definitions_for_root(std::string_view source_root) noexcept
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    std::erase_if(definitions.registrations, [&](SourceRegistrationView const& registration) {
        return belongs_to_source_root(registration, source_root);
    });
}

SourceModuleRegistration::SourceModuleRegistration(
    char const* id,
    std::size_t id_size,
    SourceModuleBuildFunction build,
    char const* source_file,
    std::size_t source_file_size,
    char const* source_root,
    std::size_t source_root_size)
{
    register_source_definition({
        .kind = SourceRegistrationKind::module,
        .id = id,
        .id_size = id_size,
        .source_file = source_file,
        .source_file_size = source_file_size,
        .source_root = source_root,
        .source_root_size = source_root_size,
        .module_build = build,
    });
}

SourceNodeRegistration::SourceNodeRegistration(
    char const* id,
    std::size_t id_size,
    SourceNodeBuildFunction build,
    void const* node_compiler_record,
    char const* source_file,
    std::size_t source_file_size,
    char const* source_root,
    std::size_t source_root_size)
{
    register_source_definition({
        .kind = SourceRegistrationKind::node,
        .id = id,
        .id_size = id_size,
        .source_file = source_file,
        .source_file_size = source_file_size,
        .source_root = source_root,
        .source_root_size = source_root_size,
        .node_build = build,
        .node_compiler_record = node_compiler_record,
    });
}
} // namespace iv::details
