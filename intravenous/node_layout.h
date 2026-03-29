#pragma once

#include "node_traits.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <type_traits>
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

    struct ResourceContext {
        struct VstResources {
            template<typename Descriptor>
            void* create(Descriptor const& descriptor) const
            {
                (void)descriptor;
                return nullptr;
            }
        };

        VstResources const& vst;
    };

    struct NodeLayout;
    struct NodeStorage;

    struct NodeLifecycleCallbacks {
        void (*initialize_fn)(void const*, size_t, NodeStorage&) = nullptr;
        void (*release_fn)(void const*, size_t, NodeStorage&) = nullptr;
        void (*construct_state_fn)(void*) = nullptr;
        void (*destroy_state_fn)(void*) = nullptr;
    };

    struct NodeLayout {
        struct Region {
            enum class Kind {
                state,
                local_array,
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
        };

        struct ArrayBinding {
            size_t owner_node = 0;
            std::string id;
            ptrdiff_t state_field_offset = 0;
            void const* element_type = nullptr;
            void (*assign_span_fn)(void* state_base, ptrdiff_t field_offset, void* data, size_t count) = nullptr;
            void (*read_span_fn)(void const* state_base, ptrdiff_t field_offset, void*& data, size_t& count) = nullptr;
        };

        struct NodeRecord {
            std::shared_ptr<void> node;
            ptrdiff_t state_offset = 0;
            size_t state_size = 0;
            std::vector<size_t> dependencies;
            NodeLifecycleCallbacks lifecycle;
        };

        size_t storage_size = 0;
        size_t storage_alignment = 1;
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

        template<typename A>
        void export_array(size_t node_index, std::string id, std::span<A> const*);

        template<typename A>
        void import_array(size_t node_index, std::string id, std::span<A> const*);

        template<typename A>
        bool has_import_array(std::string const& id) const;

        template<typename A>
        bool has_export_array(std::string const& id) const;

        explicit NodeLayoutBuilder(size_t max_block_size = 1)
        : _max_block_size(max_block_size)
        {}

        size_t max_block_size() const;

        NodeLayout build() &&;

    private:
        size_t _max_block_size = 1;
        size_t _storage_size = 0;
        size_t _storage_alignment = 1;
        std::unordered_map<void const*, size_t> _node_indices;
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
        std::vector<size_t> initialized_nodes;

        NodeStorage() = default;
        NodeStorage(NodeLayout const& layout, ResourceContext const& resources);
        NodeStorage(NodeStorage&& other) noexcept;
        NodeStorage& operator=(NodeStorage&& other) noexcept;
        NodeStorage(NodeStorage const&) = delete;
        NodeStorage& operator=(NodeStorage const&) = delete;
        ~NodeStorage();

        std::span<std::byte> buffer() const;
        void* state_ptr(size_t node_index) const;
        void initialize();
        void release();
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
        bool has_import_array(std::string const& id) const;

        template<typename A>
        bool has_export_array(std::string const& id) const;

        size_t max_block_size() const;
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

        explicit InitializationContext(NodeStorage& storage, void* state, ResourceContext const& resources);

        template<typename Node2>
        InitializationContext(InitializationContext<Node2> const& ctx);

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>);
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

        explicit ReleaseContext(NodeStorage& storage, void* state, ResourceContext const& resources);

        template<typename Node2>
        ReleaseContext(ReleaseContext<Node2> const& ctx);

        std::add_lvalue_reference_t<State> state() const
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
    {}

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
    inline size_t DeclarationContext<Node>::max_block_size() const
    {
        return _builder->max_block_size();
    }

    template<typename Node>
    size_t NodeLayoutBuilder::register_node(Node const& node)
    {
        auto const key = static_cast<void const*>(std::addressof(node));
        if (auto it = _node_indices.find(key); it != _node_indices.end()) {
            return it->second;
        }

        size_t const node_index = _nodes.size();
        _node_indices.emplace(key, node_index);

        NodeLayout::NodeRecord record;
        if constexpr (!std::is_empty_v<Node>) {
            record.node = std::make_shared<Node>(node);
        }
        record.lifecycle = make_lifecycle_callbacks<Node>();
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
            .assign_span_fn = [](void* state_base_ptr, ptrdiff_t offset, void* data, size_t count_value) {
                auto& span_ref = *reinterpret_cast<std::span<A>*>(static_cast<std::byte*>(state_base_ptr) + offset);
                span_ref = { static_cast<A*>(data), count_value };
            },
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
            callbacks.construct_state_fn = [](void* ptr) {
                new (ptr) State();
            };
            callbacks.destroy_state_fn = [](void* ptr) {
                std::destroy_at(static_cast<State*>(ptr));
            };
        }

        if constexpr (requires(Node const& node, InitializationContext<Node> ctx) { node.initialize(ctx); }) {
            callbacks.initialize_fn = [](void const* node_ptr, size_t node_index, NodeStorage& storage) {
                void* state = storage.state_ptr(node_index);
                if constexpr (std::is_empty_v<Node>) {
                    Node node {};
                    InitializationContext<Node> ctx(storage, state, *storage.resources);
                    node.initialize(ctx);
                } else {
                    auto const& node = *static_cast<Node const*>(node_ptr);
                    InitializationContext<Node> ctx(storage, state, *storage.resources);
                    node.initialize(ctx);
                }
            };
        }

        if constexpr (requires(Node const& node, ReleaseContext<Node> ctx) { node.release(ctx); }) {
            callbacks.release_fn = [](void const* node_ptr, size_t node_index, NodeStorage& storage) {
                void* state = storage.state_ptr(node_index);
                if constexpr (std::is_empty_v<Node>) {
                    Node node {};
                    ReleaseContext<Node> ctx(storage, state, *storage.resources);
                    node.release(ctx);
                } else {
                    auto const& node = *static_cast<Node const*>(node_ptr);
                    ReleaseContext<Node> ctx(storage, state, *storage.resources);
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

        for (size_t node_index = 0; node_index < layout->nodes.size(); ++node_index) {
            auto const& node = layout->nodes[node_index];
            if (node.state_size != 0 && node.lifecycle.construct_state_fn) {
                node.lifecycle.construct_state_fn(state_ptr(node_index));
            }
        }

        for (auto const& region : layout->regions) {
            if (region.kind != NodeLayout::Region::Kind::local_array || !region.assign_span_fn) {
                continue;
            }
            void* state = state_ptr(region.owner_node);
            void* data = storage.get() + region.storage_offset;
            region.assign_span_fn(state, region.state_field_offset, data, region.element_count);
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
    }

    inline NodeStorage::NodeStorage(NodeStorage&& other) noexcept
    : layout(other.layout)
    , resources(other.resources)
    , storage(std::move(other.storage))
    , initialized_nodes(std::move(other.initialized_nodes))
    {
        other.layout = nullptr;
        other.resources = nullptr;
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
        initialized_nodes = std::move(other.initialized_nodes);

        other.layout = nullptr;
        other.resources = nullptr;
        other.initialized_nodes.clear();
        return *this;
    }

    inline NodeStorage::~NodeStorage()
    {
        release();
        if (layout) {
            for (auto it = layout->initialize_order.rbegin(); it != layout->initialize_order.rend(); ++it) {
                auto const& node = layout->nodes[*it];
                if (node.state_size != 0 && node.lifecycle.destroy_state_fn) {
                    node.lifecycle.destroy_state_fn(state_ptr(*it));
                }
            }
        }
    }

    inline std::span<std::byte> NodeStorage::buffer() const
    {
        return { storage.get(), layout ? layout->storage_size : 0 };
    }

    inline void* NodeStorage::state_ptr(size_t node_index) const
    {
        if (!layout || node_index >= layout->nodes.size() || layout->nodes[node_index].state_size == 0) {
            return nullptr;
        }
        return storage.get() + layout->nodes[node_index].state_offset;
    }

    inline void NodeStorage::initialize()
    {
        if (!layout || !resources) {
            return;
        }

        initialized_nodes.clear();
        for (size_t node_index : layout->initialize_order) {
            auto const& record = layout->nodes[node_index];
            if (record.lifecycle.initialize_fn) {
                record.lifecycle.initialize_fn(record.node.get(), node_index, *this);
            }
            initialized_nodes.push_back(node_index);
        }
    }

    inline void NodeStorage::release()
    {
        if (!layout || !resources) {
            return;
        }

        for (auto it = initialized_nodes.rbegin(); it != initialized_nodes.rend(); ++it) {
            size_t const node_index = *it;
            auto const& record = layout->nodes[node_index];
            if (record.lifecycle.release_fn) {
                record.lifecycle.release_fn(record.node.get(), node_index, *this);
            }
        }
        initialized_nodes.clear();
    }

    template<typename Node>
    inline InitializationContext<Node>::InitializationContext(NodeStorage& storage, void* state, ResourceContext const& resources_)
    : _storage(&storage)
    , _state(state)
    , resources(resources_)
    {}

    template<typename Node>
    template<typename Node2>
    inline InitializationContext<Node>::InitializationContext(InitializationContext<Node2> const& ctx)
    : _storage(ctx._storage)
    , _state(ctx._state)
    , resources(ctx.resources)
    {}

    template<typename Node>
    inline std::add_lvalue_reference_t<typename InitializationContext<Node>::State> InitializationContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        return *static_cast<State*>(_state);
    }

    template<typename Node>
    inline ReleaseContext<Node>::ReleaseContext(NodeStorage& storage, void* state, ResourceContext const& resources_)
    : _storage(&storage)
    , _state(state)
    , resources(resources_)
    {}

    template<typename Node>
    template<typename Node2>
    inline ReleaseContext<Node>::ReleaseContext(ReleaseContext<Node2> const& ctx)
    : _storage(ctx._storage)
    , _state(ctx._state)
    , resources(ctx.resources)
    {}

    template<typename Node>
    inline std::add_lvalue_reference_t<typename ReleaseContext<Node>::State> ReleaseContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        return *static_cast<State*>(_state);
    }
}
