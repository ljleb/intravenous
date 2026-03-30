#pragma once

#include "compat.h"
#include "node_traits.h"
#include "node_resources.h"
#include "execution_targets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <sstream>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iv {
    template<typename Node>
    struct DeclarationContext;

    template<typename Node>
    struct InitializationContext;

    template<typename Node>
    struct ReleaseContext;

    template<typename Node>
    struct MoveContext;

    struct NodeLayout;
    struct NodeStorage;

    struct NodeLifecycleCallbacks {
        void (*move_fn)(void const*, size_t, NodeStorage&, NodeStorage const&, ExecutionTargets*) = nullptr;
        void (*initialize_fn)(void const*, size_t, NodeStorage&, ExecutionTargets*) = nullptr;
        void (*release_fn)(void const*, size_t, NodeStorage&, ExecutionTargets*) = nullptr;
        void (*default_construct_state_fn)(void*) = nullptr;
        void (*move_construct_state_fn)(void*, void*) = nullptr;
        void (*destroy_state_fn)(void*) = nullptr;
    };

    struct NodeLayout {
        struct Region {
            enum class Kind {
                state,
                local_array,
                nested_nodes,
            };

            Kind kind = Kind::state;
            size_t owner_node = 0;
            ptrdiff_t state_field_offset = 0;
            size_t storage_offset = 0;
            size_t size = 0;
            size_t alignment = 1;
            size_t element_count = 0;
            void const* element_type = nullptr;
            void (*assign_span_fn)(void* state_base, ptrdiff_t field_offset, void* data, size_t count) = nullptr;
            std::vector<size_t> nested_node_indices;
        };

        struct ArrayBinding {
            size_t owner_node = 0;
            std::string id;
            ptrdiff_t state_field_offset = 0;
            void const* element_type = nullptr;
            size_t element_size = 0;
            void (*assign_span_fn)(void* state_base, ptrdiff_t field_offset, void* data, size_t count) = nullptr;
            void (*read_span_fn)(void const* state_base, ptrdiff_t field_offset, void*& data, size_t& count) = nullptr;
        };

        struct NodeRecord {
            void const* node = nullptr;
            void const* node_type = nullptr;
            char const* node_type_name = nullptr;
            ptrdiff_t state_offset = 0;
            size_t state_size = 0;
            std::vector<size_t> dependencies;
            NodeLifecycleCallbacks lifecycle;
        };

        size_t storage_size = 0;
        size_t storage_alignment = 1;
        size_t max_block_size = 1;
        std::vector<NodeRecord> nodes;
        std::vector<Region> regions;
        std::vector<ArrayBinding> imported_arrays;
        std::vector<ArrayBinding> exported_arrays;
        std::vector<size_t> initialize_order;

        NodeStorage create_storage(ResourceContext const& resources) const;
    };

    struct NodeLayoutBuilder {
        template<typename Node>
        size_t register_node(Node const& node);

        template<typename Node, typename A>
        A const* local_object(size_t node_index);

        template<typename Marker, typename A>
        void local_array(size_t node_index, Marker const*, std::span<A> const*, size_t);

        template<typename Marker>
        void nested_nodes(size_t node_index, Marker const*, std::span<std::byte*> const*, std::vector<size_t>);

        template<typename A>
        void export_array(size_t node_index, std::string id, std::span<A> const*);

        template<typename A>
        void import_array(size_t node_index, std::string id, std::span<A> const*);

        template<typename A>
        void require_export_array(size_t node_index, std::string id);

        template<typename A>
        bool has_import_array(std::string const& id) const;

        template<typename A>
        bool has_export_array(std::string const& id) const;

        explicit NodeLayoutBuilder(size_t max_block_size = 1)
        : _max_block_size(max_block_size)
        {}

        size_t max_block_size() const;

        template<typename A>
        static void const* array_type_token()
        {
            return type_token<A>();
        }

        static void log_node_event(char const* event, NodeLayout::NodeRecord const& record, size_t node_index)
        {
            (void)event;
            (void)record;
            (void)node_index;
        }

        NodeLayout build() &&;

    private:
        size_t _max_block_size = 1;
        size_t _storage_size = 0;
        size_t _storage_alignment = 1;
        std::vector<NodeLayout::NodeRecord> _nodes;
        std::vector<NodeLayout::Region> _regions;
        std::vector<NodeLayout::ArrayBinding> _imports;
        std::vector<NodeLayout::ArrayBinding> _exports;

        template<typename Node>
        static NodeLifecycleCallbacks make_lifecycle_callbacks();

        template<typename A>
        static void const* type_token();

        static size_t align_up(size_t value, size_t alignment);
    };

    struct NodeStorage {
        struct StorageDeleter {
            size_t alignment = alignof(std::max_align_t);

            void operator()(std::byte* p) const noexcept
            {
                if (p) {
                    ::operator delete(p, std::align_val_t(alignment));
                }
            }
        };

        NodeLayout const* layout = nullptr;
        ResourceContext const* resources = nullptr;
        std::unique_ptr<std::byte[], StorageDeleter> storage {};
        std::vector<size_t> constructed_nodes;
        std::vector<size_t> initialized_nodes;

        NodeStorage() = default;
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
        std::span<A const> resolve_exported_array_storage(std::string const& id) const;
        bool can_move_from(NodeStorage const& previous, size_t node_index) const;
        void initialize(NodeStorage const* previous = nullptr, ExecutionTargets* execution_targets = nullptr);
        void release(ExecutionTargets* execution_targets = nullptr);
    };

    template<typename Node>
    struct DeclarationContext
    {
        template<typename>
        friend struct DeclarationContext;

        using State = typename NodeState<Node>::Type;

    private:
        NodeLayoutBuilder* _builder;
        size_t _node_index;
        State const* _state_marker;
        mutable std::vector<size_t> _direct_nested_node_indices;
        mutable size_t _nested_node_cursor = 0;

    public:
        explicit DeclarationContext(NodeLayoutBuilder& builder, Node const& node);

        template<typename Node2>
        DeclarationContext(DeclarationContext<Node2> const& ctx, Node const& node);

        NoCopy<State> const& state() const
        requires(!std::is_void_v<State>);

        template<typename A>
        void local_array(std::span<A> const& span, size_t count) const;

        template<typename A>
        void export_array(std::string id, std::span<A> const& span) const;

        template<typename A>
        void import_array(std::string id, std::span<A> const& span) const;

        template<typename A>
        void require_export_array(std::string id) const;

        template<typename A>
        bool has_import_array(std::string const& id) const;

        template<typename A>
        bool has_export_array(std::string const& id) const;

        void nested_node_states(std::span<std::byte*> const& nodes) const;

        size_t max_block_size() const;

        size_t node_index() const
        {
            return _node_index;
        }

        size_t pending_direct_nested_node_count() const
        {
            return _direct_nested_node_indices.size() - _nested_node_cursor;
        }
    };

    template<typename Node>
    struct InitializationContext {
        template<typename>
        friend struct InitializationContext;

        using State = typename NodeState<Node>::Type;

    private:
        NodeStorage* _storage;
        void* _state = nullptr;

    public:
        ResourceContext const& resources;
        ExecutionTargets* execution_targets = nullptr;

        explicit InitializationContext(NodeStorage& storage, void* state, ResourceContext const& resources, ExecutionTargets* execution_targets = nullptr);

        template<typename Node2>
        InitializationContext(InitializationContext<Node2> const& ctx);

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>);

        NodeStorage& storage() const
        {
            return *_storage;
        }

        size_t max_block_size() const
        {
            return _storage->max_block_size();
        }

        template<typename A>
        std::span<A const> resolve_exported_array_storage(std::string const& id) const;
    };

    template<typename Node>
    struct ReleaseContext {
        template<typename>
        friend struct ReleaseContext;

        using State = typename NodeState<Node>::Type;

    private:
        NodeStorage* _storage;
        void* _state = nullptr;

    public:
        ResourceContext const& resources;
        ExecutionTargets* execution_targets = nullptr;

        explicit ReleaseContext(NodeStorage& storage, void* state, ResourceContext const& resources, ExecutionTargets* execution_targets = nullptr);

        template<typename Node2>
        ReleaseContext(ReleaseContext<Node2> const& ctx);

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>);
    };

    template<typename Node>
    struct MoveContext {
        template<typename>
        friend struct MoveContext;

        using State = typename NodeState<Node>::Type;

    private:
        NodeStorage* _storage;
        void* _state = nullptr;
        NodeStorage const* _previous_storage;
        void* _previous_state = nullptr;

    public:
        ResourceContext const& resources;
        ExecutionTargets* execution_targets = nullptr;

        explicit MoveContext(
            NodeStorage& storage,
            void* state,
            NodeStorage const& previous_storage,
            void* previous_state,
            ResourceContext const& resources,
            ExecutionTargets* execution_targets = nullptr
        );

        template<typename Node2>
        MoveContext(MoveContext<Node2> const& ctx);

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>);

        std::add_lvalue_reference_t<State> previous_state() const
        requires(!std::is_void_v<State>);
    };

    namespace details {
        template <typename Node>
        concept has_declare = requires(Node node, DeclarationContext<Node> ctx)
        {
            node.declare(ctx);
        };

        template <typename Node>
        concept has_initialize = requires(Node node, InitializationContext<Node> ctx)
        {
            node.initialize(ctx);
        };

        template <typename Node>
        concept has_release = requires(Node node, ReleaseContext<Node> ctx)
        {
            node.release(ctx);
        };

        template <typename Node>
        concept has_move = requires(Node node, MoveContext<Node> ctx)
        {
            node.move(ctx);
        };
    }

    template<typename Node>
    inline DeclarationContext<Node>::DeclarationContext(NodeLayoutBuilder& builder, Node const& node)
    : _builder(&builder)
    , _node_index(_builder->register_node(node))
    {
        if constexpr (!std::is_void_v<State>) {
            _state_marker = _builder->template local_object<Node, State>(_node_index);
        } else {
            _state_marker = nullptr;
        }
    }

    template<typename Node>
    template<typename Node2>
    inline DeclarationContext<Node>::DeclarationContext(DeclarationContext<Node2> const& ctx, Node const& node)
    : DeclarationContext<Node>(*ctx._builder, node)
    {
        ctx._direct_nested_node_indices.push_back(_node_index);
    }

    template<typename Node>
    inline NoCopy<typename DeclarationContext<Node>::State> const& DeclarationContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        return reinterpret_cast<NoCopy<State> const&>(*_state_marker);
    }

    template<typename Node>
    template<typename A>
    inline void DeclarationContext<Node>::local_array(std::span<A> const& span, size_t count) const
    {
        _builder->local_array(_node_index, _state_marker, &span, count);
    }

    template<typename Node>
    template<typename A>
    inline void DeclarationContext<Node>::export_array(std::string id, std::span<A> const& span) const
    {
        _builder->export_array(_node_index, std::move(id), &span);
    }

    template<typename Node>
    template<typename A>
    inline void DeclarationContext<Node>::import_array(std::string id, std::span<A> const& span) const
    {
        _builder->import_array(_node_index, std::move(id), &span);
    }

    template<typename Node>
    template<typename A>
    inline void DeclarationContext<Node>::require_export_array(std::string id) const
    {
        _builder->template require_export_array<A>(_node_index, std::move(id));
    }

    template<typename Node>
    template<typename A>
    inline bool DeclarationContext<Node>::has_import_array(std::string const& id) const
    {
        return _builder->template has_import_array<A>(id);
    }

    template<typename Node>
    template<typename A>
    inline bool DeclarationContext<Node>::has_export_array(std::string const& id) const
    {
        return _builder->template has_export_array<A>(id);
    }

    template<typename Node>
    inline void DeclarationContext<Node>::nested_node_states(std::span<std::byte*> const& nodes) const
    {
        std::vector<size_t> nested_node_indices;
        nested_node_indices.reserve(_direct_nested_node_indices.size() - _nested_node_cursor);
        for (size_t i = _nested_node_cursor; i < _direct_nested_node_indices.size(); ++i) {
            nested_node_indices.push_back(_direct_nested_node_indices[i]);
        }
        _nested_node_cursor = _direct_nested_node_indices.size();
        _builder->nested_nodes(_node_index, _state_marker, &nodes, std::move(nested_node_indices));
    }

    template<typename Node>
    inline size_t DeclarationContext<Node>::max_block_size() const
    {
        return _builder->max_block_size();
    }

    template<typename Node>
    size_t NodeLayoutBuilder::register_node(Node const& node)
    {
        size_t const node_index = _nodes.size();

        NodeLayout::NodeRecord record;
        if constexpr (!std::is_empty_v<Node>) {
            record.node = std::addressof(node);
        }
        record.node_type = type_token<Node>();
        record.node_type_name = typeid(Node).name();
        record.lifecycle = make_lifecycle_callbacks<Node>();
        if constexpr (std::is_void_v<typename NodeState<Node>::Type>) {
            size_t const offset = align_up(_storage_size, 1);
            record.state_offset = static_cast<ptrdiff_t>(offset);
            record.state_size = 0;
            _storage_size = offset + 1;

            NodeLayout::Region region;
            region.kind = NodeLayout::Region::Kind::state;
            region.owner_node = node_index;
            region.storage_offset = offset;
            region.size = 0;
            region.alignment = 1;
            _regions.push_back(region);
        }
        _nodes.push_back(std::move(record));
        return node_index;
    }

    template<typename Node, typename A>
    A const* NodeLayoutBuilder::local_object(size_t node_index)
    {
        constexpr uintptr_t fictitious_base = 0x10000;

        auto& node = _nodes[node_index];
        if (node.state_size == 0) {
            _storage_alignment = std::max(_storage_alignment, size_t(alignof(A)));
            size_t const offset = align_up(_storage_size, alignof(A));
            node.state_offset = static_cast<ptrdiff_t>(offset);
            node.state_size = sizeof(A);
            _storage_size = offset + sizeof(A);

            NodeLayout::Region region;
            region.kind = NodeLayout::Region::Kind::state;
            region.owner_node = node_index;
            region.storage_offset = offset;
            region.size = sizeof(A);
            region.alignment = alignof(A);
            _regions.push_back(region);
        }

        return reinterpret_cast<A const*>(fictitious_base + node.state_offset);
    }

    template<typename Marker, typename A>
    void NodeLayoutBuilder::local_array(size_t node_index, Marker const* state_marker, std::span<A> const* span, size_t count)
    {
        auto const state_base = reinterpret_cast<uintptr_t>(state_marker);
        auto const field_ptr = reinterpret_cast<uintptr_t>(span);

        NodeLayout::Region region;
        region.kind = NodeLayout::Region::Kind::local_array;
        region.owner_node = node_index;
        region.state_field_offset = static_cast<ptrdiff_t>(field_ptr - state_base);
        region.storage_offset = align_up(_storage_size, alignof(A));
        region.size = sizeof(A) * count;
        region.alignment = alignof(A);
        region.element_count = count;
        region.element_type = type_token<A>();
        region.assign_span_fn = [](void* state_base_ptr, ptrdiff_t field_offset, void* data, size_t count_value) {
            auto& span_ref = *reinterpret_cast<std::span<A>*>(static_cast<std::byte*>(state_base_ptr) + field_offset);
            span_ref = { static_cast<A*>(data), count_value };
        };

        _storage_alignment = std::max(_storage_alignment, size_t(alignof(A)));
        _storage_size = region.storage_offset + region.size;

        _regions.push_back(region);
    }

    template<typename Marker>
    void NodeLayoutBuilder::nested_nodes(
        size_t node_index,
        Marker const* state_marker,
        std::span<std::byte*> const* span,
        std::vector<size_t> nested_node_indices
    )
    {
        auto const state_base = reinterpret_cast<uintptr_t>(state_marker);
        auto const field_ptr = reinterpret_cast<uintptr_t>(span);

        NodeLayout::Region region;
        region.kind = NodeLayout::Region::Kind::nested_nodes;
        region.owner_node = node_index;
        region.state_field_offset = static_cast<ptrdiff_t>(field_ptr - state_base);
        region.storage_offset = align_up(_storage_size, alignof(std::byte*));
        region.size = sizeof(std::byte*) * nested_node_indices.size();
        region.alignment = alignof(std::byte*);
        region.element_count = nested_node_indices.size();
        region.element_type = type_token<std::byte*>();
        region.assign_span_fn = [](void* state_base_ptr, ptrdiff_t field_offset, void* data, size_t count_value) {
            auto& span_ref = *reinterpret_cast<std::span<std::byte*>*>(static_cast<std::byte*>(state_base_ptr) + field_offset);
            span_ref = { static_cast<std::byte**>(data), count_value };
        };
        region.nested_node_indices = std::move(nested_node_indices);

        _storage_alignment = std::max(_storage_alignment, size_t(alignof(std::byte*)));
        _storage_size = region.storage_offset + region.size;

        _regions.push_back(std::move(region));
    }

    template<typename A>
    void NodeLayoutBuilder::export_array(size_t node_index, std::string id, std::span<A> const* span)
    {
        constexpr uintptr_t fictitious_base = 0x10000;
        auto const state_base = fictitious_base + _nodes[node_index].state_offset;
        auto const field_ptr = reinterpret_cast<uintptr_t>(span);
        auto const field_offset = static_cast<ptrdiff_t>(field_ptr - state_base);

        _exports.push_back({
            .owner_node = node_index,
            .id = std::move(id),
            .state_field_offset = field_offset,
            .element_type = type_token<A>(),
            .element_size = sizeof(A),
            .assign_span_fn = nullptr,
            .read_span_fn = [](void const* state_base_ptr, ptrdiff_t offset, void*& data, size_t& count_value) {
                auto const& span_ref = *reinterpret_cast<std::span<A> const*>(static_cast<std::byte const*>(state_base_ptr) + offset);
                data = span_ref.data();
                count_value = span_ref.size();
            },
        });
    }

    template<typename A>
    void NodeLayoutBuilder::import_array(size_t node_index, std::string id, std::span<A> const* span)
    {
        constexpr uintptr_t fictitious_base = 0x10000;
        auto const state_base = fictitious_base + _nodes[node_index].state_offset;
        auto const field_ptr = reinterpret_cast<uintptr_t>(span);
        auto const field_offset = static_cast<ptrdiff_t>(field_ptr - state_base);

        _imports.push_back({
            .owner_node = node_index,
            .id = std::move(id),
            .state_field_offset = field_offset,
            .element_type = type_token<A>(),
            .element_size = sizeof(A),
            .assign_span_fn = [](void* state_base_ptr, ptrdiff_t offset, void* data, size_t count_value) {
                auto& span_ref = *reinterpret_cast<std::span<A>*>(static_cast<std::byte*>(state_base_ptr) + offset);
                span_ref = { static_cast<A*>(data), count_value };
            },
            .read_span_fn = nullptr,
        });
    }

    template<typename A>
    void NodeLayoutBuilder::require_export_array(size_t node_index, std::string id)
    {
        _imports.push_back({
            .owner_node = node_index,
            .id = std::move(id),
            .state_field_offset = 0,
            .element_type = type_token<A>(),
            .element_size = sizeof(A),
            .assign_span_fn = nullptr,
            .read_span_fn = nullptr,
        });
    }

    template<typename A>
    bool NodeLayoutBuilder::has_import_array(std::string const& id) const
    {
        return std::any_of(_imports.begin(), _imports.end(), [&](auto const& binding) {
            return binding.id == id && binding.element_type == type_token<A>();
        });
    }

    template<typename A>
    bool NodeLayoutBuilder::has_export_array(std::string const& id) const
    {
        return std::any_of(_exports.begin(), _exports.end(), [&](auto const& binding) {
            return binding.id == id && binding.element_type == type_token<A>();
        });
    }

    inline size_t NodeLayoutBuilder::max_block_size() const
    {
        return _max_block_size;
    }

    inline NodeLayout NodeLayoutBuilder::build() &&
    {
        NodeLayout layout;
        layout.storage_size = _storage_size;
        layout.storage_alignment = _storage_alignment;
        layout.max_block_size = _max_block_size;
        layout.nodes = std::move(_nodes);
        layout.regions = std::move(_regions);
        layout.imported_arrays = std::move(_imports);
        layout.exported_arrays = std::move(_exports);

        for (auto const& import_binding : layout.imported_arrays) {
            for (auto const& export_binding : layout.exported_arrays) {
                if (
                    import_binding.id == export_binding.id &&
                    import_binding.element_type == export_binding.element_type &&
                    import_binding.owner_node != export_binding.owner_node
                ) {
                    layout.nodes[import_binding.owner_node].dependencies.push_back(export_binding.owner_node);
                }
            }
        }

        for (auto& node : layout.nodes) {
            std::sort(node.dependencies.begin(), node.dependencies.end());
            node.dependencies.erase(std::unique(node.dependencies.begin(), node.dependencies.end()), node.dependencies.end());
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

    template<typename Node>
    NodeLifecycleCallbacks NodeLayoutBuilder::make_lifecycle_callbacks()
    {
        NodeLifecycleCallbacks callbacks;

        if constexpr (!std::is_void_v<typename NodeState<Node>::Type>) {
            using State = typename NodeState<Node>::Type;
            callbacks.default_construct_state_fn = [](void* ptr) {
                new (ptr) State();
            };
            if constexpr (std::is_move_constructible_v<State>) {
                callbacks.move_construct_state_fn = [](void* ptr, void* previous_ptr) {
                    new (ptr) State(std::move(*static_cast<State*>(previous_ptr)));
                };
            }
            callbacks.destroy_state_fn = [](void* ptr) {
                std::destroy_at(static_cast<State*>(ptr));
            };
        }

        if constexpr (requires(Node const& node, MoveContext<Node> ctx) { node.move(ctx); }) {
            callbacks.move_fn = [](void const* node_ptr, size_t node_index, NodeStorage& storage, NodeStorage const& previous_storage, ExecutionTargets* execution_targets) {
                void* state = storage.state_ptr(node_index);
                void* previous_state = previous_storage.state_ptr(node_index);
                if constexpr (std::is_empty_v<Node>) {
                    (void) node_ptr;
                    Node node {};
                    MoveContext<Node> ctx(storage, state, previous_storage, previous_state, *storage.resources, execution_targets);
                    node.move(ctx);
                } else {
                    auto const& node = *static_cast<Node const*>(node_ptr);
                    MoveContext<Node> ctx(storage, state, previous_storage, previous_state, *storage.resources, execution_targets);
                    node.move(ctx);
                }
            };
        }

        if constexpr (requires(Node const& node, InitializationContext<Node> ctx) { node.initialize(ctx); }) {
            callbacks.initialize_fn = [](void const* node_ptr, size_t node_index, NodeStorage& storage, ExecutionTargets* execution_targets) {
                void* state = storage.state_ptr(node_index);
                if constexpr (std::is_empty_v<Node>) {
                    (void)node_ptr;
                    Node node {};
                    InitializationContext<Node> ctx(storage, state, *storage.resources, execution_targets);
                    node.initialize(ctx);
                } else {
                    auto const& node = *static_cast<Node const*>(node_ptr);
                    InitializationContext<Node> ctx(storage, state, *storage.resources, execution_targets);
                    node.initialize(ctx);
                }
            };
        }

        if constexpr (requires(Node const& node, ReleaseContext<Node> ctx) { node.release(ctx); }) {
            callbacks.release_fn = [](void const* node_ptr, size_t node_index, NodeStorage& storage, ExecutionTargets* execution_targets) {
                void* state = storage.state_ptr(node_index);
                if constexpr (std::is_empty_v<Node>) {
                    (void)node_ptr;
                    Node node {};
                    ReleaseContext<Node> ctx(storage, state, *storage.resources, execution_targets);
                    node.release(ctx);
                } else {
                    auto const& node = *static_cast<Node const*>(node_ptr);
                    ReleaseContext<Node> ctx(storage, state, *storage.resources, execution_targets);
                    node.release(ctx);
                }
            };
        }

        return callbacks;
    }

    template<typename A>
    void const* NodeLayoutBuilder::type_token()
    {
        static int token = 0;
        return &token;
    }

    inline size_t NodeLayoutBuilder::align_up(size_t value, size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    inline NodeStorage NodeLayout::create_storage(ResourceContext const& resources) const
    {
        return NodeStorage(*this, resources);
    }

    inline NodeStorage::NodeStorage(NodeLayout const& layout_, ResourceContext const& resources_)
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

    inline NodeStorage::NodeStorage(NodeStorage&& other) noexcept
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

    inline NodeStorage& NodeStorage::operator=(NodeStorage&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        release();
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

    inline NodeStorage::~NodeStorage()
    {
        release();
        if (layout) {
            for (auto it = constructed_nodes.rbegin(); it != constructed_nodes.rend(); ++it) {
                auto const& node = layout->nodes[*it];
                if (node.state_size != 0 && node.lifecycle.destroy_state_fn) {
                    NodeLayoutBuilder::log_node_event("destroyed", node, *it);
                    node.lifecycle.destroy_state_fn(state_ptr(*it));
                }
            }
        }
    }

    inline std::span<std::byte> NodeStorage::buffer() const
    {
        return { storage.get(), layout ? layout->storage_size : 0 };
    }

    inline size_t NodeStorage::max_block_size() const
    {
        return layout ? layout->max_block_size : 1;
    }

    inline void* NodeStorage::state_ptr(size_t node_index) const
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

    template<typename A>
    inline std::span<A const> NodeStorage::resolve_exported_array_storage(std::string const& id) const
    {
        if (!layout) {
            return {};
        }

        auto export_it = std::find_if(
            layout->exported_arrays.begin(),
            layout->exported_arrays.end(),
            [&](auto const& export_endpoint) {
                return export_endpoint.id == id && export_endpoint.element_type == NodeLayoutBuilder::array_type_token<A>();
            }
        );
        if (export_it == layout->exported_arrays.end() || !export_it->read_span_fn) {
            return {};
        }

        void* data = nullptr;
        size_t count = 0;
        void* export_state = state_ptr(export_it->owner_node);
        export_it->read_span_fn(export_state, export_it->state_field_offset, data, count);
        return { static_cast<A const*>(data), count };
    }

    inline bool NodeStorage::can_move_from(NodeStorage const& previous, size_t node_index) const
    {
        if (!layout || !previous.layout) {
            return false;
        }
        if (node_index >= layout->nodes.size() || node_index >= previous.layout->nodes.size()) {
            return false;
        }

        auto const& node = layout->nodes[node_index];
        auto const& previous_node = previous.layout->nodes[node_index];
        if (node.node_type != previous_node.node_type || node.state_size != previous_node.state_size) {
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
        size_t previous_index = next_region(*previous.layout, node_index, 0);

        while (current_index < layout->regions.size() && previous_index < previous.layout->regions.size()) {
            auto const& current_region = layout->regions[current_index];
            auto const& previous_region = previous.layout->regions[previous_index];

            if (current_region.owner_node != node_index || previous_region.owner_node != node_index) {
                break;
            }

            if (
                current_region.kind != previous_region.kind ||
                current_region.state_field_offset != previous_region.state_field_offset ||
                current_region.size != previous_region.size ||
                current_region.alignment != previous_region.alignment ||
                current_region.element_count != previous_region.element_count ||
                current_region.element_type != previous_region.element_type
            ) {
                return false;
            }

            current_index = next_region(*layout, node_index, current_index + 1);
            previous_index = next_region(*previous.layout, node_index, previous_index + 1);
        }

        return
            next_region(*layout, node_index, current_index) == layout->regions.size() &&
            next_region(*previous.layout, node_index, previous_index) == previous.layout->regions.size();
    }

    inline void NodeStorage::initialize(NodeStorage const* previous, ExecutionTargets* execution_targets)
    {
        if (!layout || !resources) {
            return;
        }

        constructed_nodes.clear();
        initialized_nodes.clear();
        for (size_t node_index = 0; node_index < layout->nodes.size(); ++node_index) {
            auto const& record = layout->nodes[node_index];
            if (record.state_size == 0) {
                continue;
            }

            void* state = state_ptr(node_index);
            if (
                previous &&
                can_move_from(*previous, node_index) &&
                record.lifecycle.move_construct_state_fn
            ) {
                record.lifecycle.move_construct_state_fn(state, previous->state_ptr(node_index));
            } else if (record.lifecycle.default_construct_state_fn) {
                record.lifecycle.default_construct_state_fn(state);
            }

            constructed_nodes.push_back(node_index);
        }

        for (auto const& region : layout->regions) {
            if (
                (region.kind != NodeLayout::Region::Kind::local_array &&
                 region.kind != NodeLayout::Region::Kind::nested_nodes) ||
                !region.assign_span_fn
            ) {
                continue;
            }
            void* state = state_ptr(region.owner_node);
            void* data = storage.get() + region.storage_offset;
            region.assign_span_fn(state, region.state_field_offset, data, region.element_count);
            if (region.kind == NodeLayout::Region::Kind::nested_nodes) {
                auto const& assigned_span = *reinterpret_cast<std::span<std::byte*> const*>(
                    static_cast<std::byte*>(state) + region.state_field_offset
                );
                if (assigned_span.size() != region.element_count) {
                    throw std::logic_error(
                        "nested node span assignment failed for owner node " + std::to_string(region.owner_node) +
                        " (expected count=" + std::to_string(region.element_count) +
                        ", actual count=" + std::to_string(assigned_span.size()) + ")"
                    );
                }
                auto* nested_nodes = static_cast<std::byte**>(data);
                for (size_t i = 0; i < region.nested_node_indices.size(); ++i) {
                    auto* nested_state = static_cast<std::byte*>(state_ptr(region.nested_node_indices[i]));
                    IV_ASSERT(
                        nested_state != nullptr,
                        "nested child state pointer must resolve during storage initialization"
                    );
                    nested_nodes[i] = nested_state;
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

        for (size_t node_index : layout->initialize_order) {
            auto const& record = layout->nodes[node_index];
            bool const moved_state =
                previous &&
                can_move_from(*previous, node_index) &&
                record.lifecycle.move_construct_state_fn != nullptr;

            if (previous && record.lifecycle.move_fn && can_move_from(*previous, node_index)) {
                record.lifecycle.move_fn(record.node, node_index, *this, *previous, execution_targets);
                auto& previous_initialized_nodes = const_cast<NodeStorage&>(*previous).initialized_nodes;
                previous_initialized_nodes.erase(
                    std::remove(previous_initialized_nodes.begin(), previous_initialized_nodes.end(), node_index),
                    previous_initialized_nodes.end()
                );
            } else {
                if (previous && previous->layout && node_index < previous->layout->nodes.size()) {
                    auto const& previous_record = previous->layout->nodes[node_index];
                    if (previous_record.lifecycle.release_fn) {
                        if (moved_state) {
                            previous_record.lifecycle.release_fn(previous_record.node, node_index, *this, execution_targets);
                        } else {
                            previous_record.lifecycle.release_fn(previous_record.node, node_index, const_cast<NodeStorage&>(*previous), execution_targets);
                        }
                    }
                    auto& previous_initialized_nodes = const_cast<NodeStorage&>(*previous).initialized_nodes;
                    previous_initialized_nodes.erase(
                        std::remove(previous_initialized_nodes.begin(), previous_initialized_nodes.end(), node_index),
                        previous_initialized_nodes.end()
                    );
                }
                if (record.lifecycle.initialize_fn) {
                    try {
                        record.lifecycle.initialize_fn(record.node, node_index, *this, execution_targets);
                    } catch (std::exception const& e) {
                        throw std::runtime_error(
                            "node initialize failed at index " + std::to_string(node_index) + ": " + e.what()
                        );
                    } catch (...) {
                        throw std::runtime_error(
                            "node initialize failed at index " + std::to_string(node_index)
                        );
                    }
                }
            }
            NodeLayoutBuilder::log_node_event(moved_state ? "moved" : "created", record, node_index);
            initialized_nodes.push_back(node_index);
        }
    }

    inline void NodeStorage::release(ExecutionTargets* execution_targets)
    {
        if (!layout || !resources) {
            return;
        }

        for (auto it = initialized_nodes.rbegin(); it != initialized_nodes.rend(); ++it) {
            size_t const node_index = *it;
            auto const& record = layout->nodes[node_index];
            if (record.lifecycle.release_fn) {
                record.lifecycle.release_fn(record.node, node_index, *this, execution_targets);
            }
        }
        initialized_nodes.clear();
    }

    template<typename Node>
    inline InitializationContext<Node>::InitializationContext(NodeStorage& storage, void* state, ResourceContext const& resources_, ExecutionTargets* execution_targets_)
    : _storage(&storage)
    , _state(state)
    , resources(resources_)
    , execution_targets(execution_targets_)
    {}

    template<typename Node>
    template<typename Node2>
    inline InitializationContext<Node>::InitializationContext(InitializationContext<Node2> const& ctx)
    : _storage(ctx._storage)
    , _state(ctx._state)
    , resources(ctx.resources)
    , execution_targets(ctx.execution_targets)
    {}

    template<typename Node>
    inline std::add_lvalue_reference_t<typename InitializationContext<Node>::State> InitializationContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        return *static_cast<State*>(_state);
    }

    template<typename Node>
    template<typename A>
    inline std::span<A const> InitializationContext<Node>::resolve_exported_array_storage(std::string const& id) const
    {
        return _storage->template resolve_exported_array_storage<A>(id);
    }

    template<typename Node>
    inline ReleaseContext<Node>::ReleaseContext(NodeStorage& storage, void* state, ResourceContext const& resources_, ExecutionTargets* execution_targets_)
    : _storage(&storage)
    , _state(state)
    , resources(resources_)
    , execution_targets(execution_targets_)
    {}

    template<typename Node>
    template<typename Node2>
    inline ReleaseContext<Node>::ReleaseContext(ReleaseContext<Node2> const& ctx)
    : _storage(ctx._storage)
    , _state(ctx._state)
    , resources(ctx.resources)
    , execution_targets(ctx.execution_targets)
    {}

    template<typename Node>
    inline std::add_lvalue_reference_t<typename ReleaseContext<Node>::State> ReleaseContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        return *static_cast<State*>(_state);
    }

    template<typename Node>
    inline MoveContext<Node>::MoveContext(
        NodeStorage& storage,
        void* state,
        NodeStorage const& previous_storage,
        void* previous_state,
        ResourceContext const& resources_,
        ExecutionTargets* execution_targets_
    )
    : _storage(&storage)
    , _state(state)
    , _previous_storage(&previous_storage)
    , _previous_state(previous_state)
    , resources(resources_)
    , execution_targets(execution_targets_)
    {}

    template<typename Node>
    template<typename Node2>
    inline MoveContext<Node>::MoveContext(MoveContext<Node2> const& ctx)
    : _storage(ctx._storage)
    , _state(ctx._state)
    , _previous_storage(ctx._previous_storage)
    , _previous_state(ctx._previous_state)
    , resources(ctx.resources)
    , execution_targets(ctx.execution_targets)
    {}

    template<typename Node>
    inline std::add_lvalue_reference_t<typename MoveContext<Node>::State> MoveContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        return *static_cast<State*>(_state);
    }

    template<typename Node>
    inline std::add_lvalue_reference_t<typename MoveContext<Node>::State> MoveContext<Node>::previous_state() const
    requires(!std::is_void_v<State>)
    {
        return *static_cast<State*>(_previous_state);
    }
}
