#include <intravenous/module/builder_session.h>

#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/state.h>

#include <algorithm>
#include <cstring>
#include <memory>
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
        std::vector<std::size_t> c_string_offsets{};
    };
    std::vector<ConfigLayout> config_layouts{};
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
                layout.c_string_offsets.begin(), layout.c_string_offsets.end())
            || std::adjacent_find(
                   layout.c_string_offsets.begin(), layout.c_string_offsets.end())
                != layout.c_string_offsets.end()) {
            throw std::invalid_argument(
                "node configuration C-string offsets must be sorted and unique");
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
        destination.c_string_offsets.assign(
            layout.c_string_offsets.begin(), layout.c_string_offsets.end());
    }
    session->config_layouts = std::move(configured_layouts);
}

NodeConfigStringRelocations capture_node_config(
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

    NodeConfigStringRelocations relocations;
    auto const* bytes = static_cast<std::byte const*>(config);
    for (std::size_t offset : layout->c_string_offsets) {
        if (offset > config_size
            || config_size - offset < sizeof(char const*)) {
            throw std::logic_error(
                "compiler C-string field metadata is outside node configuration");
        }
        char const* value = nullptr;
        std::memcpy(&value, bytes + offset, sizeof(value));
        if (!value) continue;
        relocations.push_back({
            .byte_offset = offset,
            .value = value,
        });
    }
    return relocations;
}

} // namespace iv::details
