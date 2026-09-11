#include <intravenous/module/builder_session.h>

#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/state.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv::details {
namespace {
struct BuilderPackage {
    std::string package_root{};
    std::vector<PackageDefinition> definitions{};
    std::vector<NodeConfigPointerFieldData> config_pointer_fields{};
    std::vector<RetainedGlobalData> retained_globals{};
    std::vector<BuilderNodeStateStructure> node_state_structures{};
};

struct BuilderConfiguration {
    std::vector<BuilderPackage> packages{};
    std::vector<std::string> module_stack{};
    std::vector<bool> used_packages{};
};

void validate_definition(PackageDefinition const& definition)
{
    if (!definition.id || definition.id_size == 0) {
        throw std::invalid_argument("IV package definition has an empty stable ID");
    }
    auto const id = std::string_view(definition.id, definition.id_size);
    if (id.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("IV package definition ID contains a null byte");
    }
    if (!definition.source_file || definition.source_file_size == 0) {
        throw std::invalid_argument("IV package definition has no source file");
    }
    if (!definition.package_root || definition.package_root_size == 0) {
        throw std::invalid_argument("IV package definition has no package root");
    }
    if (definition.kind == PackageDefinitionKind::module) {
        if (!definition.module_build || definition.node_build
            || definition.node_compiler_record) {
            throw std::invalid_argument("IV module definition is invalid");
        }
    } else if (definition.kind == PackageDefinitionKind::node) {
        if (definition.module_build || !definition.node_build
            || !definition.node_compiler_record) {
            throw std::invalid_argument("IV node definition is invalid");
        }
    } else {
        throw std::invalid_argument("IV package definition has invalid kind");
    }
}

std::string module_cycle_message(
    std::span<std::string const> stack, std::string_view requested)
{
    std::ostringstream message;
    message << "iv module configuration cycle: ";
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
}

struct BuilderSession {
    std::unique_ptr<GraphBuilderState> state = std::make_unique<GraphBuilderState>();
    bool graph_taken = false;
    std::shared_ptr<BuilderConfiguration> configuration =
        std::make_shared<BuilderConfiguration>();
    std::size_t package_index = static_cast<std::size_t>(-1);

    struct NodeConfigAllocation {
        void* storage = nullptr;
        std::size_t size = 0;
        std::size_t alignment = 1;

