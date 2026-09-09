#include <intravenous/node/layout.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <new>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace iv {
    NodeLayoutBuilder::NodeLayoutBuilder(size_t max_block_size)
        : _max_block_size(max_block_size)
    {}

    NodeLayoutBuilder::NodeLayoutBuilder(
        size_t max_block_size, size_t default_silence_ttl_samples)
        : _max_block_size(max_block_size)
        , _default_silence_ttl_samples(default_silence_ttl_samples)
    {}

    NodeLayoutBuilder::NodeLayoutBuilder(
        size_t max_block_size,
        size_t default_silence_ttl_samples,
        size_t event_port_buffer_base_multiplier)
        : _max_block_size(max_block_size)
        , _default_silence_ttl_samples(default_silence_ttl_samples)
        , _event_port_buffer_base_multiplier(event_port_buffer_base_multiplier)
    {}

    size_t NodeLayoutBuilder::max_block_size() const
    {
        return _max_block_size;
    }

    size_t NodeLayoutBuilder::default_silence_ttl_samples() const
    {
        return _default_silence_ttl_samples;
    }

    size_t NodeLayoutBuilder::event_port_buffer_base_multiplier() const
    {
        return _event_port_buffer_base_multiplier;
    }

    void NodeLayoutBuilder::log_node_event(
        char const* event, NodeLayout::NodeRecord const& record, size_t node_index)
    {
        (void)event;
        (void)record;
        (void)node_index;
    }

    void NodeLayoutBuilder::override_node_state_structure(
        size_t node_index, NodeStateStructure structure)
    {
        if (node_index >= _nodes.size()) {
            throw std::out_of_range("node state structure index out of range");
        }
        _nodes[node_index].node_state_structure = std::move(structure);
    }

    size_t NodeLayoutBuilder::align_up(size_t value, size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    namespace details {
        size_t register_node(
            NodeLayoutBuilder& builder,
            NodeLayoutNodeRegistration const& registration)
        {
            size_t const node_index = builder._nodes.size();

            NodeLayout::NodeRecord record;
            record.node = registration.node;
            record.node_type = registration.node_type;
            record.node_type_name = registration.node_type_name;
            record.lifecycle = registration.lifecycle;
            if (registration.has_state) {
                record.node_state_structure = NodeStateStructure {
                    .size_bits = registration.state_size * 8,
                    .alignment_bits = registration.state_alignment * 8,
                };
            } else {
                NodeLayout::Region region;
                region.kind = NodeLayout::Region::Kind::state;
                region.owner_node = node_index;
                region.size = 0;
                region.alignment = 1;
                builder._regions.push_back(region);
            }
            builder._nodes.push_back(std::move(record));
            return node_index;
        }

        void allocate_node_state(
            NodeLayoutBuilder& builder,
            size_t node_index,
            size_t size,
            size_t alignment)
        {
            if (node_index >= builder._nodes.size()) {
                throw std::out_of_range("node state allocation index out of range");
            }

            auto& node = builder._nodes[node_index];
            if (node.state_size != 0) {
                return;
            }

            builder._storage_alignment =
                std::max(builder._storage_alignment, alignment);
            node.state_size = size;
            node.state_alignment = alignment;

            NodeLayout::Region region;
            region.kind = NodeLayout::Region::Kind::state;
            region.owner_node = node_index;
            region.size = size;
            region.alignment = alignment;
            builder._regions.push_back(region);
        }

        void declare_local_array(
            NodeLayoutBuilder& builder,
            NodeLayoutArrayDeclaration const& declaration)
        {
            NodeLayout::Region region;
            region.kind = NodeLayout::Region::Kind::local_array;
            region.owner_node = declaration.owner_node;
            region.state_field_offset = declaration.state_field_offset;
            region.size = declaration.element_size * declaration.element_count;
            region.alignment = declaration.element_alignment;
            region.element_count = declaration.element_count;
            region.element_type = declaration.element_type;
            region.element_type_name = declaration.element_type_name;
            region.assign_span_fn = declaration.assign_span_fn;

            builder._storage_alignment = std::max(
                builder._storage_alignment, declaration.element_alignment);
            builder._regions.push_back(region);
        }

        size_t declare_nested_node_states(
            NodeLayoutBuilder& builder,
            size_t node_index,
            ptrdiff_t state_field_offset)
        {
            NodeLayout::Region region;
            region.kind = NodeLayout::Region::Kind::nested_node_states;
            region.owner_node = node_index;
            region.state_field_offset = state_field_offset;
            region.size = 0;
            region.alignment = alignof(std::span<std::byte>);
            region.element_count = 0;
            region.element_type = node_layout_type_token<std::span<std::byte>>();
            region.element_type_name = typeid(std::span<std::byte>).name();
            region.assign_span_fn = [](
                void* state_base,
                ptrdiff_t field_offset,
                void* data,
                size_t count) {
                auto& span_ref = *reinterpret_cast<std::span<std::span<std::byte>>*>(
                    static_cast<std::byte*>(state_base) + field_offset);
                span_ref = {
                    static_cast<std::span<std::byte>*>(data),
                    count,
                };
            };

            builder._storage_alignment = std::max(
                builder._storage_alignment, size_t(alignof(std::span<std::byte>)));
            builder._regions.push_back(std::move(region));
            return builder._regions.size() - 1;
        }

        void finalize_nested_node_states(
            NodeLayoutBuilder& builder,
            size_t region_index,
            std::vector<size_t> nested_node_indices)
        {
            IV_ASSERT(
                region_index < builder._regions.size(),
                "nested nodes region index out of bounds");
            auto& region = builder._regions[region_index];
            IV_ASSERT(
                region.kind == NodeLayout::Region::Kind::nested_node_states,
                "region must be a nested nodes region");

            region.size =
                sizeof(std::span<std::byte>) * nested_node_indices.size();
            region.element_count = nested_node_indices.size();
            region.nested_node_indices = std::move(nested_node_indices);
        }

        void declare_export_array(
            NodeLayoutBuilder& builder,
            std::string id,
            NodeLayoutArrayDeclaration const& declaration)
        {
            builder._exports.push_back({
                .owner_node = declaration.owner_node,
                .id = std::move(id),
                .state_field_offset = declaration.state_field_offset,
                .element_type = declaration.element_type,
                .element_size = declaration.element_size,
                .assign_span_fn = declaration.assign_span_fn,
                .read_span_fn = declaration.read_span_fn,
            });
        }

        void declare_import_array(
            NodeLayoutBuilder& builder,
            std::string id,
            NodeLayoutArrayDeclaration const& declaration)
        {
            builder._imports.push_back({
                .owner_node = declaration.owner_node,
                .id = std::move(id),
                .state_field_offset = declaration.state_field_offset,
                .element_type = declaration.element_type,
                .element_size = declaration.element_size,
                .assign_span_fn = declaration.assign_span_fn,
                .read_span_fn = declaration.read_span_fn,
            });
        }

        void require_export_array(
            NodeLayoutBuilder& builder,
            size_t node_index,
            std::string id,
            void const* element_type,
            size_t element_size)
        {
            builder._imports.push_back({
                .owner_node = node_index,
                .id = std::move(id),
                .state_field_offset = 0,
                .element_type = element_type,
                .element_size = element_size,
                .assign_span_fn = nullptr,
                .read_span_fn = nullptr,
            });
        }

        bool has_import_array(
            NodeLayoutBuilder const& builder,
            std::string const& id,
            void const* element_type)
        {
            return std::any_of(
                builder._imports.begin(),
                builder._imports.end(),
                [&](auto const& binding) {
                    return binding.id == id && binding.element_type == element_type;
                });
        }

        bool has_export_array(
            NodeLayoutBuilder const& builder,
            std::string const& id,
            void const* element_type)
        {
            return std::any_of(
                builder._exports.begin(),
                builder._exports.end(),
                [&](auto const& binding) {
                    return binding.id == id && binding.element_type == element_type;
                });
        }

        void override_node_state_structure(
            NodeLayoutBuilder& builder,
            size_t node_index,
            NodeStateStructure const& structure)
        {
            builder.override_node_state_structure(node_index, structure);
        }

        size_t node_layout_max_block_size(NodeLayoutBuilder const& builder)
        {
            return builder._max_block_size;
        }

        size_t node_layout_event_port_buffer_base_multiplier(
            NodeLayoutBuilder const& builder)
        {
            return builder._event_port_buffer_base_multiplier;
        }
    }

    NodeLayout NodeLayoutBuilder::build() &&
    {
        NodeLayout layout;
        layout.storage_alignment = _storage_alignment;
        layout.max_block_size = _max_block_size;
        layout.default_silence_ttl_samples = _default_silence_ttl_samples;
        layout.nodes = std::move(_nodes);
        layout.regions = std::move(_regions);
        layout.imported_arrays = std::move(_imports);
        layout.exported_arrays = std::move(_exports);

        size_t storage_size = 0;
        for (auto& region : layout.regions) {
            storage_size = align_up(storage_size, region.alignment);
            region.storage_offset = storage_size;
            storage_size += region.size;

            if (region.kind == NodeLayout::Region::Kind::state) {
                layout.nodes[region.owner_node].state_offset =
                    static_cast<ptrdiff_t>(region.storage_offset);
            }
        }

        layout.storage_size = storage_size;

        for (auto const& import_binding : layout.imported_arrays) {
            for (auto const& export_binding : layout.exported_arrays) {
                if (
                    import_binding.id == export_binding.id &&
                    import_binding.element_type == export_binding.element_type &&
                    import_binding.owner_node != export_binding.owner_node
                ) {
                    layout.nodes[import_binding.owner_node].dependencies.push_back(
                        export_binding.owner_node);
                }
            }
        }

        for (auto& node : layout.nodes) {
            std::sort(node.dependencies.begin(), node.dependencies.end());
            node.dependencies.erase(
                std::unique(node.dependencies.begin(), node.dependencies.end()),
                node.dependencies.end());
        }

        std::vector<size_t> indegree(layout.nodes.size(), 0);
        std::vector<std::vector<size_t>> outgoing(layout.nodes.size());
        for (size_t node_i = 0; node_i < layout.nodes.size(); ++node_i) {
            for (size_t dep : layout.nodes[node_i].dependencies) {
                ++indegree[node_i];
                outgoing[dep].push_back(node_i);
            }
        }

        std::deque<size_t> ready;
        for (size_t i = 0; i < indegree.size(); ++i) {
            if (indegree[i] == 0) {
                ready.push_back(i);
            }
        }

        while (!ready.empty()) {
            size_t const node_i = ready.front();
            ready.pop_front();
            layout.initialize_order.push_back(node_i);
            for (size_t next : outgoing[node_i]) {
                if (--indegree[next] == 0) {
                    ready.push_back(next);
                }
            }
        }

        for (size_t i = 0; i < layout.nodes.size(); ++i) {
            if (indegree[i] != 0) {
                layout.initialize_order.push_back(i);
            }
        }

        return layout;
    }

    namespace details {
        NodeLayoutArrayStorage resolve_exported_array_storage(
            NodeStorage const& storage,
            std::string const& id,
            void const* element_type)
        {
            if (!storage.layout) {
                return {};
            }

            auto export_it = std::find_if(
                storage.layout->exported_arrays.begin(),
                storage.layout->exported_arrays.end(),
                [&](auto const& export_endpoint) {
                    return export_endpoint.id == id &&
                        export_endpoint.element_type == element_type;
                });
            if (export_it == storage.layout->exported_arrays.end() ||
                !export_it->read_span_fn) {
                return {};
            }

            void* data = nullptr;
            size_t count = 0;
            void* export_state = storage.state_ptr(export_it->owner_node);
            export_it->read_span_fn(
                export_state, export_it->state_field_offset, data, count);
            return {
                .data = data,
                .count = count,
            };
        }

        void* node_storage_state_ptr(
            NodeStorage const& storage, size_t node_index)
        {
            return storage.state_ptr(node_index);
        }

        ResourceContext const& node_storage_resources(NodeStorage const& storage)
        {
            IV_ASSERT(storage.resources, "node storage resources must be present");
            return *storage.resources;
        }

        size_t node_storage_max_block_size(NodeStorage const& storage)
        {
            return storage.max_block_size();
        }

        size_t node_storage_default_silence_ttl_samples(
            NodeStorage const& storage)
        {
            return storage.layout
                ? storage.layout->default_silence_ttl_samples
                : std::numeric_limits<size_t>::max();
        }
    }

    NodeStorage NodeLayout::create_storage(ResourceContext const& resources) const
    {
        return NodeStorage(*this, resources);
    }

    void NodeStorage::StorageDeleter::operator()(std::byte* p) const noexcept
    {
        if (p) {
            ::operator delete(p, std::align_val_t(alignment));
        }
    }

    NodeStorage::NodeStorage()
        : storage(nullptr, StorageDeleter {})
    {}

    NodeStorage::NodeStorage(NodeLayout const& layout_, ResourceContext const& resources_)
    : layout(&layout_)
    , resources(&resources_)
    {
        size_t const storage_alignment = std::max(layout_.storage_alignment, size_t(alignof(std::max_align_t)));
        storage = {
            layout_.storage_size == 0
                ? nullptr
                : static_cast<std::byte*>(::operator new(layout_.storage_size, std::align_val_t(storage_alignment))),
            StorageDeleter { storage_alignment }
        };
        std::fill_n(storage.get(), layout_.storage_size, std::byte {});
    }

    NodeStorage::NodeStorage(NodeStorage&& other) noexcept
    : layout(other.layout)
    , resources(other.resources)
    , storage(std::move(other.storage))
    , constructed_nodes(std::move(other.constructed_nodes))
    , initialized_nodes(std::move(other.initialized_nodes))
    {
        other.layout = nullptr;
        other.resources = nullptr;
        other.constructed_nodes.clear();
        other.initialized_nodes.clear();
    }

    NodeStorage& NodeStorage::operator=(NodeStorage&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        release();
        destroy_constructed_states();
        layout = other.layout;
        resources = other.resources;
        storage = std::move(other.storage);
        constructed_nodes = std::move(other.constructed_nodes);
        initialized_nodes = std::move(other.initialized_nodes);

        other.layout = nullptr;
        other.resources = nullptr;
        other.constructed_nodes.clear();
        other.initialized_nodes.clear();
        return *this;
    }

    NodeStorage::~NodeStorage()
    {
        release();
        destroy_constructed_states();
    }

    void NodeStorage::destroy_constructed_states()
    {
        if (layout) {
            for (auto it = constructed_nodes.rbegin(); it != constructed_nodes.rend(); ++it) {
                auto const& node = layout->nodes[*it];
                if (node.state_size != 0 && node.lifecycle.destroy_state_fn) {
                    NodeLayoutBuilder::log_node_event("destroyed", node, *it);
                    node.lifecycle.destroy_state_fn(state_ptr(*it));
                }
            }
        }
        constructed_nodes.clear();
    }

    std::span<std::byte> NodeStorage::buffer() const
    {
        return { storage.get(), layout ? layout->storage_size : 0 };
    }

    size_t NodeStorage::max_block_size() const
    {
        return layout ? layout->max_block_size : 1;
    }

    void* NodeStorage::state_ptr(size_t node_index) const
    {
        if (!layout || node_index >= layout->nodes.size()) {
            return nullptr;
        }
        auto const& node = layout->nodes[node_index];
        if (node.state_offset < 0) {
            return nullptr;
        }
        return storage.get() + node.state_offset;
    }

    bool NodeStorage::can_move_from(NodeStorage const& previous, size_t node_index, size_t previous_node_index) const
    {
        if (!layout || !previous.layout) {
            return false;
        }
        if (node_index >= layout->nodes.size() || previous_node_index >= previous.layout->nodes.size()) {
            return false;
        }

        auto const& node = layout->nodes[node_index];
        auto const& previous_node = previous.layout->nodes[previous_node_index];
        auto const same_node_name =
            node.node_type_name && previous_node.node_type_name &&
            std::strcmp(
                node.node_type_name, previous_node.node_type_name) == 0;
        auto const same_state_structure =
            node.node_state_structure.has_value() &&
            node.node_state_structure == previous_node.node_state_structure;
        auto const same_node_type = node.node_type == previous_node.node_type ||
            (same_node_name && same_state_structure);
        if (!same_node_type || node.state_size != previous_node.state_size) {
            return false;
        }

        auto next_region = [](NodeLayout const& layout_ref, size_t owner_node, size_t start_index) -> size_t {
            for (size_t i = start_index; i < layout_ref.regions.size(); ++i) {
                if (layout_ref.regions[i].owner_node == owner_node) {
                    return i;
                }
            }
            return layout_ref.regions.size();
        };

        size_t current_index = next_region(*layout, node_index, 0);
        size_t previous_index = next_region(*previous.layout, previous_node_index, 0);

        while (current_index < layout->regions.size() && previous_index < previous.layout->regions.size()) {
            auto const& current_region = layout->regions[current_index];
            auto const& previous_region = previous.layout->regions[previous_index];

            if (current_region.owner_node != node_index || previous_region.owner_node != previous_node_index) {
                break;
            }

            if (
                current_region.kind != previous_region.kind ||
                current_region.state_field_offset != previous_region.state_field_offset ||
                current_region.size != previous_region.size ||
                current_region.alignment != previous_region.alignment ||
                current_region.element_count != previous_region.element_count ||
                (current_region.element_type != previous_region.element_type &&
                 (!current_region.element_type_name ||
                  !previous_region.element_type_name ||
                  std::strcmp(
                      current_region.element_type_name,
                      previous_region.element_type_name) != 0))
            ) {
                return false;
            }

            current_index = next_region(*layout, node_index, current_index + 1);
            previous_index = next_region(*previous.layout, previous_node_index, previous_index + 1);
        }

        return
            (current_index == layout->regions.size() || layout->regions[current_index].owner_node != node_index) &&
            (previous_index == previous.layout->regions.size() || previous.layout->regions[previous_index].owner_node != previous_node_index);
    }

    template<class OwnerIsConstructed>
    void patch_node_storage_regions(
        NodeStorage& storage,
        OwnerIsConstructed&& owner_is_constructed)
    {
        for (auto const& region : storage.layout->regions) {
            if (!owner_is_constructed(region.owner_node) ||
                (region.kind != NodeLayout::Region::Kind::local_array &&
                 region.kind != NodeLayout::Region::Kind::nested_node_states) ||
                !region.assign_span_fn) {
                continue;
            }
            void* state = storage.state_ptr(region.owner_node);
            void* data = storage.storage.get() + region.storage_offset;
            region.assign_span_fn(
                state, region.state_field_offset, data, region.element_count);
            if (region.kind != NodeLayout::Region::Kind::nested_node_states) {
                continue;
            }
            auto const& assigned_span =
                *reinterpret_cast<std::span<std::span<std::byte>> const*>(
                    static_cast<std::byte*>(state) + region.state_field_offset);
            if (assigned_span.size() != region.element_count) {
                throw std::logic_error(
                    "nested node span assignment failed for owner node " +
                    std::to_string(region.owner_node) + " (expected count=" +
                    std::to_string(region.element_count) + ", actual count=" +
                    std::to_string(assigned_span.size()) + ")");
            }
            auto* nested_node_states = static_cast<std::span<std::byte>*>(data);
            for (size_t i = 0; i < region.nested_node_indices.size(); ++i) {
                auto* nested_state = static_cast<std::byte*>(
                    storage.state_ptr(region.nested_node_indices[i]));
                IV_ASSERT(
                    nested_state != nullptr,
                    "nested child state pointer must resolve during storage initialization");
                nested_node_states[i] = {
                    nested_state,
                    static_cast<size_t>(
                        (storage.storage.get() + storage.layout->storage_size) -
                        nested_state),
                };
            }
        }
    }

    void patch_node_storage_imports(NodeStorage& storage)
    {
        for (auto const& import_endpoint : storage.layout->imported_arrays) {
            auto export_it = std::find_if(
                storage.layout->exported_arrays.begin(),
                storage.layout->exported_arrays.end(),
                [&](auto const& export_endpoint) {
                    return export_endpoint.id == import_endpoint.id &&
                        export_endpoint.element_type == import_endpoint.element_type;
                });

            void* data = nullptr;
            size_t count = 0;
            if (export_it != storage.layout->exported_arrays.end() &&
                export_it->read_span_fn) {
                void* export_state = storage.state_ptr(export_it->owner_node);
                export_it->read_span_fn(
                    export_state, export_it->state_field_offset, data, count);
            }
            if (import_endpoint.assign_span_fn) {
                void* import_state = storage.state_ptr(import_endpoint.owner_node);
                import_endpoint.assign_span_fn(
                    import_state,
                    import_endpoint.state_field_offset,
                    data,
                    count);
            }
        }
    }

    NodeStorage::PreparedMigration
    NodeStorage::prepare_migration_from(NodeStorage& previous)
    {
        if (!layout || !resources || !previous.layout || !previous.resources) {
            throw std::logic_error("node storage migration requires two valid storages");
        }
        if (!constructed_nodes.empty() || !initialized_nodes.empty()) {
            throw std::logic_error("node storage migration target must be uninitialized");
        }

        PreparedMigration prepared;
        prepared.current = this;
        prepared.previous = &previous;
        prepared.previous_node_for_current.assign(
            layout->nodes.size(), PreparedMigration::no_node);
        prepared.previous_nodes_consumed.assign(
            previous.layout->nodes.size(), false);
        prepared.deferred_initialize_nodes.reserve(layout->nodes.size());
        prepared.previous_release_nodes.reserve(previous.layout->nodes.size());
        constructed_nodes.reserve(layout->nodes.size());
        initialized_nodes.reserve(layout->nodes.size());

        std::unordered_map<std::string, size_t> previous_by_identity;
        previous_by_identity.reserve(previous.layout->nodes.size());
        for (size_t previous_node = 0;
             previous_node < previous.layout->nodes.size();
             ++previous_node) {
            auto const& record = previous.layout->nodes[previous_node];
            if (!record.lifecycle.identity_fn) continue;
            auto identity = record.lifecycle.identity_fn(record.node);
            if (identity.empty()) continue;
            if (!previous_by_identity.emplace(std::move(identity), previous_node).second)
                throw std::runtime_error(
                    "duplicate node migration identity in previous storage");
        }

        std::unordered_map<std::string, size_t> current_by_identity;
        current_by_identity.reserve(layout->nodes.size());
        std::vector<bool> identity_collision(layout->nodes.size(), false);
        for (size_t node = 0; node < layout->nodes.size(); ++node) {
            auto const& record = layout->nodes[node];
            if (!record.lifecycle.identity_fn) continue;
            auto identity = record.lifecycle.identity_fn(record.node);
            if (identity.empty()) continue;
            if (!current_by_identity.emplace(identity, node).second)
                throw std::runtime_error(
                    "duplicate node migration identity in current storage: " +
                    identity);
            auto const previous_it = previous_by_identity.find(identity);
            if (previous_it == previous_by_identity.end()) continue;
            auto const previous_node = previous_it->second;
            identity_collision[node] = true;
            if (!record.lifecycle.move_fn ||
                !can_move_from(previous, node, previous_node)) {
                continue;
            }
            prepared.previous_node_for_current[node] = previous_node;
            prepared.previous_nodes_consumed[previous_node] = true;
        }

        std::vector<bool> defer_initialize(layout->nodes.size(), false);
        for (size_t node = 0; node < layout->nodes.size(); ++node) {
            defer_initialize[node] =
                prepared.previous_node_for_current[node] != PreparedMigration::no_node ||
                identity_collision[node];
        }
        for (auto const& import : layout->imported_arrays)
            defer_initialize[import.owner_node] = true;

        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t node = 0; node < layout->nodes.size(); ++node) {
                if (defer_initialize[node]) continue;
                for (auto const dependency : layout->nodes[node].dependencies) {
                    if (dependency < defer_initialize.size() &&
                        defer_initialize[dependency]) {
                        defer_initialize[node] = true;
                        changed = true;
                        break;
                    }
                }
            }
        }

        for (size_t node = 0; node < layout->nodes.size(); ++node) {
            auto const& record = layout->nodes[node];
            if (record.state_size == 0) continue;
            if (record.lifecycle.default_construct_state_fn)
                record.lifecycle.default_construct_state_fn(state_ptr(node));
            constructed_nodes.push_back(node);
        }

        patch_node_storage_regions(*this, [](size_t) { return true; });

        for (auto const node : layout->initialize_order) {
            if (prepared.previous_node_for_current[node] !=
                PreparedMigration::no_node) {
                continue;
            }
            if (defer_initialize[node]) {
                prepared.deferred_initialize_nodes.push_back(node);
                continue;
            }
            auto const& record = layout->nodes[node];
            if (record.lifecycle.initialize_fn)
                record.lifecycle.initialize_fn(record.node, node, *this);
            NodeLayoutBuilder::log_node_event("created", record, node);
            initialized_nodes.push_back(node);
        }

        for (auto it = previous.initialized_nodes.rbegin();
             it != previous.initialized_nodes.rend();
             ++it) {
            if (*it >= prepared.previous_nodes_consumed.size() ||
                !prepared.previous_nodes_consumed[*it]) {
                prepared.previous_release_nodes.push_back(*it);
            }
        }
        return prepared;
    }

    void NodeStorage::PreparedMigration::commit()
    {
        if (committed || !current || !previous) {
            throw std::logic_error("invalid or already committed node storage migration");
        }

        patch_node_storage_imports(*current);

        for (auto const node : current->layout->initialize_order) {
            auto const previous_node = previous_node_for_current[node];
            if (previous_node == no_node) continue;
            auto const& record = current->layout->nodes[node];
            record.lifecycle.move_fn(
                record.node, node, previous_node, *current, *previous);
            NodeLayoutBuilder::log_node_event("moved", record, node);
            current->initialized_nodes.push_back(node);
        }

        for (auto const previous_node : previous_release_nodes) {
            auto const& record = previous->layout->nodes[previous_node];
            if (record.lifecycle.release_fn)
                record.lifecycle.release_fn(
                    record.node, previous_node, *previous);
        }
        previous->initialized_nodes.clear();

        for (auto const node : deferred_initialize_nodes) {
            auto const& record = current->layout->nodes[node];
            if (record.lifecycle.initialize_fn)
                record.lifecycle.initialize_fn(record.node, node, *current);
            NodeLayoutBuilder::log_node_event("created", record, node);
            current->initialized_nodes.push_back(node);
        }
        committed = true;
    }

    void NodeStorage::initialize(NodeStorage const* previous)
    {
        if (!layout || !resources) {
            return;
        }

        constexpr size_t no_node = std::numeric_limits<size_t>::max();
        std::vector<size_t> previous_node_for_current(layout->nodes.size(), no_node);
        std::vector<bool> previous_nodes_consumed(
            previous && previous->layout ? previous->layout->nodes.size() : 0,
            false
        );

        if (previous && previous->layout) {
            std::unordered_map<std::string, size_t> previous_by_identity;
            previous_by_identity.reserve(previous->layout->nodes.size());

            for (size_t previous_node_index = 0; previous_node_index < previous->layout->nodes.size(); ++previous_node_index) {
                auto const& previous_record = previous->layout->nodes[previous_node_index];
                if (!previous_record.lifecycle.identity_fn) {
                    continue;
                }

                auto const identity = previous_record.lifecycle.identity_fn(previous_record.node);
                if (identity.empty()) {
                    continue;
                }

                auto [_, inserted] = previous_by_identity.emplace(identity, previous_node_index);
                if (!inserted) {
                    throw std::runtime_error(
                        "duplicate node migration identity in previous storage: " + identity
                    );
                }
            }

            std::unordered_map<std::string, size_t> current_by_identity;
            current_by_identity.reserve(layout->nodes.size());

            for (size_t node_index = 0; node_index < layout->nodes.size(); ++node_index) {
                auto const& record = layout->nodes[node_index];
                if (!record.lifecycle.identity_fn) {
                    continue;
                }

                auto const identity = record.lifecycle.identity_fn(record.node);
                if (identity.empty()) {
                    continue;
                }

                auto [_, inserted] = current_by_identity.emplace(identity, node_index);
                if (!inserted) {
                    throw std::runtime_error(
                        "duplicate node migration identity in current storage: " + identity
                    );
                }

                auto previous_it = previous_by_identity.find(identity);
                if (previous_it == previous_by_identity.end()) {
                    continue;
                }

                size_t const previous_node_index = previous_it->second;
                auto const& previous_record = previous->layout->nodes[previous_node_index];
                if (record.node_type != previous_record.node_type ||
                    !record.lifecycle.move_fn) {
                    continue;
                }
                if (!can_move_from(*previous, node_index, previous_node_index)) {
                    continue;
                }

                previous_node_for_current[node_index] = previous_node_index;
                previous_nodes_consumed[previous_node_index] = true;
            }
        }

        constructed_nodes.clear();
        initialized_nodes.clear();
        for (size_t node_index = 0; node_index < layout->nodes.size(); ++node_index) {
            auto const& record = layout->nodes[node_index];
            if (record.state_size == 0) {
                continue;
            }

            void* state = state_ptr(node_index);
            if (record.lifecycle.default_construct_state_fn) {
                record.lifecycle.default_construct_state_fn(state);
            }

            constructed_nodes.push_back(node_index);
        }

        for (auto const& region : layout->regions) {
            if (
                (region.kind != NodeLayout::Region::Kind::local_array &&
                 region.kind != NodeLayout::Region::Kind::nested_node_states) ||
                !region.assign_span_fn
            ) {
                continue;
            }
            void* state = state_ptr(region.owner_node);
            void* data = storage.get() + region.storage_offset;
            region.assign_span_fn(state, region.state_field_offset, data, region.element_count);
            if (region.kind == NodeLayout::Region::Kind::nested_node_states) {
                auto const& assigned_span = *reinterpret_cast<std::span<std::span<std::byte>> const*>(
                    static_cast<std::byte*>(state) + region.state_field_offset
                );
                if (assigned_span.size() != region.element_count) {
                    throw std::logic_error(
                        "nested node span assignment failed for owner node " + std::to_string(region.owner_node) +
                        " (expected count=" + std::to_string(region.element_count) +
                        ", actual count=" + std::to_string(assigned_span.size()) + ")"
                    );
                }
                auto* nested_node_states = static_cast<std::span<std::byte>*>(data);
                for (size_t i = 0; i < region.nested_node_indices.size(); ++i) {
                    auto* nested_state = static_cast<std::byte*>(state_ptr(region.nested_node_indices[i]));
                    IV_ASSERT(
                        nested_state != nullptr,
                        "nested child state pointer must resolve during storage initialization"
                    );
                    nested_node_states[i] = { nested_state, static_cast<size_t>((storage.get() + layout->storage_size) - nested_state) };
                }
            }
        }

        for (auto const& import_endpoint : layout->imported_arrays) {
            auto export_it = std::find_if(
                layout->exported_arrays.begin(),
                layout->exported_arrays.end(),
                [&](auto const& export_endpoint) {
                    return
                        export_endpoint.id == import_endpoint.id &&
                        export_endpoint.element_type == import_endpoint.element_type;
                }
            );

            void* data = nullptr;
            size_t count = 0;
            if (export_it != layout->exported_arrays.end() && export_it->read_span_fn) {
                void* export_state = state_ptr(export_it->owner_node);
                export_it->read_span_fn(export_state, export_it->state_field_offset, data, count);
            }

            if (import_endpoint.assign_span_fn) {
                void* import_state = state_ptr(import_endpoint.owner_node);
                import_endpoint.assign_span_fn(import_state, import_endpoint.state_field_offset, data, count);
            }
        }

        if (previous && previous->layout) {
            for (size_t node_index : layout->initialize_order) {
                auto const& record = layout->nodes[node_index];
                size_t const previous_node_index = previous_node_for_current[node_index];
                if (
                    previous_node_index == no_node ||
                    !record.lifecycle.move_fn
                ) {
                    continue;
                }

                record.lifecycle.move_fn(
                    record.node,
                    node_index,
                    previous_node_index,
                    *this,
                    *previous
                );
                NodeLayoutBuilder::log_node_event("moved", record, node_index);
                initialized_nodes.push_back(node_index);
            }

            for (size_t previous_node_index = 0; previous_node_index < previous->layout->nodes.size(); ++previous_node_index) {
                if (previous_node_index < previous_nodes_consumed.size() && previous_nodes_consumed[previous_node_index]) {
                    continue;
                }

                auto const& previous_record = previous->layout->nodes[previous_node_index];
                if (previous_record.lifecycle.release_fn) {
                    previous_record.lifecycle.release_fn(
                        previous_record.node,
                        previous_node_index,
                        const_cast<NodeStorage&>(*previous)
                    );
                }
            }
        }

        for (size_t node_index : layout->initialize_order) {
            auto const& record = layout->nodes[node_index];
            if (previous && previous_node_for_current[node_index] != no_node) {
                continue;
            }

            if (record.lifecycle.initialize_fn) {
                try {
                    record.lifecycle.initialize_fn(record.node, node_index, *this);
                } catch (std::exception const& e) {
                    throw std::runtime_error(wrap_exception(
                        "node initialize failed at index " + std::to_string(node_index),
                        e
                    ));
                } catch (...) {
                    throw std::runtime_error(
                        "node initialize failed at index " + std::to_string(node_index)
                    );
                }
            }
            NodeLayoutBuilder::log_node_event("created", record, node_index);
            initialized_nodes.push_back(node_index);
        }

        if (previous) {
            auto& previous_initialized_nodes = const_cast<NodeStorage&>(*previous).initialized_nodes;
            previous_initialized_nodes.clear();
        }
    }

    void NodeStorage::release()
    {
        if (!layout || !resources) {
            return;
        }

        for (auto it = initialized_nodes.rbegin(); it != initialized_nodes.rend(); ++it) {
            size_t const node_index = *it;
            auto const& record = layout->nodes[node_index];
            if (record.lifecycle.release_fn) {
                record.lifecycle.release_fn(record.node, node_index, *this);
            }
        }
        initialized_nodes.clear();
    }

}
