#pragma once

#include <intravenous/node/context.h>
#include <intravenous/node/node_state_structure.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    struct NodeLayout {
        struct Region {
            enum class Kind {
                state,
                local_array,
                nested_node_states,
            };

            Kind kind = Kind::state;
            size_t owner_node = 0;
            ptrdiff_t state_field_offset = 0;
            size_t storage_offset = 0;
            size_t size = 0;
            size_t alignment = 1;
            size_t element_count = 0;
            void const* element_type = nullptr;
            char const* element_type_name = nullptr;
            void (*assign_span_fn)(
                void* state_base, ptrdiff_t field_offset, void* data,
                size_t count) = nullptr;
            std::vector<size_t> nested_node_indices;
        };

        struct ArrayBinding {
            size_t owner_node = 0;
            std::string id;
            ptrdiff_t state_field_offset = 0;
            void const* element_type = nullptr;
            size_t element_size = 0;
            void (*assign_span_fn)(
                void* state_base, ptrdiff_t field_offset, void* data,
                size_t count) = nullptr;
            void (*read_span_fn)(
                void const* state_base, ptrdiff_t field_offset, void*& data,
                size_t& count) = nullptr;
        };

        struct NodeRecord {
            void const* node = nullptr;
            void const* node_type = nullptr;
            char const* node_type_name = nullptr;
            std::optional<NodeStateStructure> node_state_structure {};
            ptrdiff_t state_offset = 0;
            size_t state_size = 0;
            size_t state_alignment = 1;
            std::vector<size_t> dependencies;
            NodeLifecycleCallbacks lifecycle;
        };

        size_t storage_size = 0;
        size_t storage_alignment = 1;
        size_t max_block_size = 1;
        size_t default_silence_ttl_samples = std::numeric_limits<size_t>::max();
        std::vector<NodeRecord> nodes;
        std::vector<Region> regions;
        std::vector<ArrayBinding> imported_arrays;
        std::vector<ArrayBinding> exported_arrays;
        std::vector<size_t> initialize_order;

        NodeStorage create_storage(ResourceContext const& resources) const;
    };

    struct NodeLayoutBuilder {
        explicit NodeLayoutBuilder(size_t max_block_size = 1);
        explicit NodeLayoutBuilder(
            size_t max_block_size, size_t default_silence_ttl_samples);
        explicit NodeLayoutBuilder(
            size_t max_block_size,
            size_t default_silence_ttl_samples,
            size_t event_port_buffer_base_multiplier);

        size_t max_block_size() const;
        size_t default_silence_ttl_samples() const;
        size_t event_port_buffer_base_multiplier() const;

        template<typename A>
        static void const* array_type_token()
        {
            return details::node_layout_type_token<A>();
        }

        static void log_node_event(
            char const* event,
            NodeLayout::NodeRecord const& record,
            size_t node_index);

        void override_node_state_structure(
            size_t node_index, NodeStateStructure structure);

        template<typename A>
        bool has_import_array(std::string const& id) const
        {
            return details::has_import_array(
                *this, id, details::node_layout_type_token<A>());
        }

        template<typename A>
        bool has_export_array(std::string const& id) const
        {
            return details::has_export_array(
                *this, id, details::node_layout_type_token<A>());
        }

        NodeLayout build() &&;

    private:
        size_t _max_block_size = 1;
        size_t _default_silence_ttl_samples = std::numeric_limits<size_t>::max();
        size_t _event_port_buffer_base_multiplier =
            DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER;
        size_t _storage_alignment = 1;
        std::vector<NodeLayout::NodeRecord> _nodes;
        std::vector<NodeLayout::Region> _regions;
        std::vector<NodeLayout::ArrayBinding> _imports;
        std::vector<NodeLayout::ArrayBinding> _exports;

        static size_t align_up(size_t value, size_t alignment);

        friend size_t details::register_node(
            NodeLayoutBuilder&, details::NodeLayoutNodeRegistration const&);
        friend void details::allocate_node_state(
            NodeLayoutBuilder&, size_t, size_t, size_t);
        friend void details::declare_local_array(
            NodeLayoutBuilder&, details::NodeLayoutArrayDeclaration const&);
        friend size_t details::declare_nested_node_states(
            NodeLayoutBuilder&, size_t, ptrdiff_t);
        friend void details::finalize_nested_node_states(
            NodeLayoutBuilder&, size_t, std::vector<size_t>);
        friend void details::declare_export_array(
            NodeLayoutBuilder&, std::string,
            details::NodeLayoutArrayDeclaration const&);
        friend void details::declare_import_array(
            NodeLayoutBuilder&, std::string,
            details::NodeLayoutArrayDeclaration const&);
        friend void details::require_export_array(
            NodeLayoutBuilder&, size_t, std::string, void const*, size_t);
        friend bool details::has_import_array(
            NodeLayoutBuilder const&, std::string const&, void const*);
        friend bool details::has_export_array(
            NodeLayoutBuilder const&, std::string const&, void const*);
        friend size_t details::node_layout_max_block_size(
            NodeLayoutBuilder const&);
        friend size_t details::node_layout_event_port_buffer_base_multiplier(
            NodeLayoutBuilder const&);
    };

    struct NodeStorage {
        struct PreparedMigration;

        struct StorageDeleter {
            size_t alignment = alignof(std::max_align_t);

            constexpr StorageDeleter() noexcept = default;
            constexpr explicit StorageDeleter(size_t alignment_) noexcept
                : alignment(alignment_)
            {}

            void operator()(std::byte* p) const noexcept;
        };

        NodeLayout const* layout = nullptr;
        ResourceContext const* resources = nullptr;
        std::unique_ptr<std::byte[], StorageDeleter> storage;
        std::vector<size_t> constructed_nodes;
        std::vector<size_t> initialized_nodes;

        NodeStorage();
        NodeStorage(NodeLayout const& layout, ResourceContext const& resources);
        NodeStorage(NodeStorage&& other) noexcept;
        NodeStorage& operator=(NodeStorage&& other) noexcept;
        NodeStorage(NodeStorage const&) = delete;
        NodeStorage& operator=(NodeStorage const&) = delete;
        ~NodeStorage();

        std::span<std::byte> buffer() const;
        size_t max_block_size() const;
        void* state_ptr(size_t node_index) const;

        template<typename A>
        std::span<A const> resolve_exported_array_storage(
            std::string const& id) const
        {
            auto const resolved = details::resolve_exported_array_storage(
                *this, id, details::node_layout_type_token<A>());
            return { static_cast<A const*>(resolved.data), resolved.count };
        }

        bool can_move_from(
            NodeStorage const& previous,
            size_t node_index,
            size_t previous_node_index) const;
        PreparedMigration prepare_migration_from(NodeStorage& previous);
        void initialize(NodeStorage const* previous = nullptr);
        void release();
        void destroy_constructed_states();
    };

    struct NodeStorage::PreparedMigration {
        static constexpr size_t no_node = std::numeric_limits<size_t>::max();

        NodeStorage* current = nullptr;
        NodeStorage* previous = nullptr;
        std::vector<size_t> previous_node_for_current;
        std::vector<bool> previous_nodes_consumed;
        std::vector<size_t> deferred_initialize_nodes;
        std::vector<size_t> previous_release_nodes;
        bool committed = false;

        PreparedMigration() = default;
        PreparedMigration(PreparedMigration&&) noexcept = default;
        PreparedMigration& operator=(PreparedMigration&&) noexcept = default;
        PreparedMigration(PreparedMigration const&) = delete;
        PreparedMigration& operator=(PreparedMigration const&) = delete;

        void commit();
    };
}