        NodeConfigAllocation() = default;
        NodeConfigAllocation(void* storage_, std::size_t size_, std::size_t alignment_)
            : storage(storage_), size(size_), alignment(alignment_) {}
        NodeConfigAllocation(NodeConfigAllocation const&) = delete;
        NodeConfigAllocation& operator=(NodeConfigAllocation const&) = delete;
        NodeConfigAllocation(NodeConfigAllocation&& other) noexcept
            : storage(std::exchange(other.storage, nullptr))
            , size(other.size)
            , alignment(other.alignment) {}
        NodeConfigAllocation& operator=(NodeConfigAllocation&& other) noexcept
        {
            if (this == &other) return *this;
            reset();
            storage = std::exchange(other.storage, nullptr);
            size = other.size;
            alignment = other.alignment;
            return *this;
        }
        ~NodeConfigAllocation() { reset(); }
        void reset() noexcept
        {
            if (storage) {
                ::operator delete(storage, std::align_val_t{alignment});
                storage = nullptr;
            }
        }
    };
    std::vector<NodeConfigAllocation> pending_node_configs{};
};

extern "C" BuilderSession* iv_builder_session_create()
{
    return new BuilderSession;
}

extern "C" void iv_builder_session_destroy(BuilderSession* session) noexcept
{
    delete session;
}

BuilderSession* iv_builder_child_session_create(
    BuilderSession* parent, std::size_t package_index)
{
    if (!parent || !parent->configuration) {
        throw std::invalid_argument("parent builder session is null");
    }
    if (package_index >= parent->configuration->packages.size()) {
        throw std::out_of_range("builder package index is out of range");
    }
    auto child = std::make_unique<BuilderSession>();
    child->configuration = parent->configuration;
    child->package_index = package_index;
    // The configured graph now contains code/data produced by this package even
    // when the nested iv module contributes no primitive node directly. Pin it.
    child->configuration->used_packages[package_index] = true;
    return child.release();
}

GraphBuilderState& builder_graph_state(GraphBuilder& builder)
{
    if (!builder._session || !builder._session->state
        || builder._session->graph_taken) {
        throw std::logic_error("builder graph is unavailable");
    }
    auto& state = *builder._session->state;
    state.bind(builder);
    return state;
}

ConfiguredGraph take_built_graph(BuilderSession* session)
{
    if (!session || !session->state || session->graph_taken) {
        throw std::logic_error("builder session has no unfinished graph");
    }
    session->graph_taken = true;
    auto state = std::move(session->state);
    return std::move(*state).finish();
}

void set_builder_packages(
    BuilderSession* session, std::span<BuilderPackageView const> packages)
{
    if (!session) throw std::invalid_argument("builder session is null");
    if (session->graph_taken) {
        throw std::logic_error("cannot configure a finished builder session");
    }

    auto configured = std::make_shared<BuilderConfiguration>();
    configured->packages.reserve(packages.size());
    configured->used_packages.assign(packages.size(), false);
    for (auto const& package : packages) {
        if (package.package_root.empty()) {
            throw std::invalid_argument("builder package has empty package root");
        }
        auto& destination = configured->packages.emplace_back();
        destination.package_root = package.package_root;
        destination.definitions.assign(
            package.definitions.begin(), package.definitions.end());
        destination.config_pointer_fields.assign(
            package.config_pointer_fields.begin(), package.config_pointer_fields.end());
        destination.retained_globals.assign(
            package.retained_globals.begin(), package.retained_globals.end());
        destination.node_state_structures.assign(
            package.node_state_structures.begin(), package.node_state_structures.end());

        for (auto const& definition : destination.definitions) {
            validate_definition(definition);
            auto const root = std::string_view(
                definition.package_root, definition.package_root_size);
            if (root != destination.package_root) {
                throw std::invalid_argument(
                    "IV package definition belongs to a different package root");
            }
        }
        for (auto const& global : destination.retained_globals) {
            if (!global.address || global.size == 0) {
                throw std::invalid_argument("retained LLVM global has invalid native address");
            }
        }
    }
    session->configuration = std::move(configured);
    session->package_index = static_cast<std::size_t>(-1);
}

std::size_t builder_package_index(
    BuilderSession const* session, std::string_view package_root)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    auto const source = std::ranges::find(
        session->configuration->packages, package_root, &BuilderPackage::package_root);
    if (source == session->configuration->packages.end()) {
        throw std::runtime_error(
            "IV package '" + std::string(package_root)
            + "' is unavailable while configuring the graph");
    }
    return static_cast<std::size_t>(source - session->configuration->packages.begin());
}

std::size_t builder_selected_package(BuilderSession const* session) noexcept
{
    return session ? session->package_index : static_cast<std::size_t>(-1);
}

void restore_builder_package(BuilderSession* session, std::size_t package_index) noexcept
{
    if (session) session->package_index = package_index;
}

void select_builder_package(BuilderSession* session, std::size_t package_index)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    if (package_index >= session->configuration->packages.size()) {
        throw std::out_of_range("builder package index is out of range");
    }
    session->package_index = package_index;
    session->configuration->used_packages[package_index] = true;
}

BuilderDefinition find_builder_definition(
    BuilderSession const* session, std::string_view id)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    std::optional<BuilderDefinition> found;
    for (std::size_t package_index = 0;
         package_index < session->configuration->packages.size(); ++package_index) {
        for (auto const& definition
             : session->configuration->packages[package_index].definitions) {
            if (std::string_view(definition.id, definition.id_size) != id) continue;
            if (found) {
                throw std::runtime_error(
                    "IV package definition '" + std::string(id)
                    + "' has multiple providers in the loaded package definitions");
            }
            found = BuilderDefinition{
                .definition = definition,
                .package_index = package_index,
            };
        }
    }
    if (found) return *found;
    throw std::runtime_error(
        "IV package definition '" + std::string(id)
        + "' is unavailable in the loaded package definitions");
}

std::vector<std::size_t> builder_used_packages(BuilderSession const* session)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < session->configuration->used_packages.size(); ++index) {
        if (session->configuration->used_packages[index]) result.push_back(index);
    }
    return result;
}

void begin_builder_module(BuilderSession* session, std::string_view id)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    auto& stack = session->configuration->module_stack;
    if (std::ranges::contains(stack, id)) {
        throw std::runtime_error(module_cycle_message(stack, id));
    }
    stack.emplace_back(id);
}

void end_builder_module(BuilderSession* session) noexcept
{
    if (!session || !session->configuration) return;
    auto& stack = session->configuration->module_stack;
    if (!stack.empty()) stack.pop_back();
}

