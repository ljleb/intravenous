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
struct BuilderSource {
    std::string source_root{};
    std::vector<SourceRegistrationView> registrations{};
    std::vector<NodeConfigPointerFieldData> config_pointer_fields{};
    std::vector<RetainedGlobalData> retained_globals{};
};

struct BuilderConfiguration {
    std::vector<BuilderSource> sources{};
    std::vector<std::string> module_stack{};
};

void validate_registration(SourceRegistrationView const& registration)
{
    if (!registration.id || registration.id_size == 0) {
        throw std::invalid_argument("IV source registration has an empty stable ID");
    }
    auto const id = std::string_view(registration.id, registration.id_size);
    if (id.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("IV source registration ID contains a null byte");
    }
    if (!registration.source_file || registration.source_file_size == 0) {
        throw std::invalid_argument("IV source registration has no source file");
    }
    if (!registration.source_root || registration.source_root_size == 0) {
        throw std::invalid_argument("IV source registration has no source package root");
    }
    if (registration.kind == SourceRegistrationKind::module) {
        if (!registration.module_build || registration.node_build
            || registration.node_compiler_record) {
            throw std::invalid_argument("IV module registration is invalid");
        }
    } else if (registration.kind == SourceRegistrationKind::node) {
        if (registration.module_build || !registration.node_build
            || !registration.node_compiler_record) {
            throw std::invalid_argument("IV node registration is invalid");
        }
    } else {
        throw std::invalid_argument("IV source registration has invalid kind");
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
    std::size_t source_index = static_cast<std::size_t>(-1);

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
    BuilderSession* parent, std::size_t source_index)
{
    if (!parent || !parent->configuration) {
        throw std::invalid_argument("parent builder session is null");
    }
    if (source_index >= parent->configuration->sources.size()) {
        throw std::out_of_range("builder source index is out of range");
    }
    auto child = std::make_unique<BuilderSession>();
    child->configuration = parent->configuration;
    child->source_index = source_index;
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

void set_builder_sources(
    BuilderSession* session, std::span<BuilderSourceView const> sources)
{
    if (!session) throw std::invalid_argument("builder session is null");
    if (session->graph_taken) {
        throw std::logic_error("cannot configure a finished builder session");
    }

    auto configured = std::make_shared<BuilderConfiguration>();
    configured->sources.reserve(sources.size());
    for (auto const& source : sources) {
        if (source.source_root.empty()) {
            throw std::invalid_argument("builder source has empty source root");
        }
        auto& destination = configured->sources.emplace_back();
        destination.source_root = source.source_root;
        destination.registrations.assign(
            source.registrations.begin(), source.registrations.end());
        destination.config_pointer_fields.assign(
            source.config_pointer_fields.begin(), source.config_pointer_fields.end());
        destination.retained_globals.assign(
            source.retained_globals.begin(), source.retained_globals.end());

        for (auto const& registration : destination.registrations) {
            validate_registration(registration);
            auto const root = std::string_view(
                registration.source_root, registration.source_root_size);
            if (root != destination.source_root) {
                throw std::invalid_argument(
                    "IV source registration belongs to a different source root");
            }
        }
        for (auto const& global : destination.retained_globals) {
            if (!global.address || global.size == 0) {
                throw std::invalid_argument("retained LLVM global has invalid native address");
            }
        }
    }
    session->configuration = std::move(configured);
    session->source_index = static_cast<std::size_t>(-1);
}

std::size_t builder_source_index(
    BuilderSession const* session, std::string_view source_root)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    auto const source = std::ranges::find(
        session->configuration->sources, source_root, &BuilderSource::source_root);
    if (source == session->configuration->sources.end()) {
        throw std::runtime_error(
            "IV source '" + std::string(source_root)
            + "' is unavailable while configuring the graph");
    }
    return static_cast<std::size_t>(source - session->configuration->sources.begin());
}

std::size_t builder_selected_source(BuilderSession const* session) noexcept
{
    return session ? session->source_index : static_cast<std::size_t>(-1);
}

void restore_builder_source(BuilderSession* session, std::size_t source_index) noexcept
{
    if (session) session->source_index = source_index;
}

void select_builder_source(BuilderSession* session, std::size_t source_index)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    if (source_index >= session->configuration->sources.size()) {
        throw std::out_of_range("builder source index is out of range");
    }
    session->source_index = source_index;
}

BuilderRegistration find_builder_registration(
    BuilderSession const* session, std::string_view id)
{
    if (!session || !session->configuration) {
        throw std::invalid_argument("builder session is null");
    }
    std::optional<BuilderRegistration> found;
    for (std::size_t source_index = 0;
         source_index < session->configuration->sources.size(); ++source_index) {
        for (auto const& registration
             : session->configuration->sources[source_index].registrations) {
            if (std::string_view(registration.id, registration.id_size) != id) continue;
            if (found) {
                throw std::runtime_error(
                    "registered IV definition '" + std::string(id)
                    + "' has multiple providers in the loaded IV sources");
            }
            found = BuilderRegistration{
                .registration = registration,
                .source_index = source_index,
            };
        }
    }
    if (found) return *found;
    throw std::runtime_error(
        "registered IV definition '" + std::string(id)
        + "' is unavailable in the loaded IV sources");
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
        || session->source_index >= session->configuration->sources.size()) {
        // Nodes created directly by host code do not have source compiler
        // metadata, and therefore have no source-relative pointer relocations.
        return {};
    }
    auto const& source = session->configuration->sources[session->source_index];

    NodeConfigRelocations relocations;
    auto const* bytes = static_cast<std::byte const*>(config);
    for (auto const& field : source.config_pointer_fields) {
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
            source.retained_globals.begin(), source.retained_globals.end(),
            [&](RetainedGlobalData const& candidate) {
                auto const begin = reinterpret_cast<std::uintptr_t>(candidate.address);
                return value_address >= begin && value_address - begin < candidate.size;
            });
        if (global == source.retained_globals.end()) {
            throw std::logic_error(
                "node configuration pointer does not refer to a retained LLVM global");
        }
        relocations.push_back({
            .byte_offset = offset,
            .source_root = source.source_root,
            .retained_global_ordinal = global->ordinal,
            .addend = static_cast<std::size_t>(
                value_address - reinterpret_cast<std::uintptr_t>(global->address)),
        });
    }
    std::ranges::sort(relocations, {}, &NodeConfigRelocation::byte_offset);
    return relocations;
}

} // namespace iv::details
