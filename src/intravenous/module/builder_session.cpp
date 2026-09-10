#include <intravenous/module/builder_session.h>

#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/state.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv::details {
struct BuilderSession {
    std::unique_ptr<GraphBuilderState> state =
        std::make_unique<GraphBuilderState>();
    bool graph_taken = false;
    struct ConfigLayout {
        NodeCodeKey node_code_key{};
        std::vector<std::size_t> pointer_offsets{};
    };
    std::vector<ConfigLayout> config_layouts{};
    std::vector<AuthoringGlobalAddress> authoring_globals{};
    struct NodeConfigAllocation {
        void* storage = nullptr;
        std::size_t size = 0;
        std::size_t alignment = 1;

        NodeConfigAllocation() = default;
        NodeConfigAllocation(void* storage_, std::size_t size_, std::size_t alignment_)
            : storage(storage_)
            , size(size_)
            , alignment(alignment_)
        {}
        NodeConfigAllocation(NodeConfigAllocation const&) = delete;
        NodeConfigAllocation& operator=(NodeConfigAllocation const&) = delete;
        NodeConfigAllocation(NodeConfigAllocation&& other) noexcept
            : storage(std::exchange(other.storage, nullptr))
            , size(other.size)
            , alignment(other.alignment)
        {}
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

GraphBuilderState& builder_graph_state(GraphBuilder& builder)
{
    if (!builder._session || !builder._session->state ||
        builder._session->graph_taken) {
        throw std::logic_error("builder graph is unavailable");
    }
    auto& state = *builder._session->state;
    state.bind(builder);
    return state;
}

AuthoredGraph take_built_graph(BuilderSession* session)
{
    if (!session || !session->state || session->graph_taken)
        throw std::logic_error("builder session has no unfinished graph");
    session->graph_taken = true;
    auto state = std::move(session->state);
    return std::move(*state).finish();
}

void set_builder_node_config_layouts(
    BuilderSession* session, std::span<NodeConfigLayout const> layouts)
{
    if (!session) throw std::invalid_argument("builder session is null");
    if (session->graph_taken)
        throw std::logic_error("cannot configure a finished builder session");

    std::vector<BuilderSession::ConfigLayout> configured_layouts;
    configured_layouts.reserve(layouts.size());
    for (auto const& layout : layouts) {
        if (!std::is_sorted(
                layout.pointer_offsets.begin(), layout.pointer_offsets.end())
            || std::adjacent_find(
                   layout.pointer_offsets.begin(), layout.pointer_offsets.end())
                != layout.pointer_offsets.end()) {
            throw std::invalid_argument(
                "node configuration pointer offsets must be sorted and unique");
        }
        if (std::any_of(
                configured_layouts.begin(), configured_layouts.end(),
                [&](auto const& existing) {
                    return existing.node_code_key == layout.node_code_key;
                })) {
            throw std::invalid_argument(
                "node configuration has duplicate compiler layouts");
        }
        auto& destination = configured_layouts.emplace_back();
        destination.node_code_key = layout.node_code_key;
        destination.pointer_offsets.assign(
            layout.pointer_offsets.begin(), layout.pointer_offsets.end());
    }
    session->config_layouts = std::move(configured_layouts);
}

void set_builder_authoring_globals(
    BuilderSession* session,
    std::span<AuthoringGlobalAddress const> globals)
{
    if (!session) throw std::invalid_argument("builder session is null");
    if (session->graph_taken)
        throw std::logic_error("cannot configure a finished builder session");

    std::vector<AuthoringGlobalAddress> configured;
    configured.reserve(globals.size());
    for (auto const& global : globals) {
        if (!global.address || global.size == 0 || !global.symbol) {
            throw std::invalid_argument("authoring global has invalid address metadata");
        }
        configured.push_back(global);
    }
    session->authoring_globals = std::move(configured);
}

void* iv_builder_allocate_node_config(
    BuilderSession* session,
    std::size_t size,
    std::size_t alignment)
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
            ::operator delete(
                const_cast<void*>(pointer), std::align_val_t{alignment});
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
    auto const layout = std::find_if(
        session->config_layouts.begin(), session->config_layouts.end(),
        [&](auto const& candidate) {
            return candidate.node_code_key == code_key;
        });
    if (layout == session->config_layouts.end()) return {};

    NodeConfigRelocations relocations;
    auto const* bytes = static_cast<std::byte const*>(config);
    for (std::size_t offset : layout->pointer_offsets) {
        if (offset > config_size
            || config_size - offset < sizeof(void const*)) {
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
            session->authoring_globals.begin(), session->authoring_globals.end(),
            [&](AuthoringGlobalAddress const& candidate) {
                auto const begin = reinterpret_cast<std::uintptr_t>(candidate.address);
                return value_address >= begin
                    && value_address - begin < candidate.size;
            });
        if (global == session->authoring_globals.end()) {
            throw std::logic_error(
                "node configuration pointer does not refer to a retained authoring global");
        }
        relocations.push_back({
            .byte_offset = offset,
            .target = global->symbol,
            .addend = static_cast<std::size_t>(
                value_address - reinterpret_cast<std::uintptr_t>(global->address)),
        });
    }
    return relocations;
}

} // namespace iv::details