void* iv_builder_allocate_node_config(
    BuilderSession* session, std::size_t size, std::size_t alignment)
{
    if (!session || session->graph_taken || size == 0 || alignment == 0) {
        throw std::invalid_argument("invalid builder node configuration allocation");
    }
    auto* storage = ::operator new(size, std::align_val_t{alignment});
    session->pending_node_configs.emplace_back(storage, size, alignment);
    return storage;
}

void iv_builder_discard_node_config(BuilderSession* session, void* storage) noexcept
{
    if (!session || !storage) return;
    auto const allocation = std::find_if(
        session->pending_node_configs.begin(), session->pending_node_configs.end(),
        [&](auto const& candidate) { return candidate.storage == storage; });
    if (allocation != session->pending_node_configs.end()) {
        session->pending_node_configs.erase(allocation);
    }
}

std::shared_ptr<void const> take_builder_node_config(
    BuilderSession* session,
    void const* storage,
    std::size_t size,
    std::size_t alignment)
{
    if (!session || !storage || size == 0 || alignment == 0) {
        throw std::invalid_argument("invalid builder-owned node configuration");
    }
    auto const allocation = std::find_if(
        session->pending_node_configs.begin(), session->pending_node_configs.end(),
        [&](auto const& candidate) {
            return candidate.storage == storage && candidate.size == size
                && candidate.alignment == alignment;
        });
    if (allocation == session->pending_node_configs.end()) {
        throw std::logic_error("node configuration was not allocated by this builder session");
    }
    auto* owned = allocation->storage;
    allocation->storage = nullptr;
    session->pending_node_configs.erase(allocation);
    return std::shared_ptr<void const>(
        owned,
        [alignment](void const* pointer) {
            ::operator delete(const_cast<void*>(pointer), std::align_val_t{alignment});
        });
}

NodeConfigRelocations capture_node_config(
    BuilderSession* session,
    NodeCodeKey code_key,
    void const* config,
    std::size_t config_size)
{
    if (!session || !config || config_size == 0) {
        throw std::logic_error("node configuration has no builder-owned storage");
    }
    if (!session->configuration
        || session->package_index >= session->configuration->packages.size()) {
        // Nodes created directly by host code do not have package compiler
        // metadata, and therefore have no source-relative pointer relocations.
        return {};
    }
    auto const& package = session->configuration->packages[session->package_index];

    NodeConfigRelocations relocations;
    auto const* bytes = static_cast<std::byte const*>(config);
    for (auto const& field : package.config_pointer_fields) {
        if (field.code_key != code_key) continue;
        auto const offset = field.byte_offset;
        if (offset > config_size || config_size - offset < sizeof(void const*)) {
            throw std::logic_error(
                "compiler pointer field metadata is outside node configuration");
        }
        void const* value = nullptr;
        std::memcpy(&value, bytes + offset, sizeof(value));
        if (!value) {
            relocations.push_back({.byte_offset = offset});
            continue;
        }
        auto const value_address = reinterpret_cast<std::uintptr_t>(value);
        auto const global = std::find_if(
            package.retained_globals.begin(), package.retained_globals.end(),
            [&](RetainedGlobalData const& candidate) {
                auto const begin = reinterpret_cast<std::uintptr_t>(candidate.address);
                return value_address >= begin && value_address - begin < candidate.size;
            });
        if (global == package.retained_globals.end()) {
            throw std::logic_error(
                "node configuration pointer does not refer to a retained LLVM global");
        }
        relocations.push_back({
            .byte_offset = offset,
            .package_root = package.package_root,
            .retained_global_ordinal = global->ordinal,
            .addend = static_cast<std::size_t>(
                value_address - reinterpret_cast<std::uintptr_t>(global->address)),
        });
    }
    std::ranges::sort(relocations, {}, &NodeConfigRelocation::byte_offset);
    return relocations;
}

std::shared_ptr<NodeStateStructure const> copy_builder_node_state_structure(
    BuilderSession* session, NodeCodeKey code_key)
{
    if (!session || !session->configuration
        || session->package_index >= session->configuration->packages.size()) {
        return {};
    }
    auto const& package = session->configuration->packages[session->package_index];
    auto const found = std::ranges::find(
        package.node_state_structures, code_key, &BuilderNodeStateStructure::code_key);
    if (found == package.node_state_structures.end()) return {};
    return std::make_shared<NodeStateStructure const>(found->structure);
}

} // namespace iv::details
