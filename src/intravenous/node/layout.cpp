#include <intravenous/node/layout.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <new>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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

    NodeLayout::RegionHandle NodeLayoutBuilder::declare_raw_region(
        size_t size,
        size_t alignment,
        std::string migration_identity,
        NodeLayout::Region::RawInitializeFn initialize_fn,
        std::vector<std::byte> initialize_payload)
    {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            throw std::invalid_argument(
                "node layout raw-region alignment must be a non-zero power of two");
        }

        NodeLayout::Region region;
        region.kind = NodeLayout::Region::Kind::raw;
        region.owner_node = NodeLayout::no_owner_node;
        region.size = size;
        region.alignment = alignment;
        region.migration_identity = std::move(migration_identity);
        region.raw_initialize_fn = initialize_fn;
        region.raw_initialize_payload = std::move(initialize_payload);
        if (!region.raw_initialize_fn && !region.raw_initialize_payload.empty()) {
            throw std::invalid_argument(
                "node layout raw-region initialize payload requires a callback");
        }

        _storage_alignment = std::max(_storage_alignment, alignment);
        _regions.push_back(std::move(region));
        return NodeLayout::RegionHandle { .index = _regions.size() - 1 };
    }

    void NodeLayoutBuilder::log_node_event(
        char const* event, NodeLayout::NodeRecord const& record, size_t node_index)
    {
        (void)event;
        (void)record;
        (void)node_index;
    }

    void NodeLayoutBuilder::override_node_state_structures(
        size_t node_index, NodeStateStructures const& structures)
    {
        if (node_index >= _nodes.size()) {
            throw std::out_of_range("node state structure index out of range");
        }

        auto& node = _nodes[node_index];
        auto validate = [&](
            std::optional<NodeStateStructure> const& structure,
            size_t size,
            size_t alignment,
            char const* label) {
            if (!structure) {
                if (size != 0) {
                    throw std::invalid_argument(
                        std::string("missing reflected ") + label + " structure");
                }
                return;
            }
            if (
                structure->size_bits != size * 8 ||
                structure->alignment_bits != alignment * 8
            ) {
                throw std::invalid_argument(
                    std::string("reflected ") + label +
                    " structure does not match declared ABI");
            }
        };

        validate(structures.state, node.state_size, node.state_alignment, "State");
        validate(
            structures.compiled_state,
            node.compiled_state_size,
            node.compiled_state_alignment,
            "CompiledState");
        node.state_structure = structures.state;
        node.compiled_state_structure = structures.compiled_state;
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
            record.compiled_state_size = registration.compiled_state_size;
            record.compiled_state_alignment = registration.compiled_state_alignment;
            record.lifecycle = registration.lifecycle;
            if (registration.has_state) {
                record.state_structure = NodeStateStructure {
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
            if (registration.has_compiled_state) {
                record.compiled_state_structure = NodeStateStructure {
                    .size_bits = registration.compiled_state_size * 8,
                    .alignment_bits = registration.compiled_state_alignment * 8,
                };
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

        void allocate_node_compiled_state(
            NodeLayoutBuilder& builder,
            size_t node_index,
            size_t size,
            size_t alignment)
        {
            if (node_index >= builder._nodes.size()) {
                throw std::out_of_range(
                    "node compiled-state allocation index out of range");
            }

            auto& node = builder._nodes[node_index];
            if (node.compiled_state_size != 0 && node.compiled_state_offset >= 0) {
                return;
            }

            builder._storage_alignment =
                std::max(builder._storage_alignment, alignment);
            node.compiled_state_size = size;
            node.compiled_state_alignment = alignment;

            NodeLayout::Region region;
            region.kind = NodeLayout::Region::Kind::compiled_state;
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
            region.compiled_state_field = declaration.compiled_state_field;
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
                auto& span_ref =
                    *reinterpret_cast<std::span<std::span<std::byte>>*>(
                        static_cast<std::byte*>(state_base) + field_offset);
                span_ref = {
                    static_cast<std::span<std::byte>*>(data),
                    count,
                };
            };

            builder._storage_alignment = std::max(
                builder._storage_alignment,
                size_t(alignof(std::span<std::byte>)));
            builder._regions.push_back(std::move(region));
            return builder._regions.size() - 1;
        }

        size_t declare_nested_node_compiled_states(
            NodeLayoutBuilder& builder,
            size_t node_index,
            ptrdiff_t state_field_offset)
        {
            NodeLayout::Region region;
            region.kind = NodeLayout::Region::Kind::nested_node_compiled_states;
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
                auto& span_ref =
                    *reinterpret_cast<std::span<std::span<std::byte>>*>(
                        static_cast<std::byte*>(state_base) + field_offset);
                span_ref = {
                    static_cast<std::span<std::byte>*>(data),
                    count,
                };
            };

            builder._storage_alignment = std::max(
                builder._storage_alignment,
                size_t(alignof(std::span<std::byte>)));
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
                region.kind == NodeLayout::Region::Kind::nested_node_states ||
                    region.kind ==
                        NodeLayout::Region::Kind::nested_node_compiled_states,
                "region must be a nested node-state region");

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
                .compiled_state_field = declaration.compiled_state_field,
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
                .compiled_state_field = declaration.compiled_state_field,
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

        void override_node_state_structures(
            NodeLayoutBuilder& builder,
            size_t node_index,
            NodeStateStructures const& structures)
        {
            builder.override_node_state_structures(node_index, structures);
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
            } else if (region.kind == NodeLayout::Region::Kind::compiled_state) {
                layout.nodes[region.owner_node].compiled_state_offset =
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
            void* export_state = export_it->compiled_state_field
                ? storage.compiled_state_ptr(export_it->owner_node)
                : storage.state_ptr(export_it->owner_node);
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

        void* node_storage_compiled_state_ptr(
            NodeStorage const& storage, size_t node_index)
        {
            return storage.compiled_state_ptr(node_index);
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
    , constructed_compiled_states(std::move(other.constructed_compiled_states))
    , initialized_nodes(std::move(other.initialized_nodes))
    {
        other.layout = nullptr;
        other.resources = nullptr;
        other.constructed_nodes.clear();
        other.constructed_compiled_states.clear();
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
        constructed_compiled_states =
            std::move(other.constructed_compiled_states);
        initialized_nodes = std::move(other.initialized_nodes);

        other.layout = nullptr;
        other.resources = nullptr;
        other.constructed_nodes.clear();
        other.constructed_compiled_states.clear();
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
            size_t state_count = constructed_nodes.size();
            size_t compiled_state_count = constructed_compiled_states.size();
            for (size_t node_index = layout->nodes.size(); node_index-- > 0;) {
                auto const& node = layout->nodes[node_index];
                if (compiled_state_count != 0 &&
                    constructed_compiled_states[compiled_state_count - 1] ==
                        node_index) {
                    if (node.lifecycle.destroy_compiled_state_fn) {
                        node.lifecycle.destroy_compiled_state_fn(
                            compiled_state_ptr(node_index));
                    }
                    --compiled_state_count;
                }
                if (state_count != 0 &&
                    constructed_nodes[state_count - 1] == node_index) {
                    if (node.lifecycle.destroy_state_fn) {
                        NodeLayoutBuilder::log_node_event(
                            "destroyed", node, node_index);
                        node.lifecycle.destroy_state_fn(state_ptr(node_index));
                    }
                    --state_count;
                }
            }
        }
        constructed_compiled_states.clear();
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

    void* NodeStorage::compiled_state_ptr(size_t node_index) const
    {
        if (!layout || node_index >= layout->nodes.size()) {
            return nullptr;
        }
        auto const& node = layout->nodes[node_index];
        if (node.compiled_state_size == 0 || node.compiled_state_offset < 0) {
            return nullptr;
        }
        return storage.get() + node.compiled_state_offset;
    }

    std::span<std::byte> NodeStorage::region_bytes(
        NodeLayout::RegionHandle region) const
    {
        if (!layout || !region.valid() || region.index >= layout->regions.size()) {
            return {};
        }
        auto const& record = layout->regions[region.index];
        if (record.size == 0) {
            return {};
        }
        return { storage.get() + record.storage_offset, record.size };
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
        auto const same_reflected_definition = [](
            std::optional<NodeStateStructure> const& current,
            std::optional<NodeStateStructure> const& prior,
            size_t size) {
            if (size == 0) {
                return !current && !prior;
            }
            return current && prior && current->type_identity.valid() &&
                prior->type_identity.valid() && *current == *prior;
        };
        auto const same_state_definitions =
            same_reflected_definition(
                node.state_structure, previous_node.state_structure, node.state_size) &&
            same_reflected_definition(
                node.compiled_state_structure,
                previous_node.compiled_state_structure,
                node.compiled_state_size);
        auto const same_node_type = node.node_type == previous_node.node_type ||
            (same_node_name && same_state_definitions);
        if (!same_node_type || node.state_size != previous_node.state_size ||
            node.state_alignment != previous_node.state_alignment ||
            node.compiled_state_size != previous_node.compiled_state_size ||
            node.compiled_state_alignment != previous_node.compiled_state_alignment) {
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
                current_region.compiled_state_field !=
                    previous_region.compiled_state_field ||
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
            if ((region.kind != NodeLayout::Region::Kind::local_array &&
                 region.kind != NodeLayout::Region::Kind::nested_node_states &&
                 region.kind !=
                     NodeLayout::Region::Kind::nested_node_compiled_states) ||
                region.owner_node == NodeLayout::no_owner_node ||
                !owner_is_constructed(region.owner_node) ||
                !region.assign_span_fn) {
                continue;
            }
            void* state = region.compiled_state_field
                ? storage.compiled_state_ptr(region.owner_node)
                : storage.state_ptr(region.owner_node);
            void* data = storage.storage.get() + region.storage_offset;
            region.assign_span_fn(
                state, region.state_field_offset, data, region.element_count);
            if (region.kind != NodeLayout::Region::Kind::nested_node_states &&
                region.kind !=
                    NodeLayout::Region::Kind::nested_node_compiled_states) {
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
                auto const nested_node = region.nested_node_indices[i];
                if (region.kind ==
                    NodeLayout::Region::Kind::nested_node_compiled_states) {
                    auto* nested_state = static_cast<std::byte*>(
                        storage.compiled_state_ptr(nested_node));
                    nested_node_states[i] = nested_state
                        ? std::span<std::byte> {
                              nested_state,
                              storage.layout->nodes[nested_node]
                                  .compiled_state_size,
                          }
                        : std::span<std::byte> {};
                    continue;
                }

                auto* nested_state =
                    static_cast<std::byte*>(storage.state_ptr(nested_node));
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

    std::unordered_set<std::string> migrate_persistent_raw_regions(
        NodeStorage& current, NodeStorage const& previous)
    {
        std::unordered_set<std::string> migrated;
        if (!current.layout || !previous.layout) return migrated;

        auto collect = [](NodeLayout const& layout) {
            std::unordered_map<std::string, NodeLayout::Region const*> result;
            for (auto const& region : layout.regions) {
                if (region.kind != NodeLayout::Region::Kind::raw
                    || region.migration_identity.empty()) {
                    continue;
                }
                auto [_, inserted] = result.emplace(
                    region.migration_identity, &region);
                if (!inserted) {
                    throw std::runtime_error(
                        "duplicate raw-region migration identity: "
                        + region.migration_identity);
                }
            }
            return result;
        };

        auto const previous_regions = collect(*previous.layout);
        auto const current_regions = collect(*current.layout);
        for (auto const& [identity, region] : current_regions) {
            auto const found = previous_regions.find(identity);
            if (found == previous_regions.end()) continue;
            auto const* prior = found->second;
            if (region->size != prior->size
                || region->alignment != prior->alignment) {
                continue;
            }
            if (region->size != 0) {
                std::memcpy(
                    current.storage.get() + region->storage_offset,
                    previous.storage.get() + prior->storage_offset,
                    region->size);
            }
            migrated.insert(identity);
        }
        return migrated;
    }

    void initialize_raw_regions(
        NodeStorage& storage,
        std::unordered_set<std::string> const& migrated)
    {
        if (!storage.layout) return;
        for (auto const& region : storage.layout->regions) {
            if (region.kind != NodeLayout::Region::Kind::raw
                || !region.raw_initialize_fn) {
                continue;
            }
            if (!region.migration_identity.empty()
                && migrated.contains(region.migration_identity)) {
                continue;
            }
            auto bytes = storage.buffer();
            region.raw_initialize_fn(
                bytes.subspan(region.storage_offset, region.size),
                region.raw_initialize_payload);
        }
    }

    void construct_node_storage_states(NodeStorage& storage)
    {
        for (size_t node_index = 0;
             node_index < storage.layout->nodes.size();
             ++node_index) {
            auto const& record = storage.layout->nodes[node_index];
            if (record.state_size != 0) {
                if (record.lifecycle.default_construct_state_fn) {
                    record.lifecycle.default_construct_state_fn(
                        storage.state_ptr(node_index));
                }
                storage.constructed_nodes.push_back(node_index);
            }
            if (record.compiled_state_size != 0) {
                if (record.lifecycle.default_construct_compiled_state_fn) {
                    record.lifecycle.default_construct_compiled_state_fn(
                        storage.compiled_state_ptr(node_index));
                }
                storage.constructed_compiled_states.push_back(node_index);
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
                void* export_state = export_it->compiled_state_field
                    ? storage.compiled_state_ptr(export_it->owner_node)
                    : storage.state_ptr(export_it->owner_node);
                export_it->read_span_fn(
                    export_state, export_it->state_field_offset, data, count);
            }
            if (import_endpoint.assign_span_fn) {
                void* import_state = import_endpoint.compiled_state_field
                    ? storage.compiled_state_ptr(import_endpoint.owner_node)
                    : storage.state_ptr(import_endpoint.owner_node);
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
        if (!constructed_nodes.empty() ||
            !constructed_compiled_states.empty() ||
            !initialized_nodes.empty()) {
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
        constructed_compiled_states.reserve(layout->nodes.size());
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

        construct_node_storage_states(*this);
        auto const migrated_raw_regions =
            migrate_persistent_raw_regions(*this, previous);
        initialize_raw_regions(*this, migrated_raw_regions);

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
                if (!record.lifecycle.move_fn) {
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
        constructed_compiled_states.clear();
        initialized_nodes.clear();
        construct_node_storage_states(*this);
        std::unordered_set<std::string> migrated_raw_regions;
        if (previous && previous->layout) {
            migrated_raw_regions = migrate_persistent_raw_regions(*this, *previous);
        }
        initialize_raw_regions(*this, migrated_raw_regions);

        patch_node_storage_regions(*this, [](size_t) { return true; });
        patch_node_storage_imports(*this);

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
