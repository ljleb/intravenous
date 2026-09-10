#include <intravenous/module/source_registration.h>

#include <intravenous/graph/builder.h>

#include <algorithm>
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
    if (registration.kind == SourceRegistrationKind::module
        && !registration.module_build) {
        throw std::invalid_argument("IV module registration has no build function");
    }
    if (registration.kind == SourceRegistrationKind::node
        && (!registration.node_build || !registration.node_compiler_record)) {
        throw std::invalid_argument("IV node registration is incomplete");
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
            return std::string_view(existing.id, existing.id_size) == id;
        });
    if (duplicate != definitions.registrations.end()) {
        throw std::logic_error("duplicate stable IV source registration: " + std::string(id));
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

std::size_t source_module_count() noexcept
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    return static_cast<std::size_t>(std::count_if(
        definitions.registrations.begin(), definitions.registrations.end(),
        [](SourceRegistrationView const& registration) {
            return registration.kind == SourceRegistrationKind::module;
        }));
}

SourceRegistrationView source_module_at(std::size_t index)
{
    auto& definitions = source_definitions();
    std::scoped_lock lock(definitions.mutex);
    for (auto const& registration : definitions.registrations) {
        if (registration.kind != SourceRegistrationKind::module) continue;
        if (index == 0) return registration;
        --index;
    }
    throw std::out_of_range("IV source module registration index is out of range");
}

NodeRef author_registered_source_node(GraphBuilder& builder, std::string_view id)
{
    SourceNodeBuildFunction build = nullptr;
    {
        auto& definitions = source_definitions();
        std::scoped_lock lock(definitions.mutex);
        auto const found = std::find_if(
            definitions.registrations.begin(), definitions.registrations.end(),
            [&](SourceRegistrationView const& registration) {
                return registration.kind == SourceRegistrationKind::node
                    && std::string_view(registration.id, registration.id_size) == id;
            });
        if (found == definitions.registrations.end()) {
            throw std::out_of_range(
                "no registered IV node interface named '" + std::string(id) + "'");
        }
        build = found->node_build;
    }
    return build(builder);
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
    SourceModuleBuildFunction build)
{
    register_source_definition({
        .kind = SourceRegistrationKind::module,
        .id = id,
        .id_size = id_size,
        .module_build = build,
    });
}

SourceNodeRegistration::SourceNodeRegistration(
    char const* id,
    std::size_t id_size,
    SourceNodeBuildFunction build,
    void const* node_compiler_record)
{
    register_source_definition({
        .kind = SourceRegistrationKind::node,
        .id = id,
        .id_size = id_size,
        .node_build = build,
        .node_compiler_record = node_compiler_record,
    });
}
} // namespace iv::details
