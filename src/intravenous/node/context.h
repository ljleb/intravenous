#pragma once

#include <intravenous/compat.h>
#include <intravenous/node/resources.h>
#include <intravenous/node/traits.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace iv {
    struct NodeLayoutBuilder;
    struct NodeStorage;
    struct NodeStateStructure;

    struct NodeLifecycleCallbacks {
        void (*move_fn)(
            void const*, size_t, size_t, NodeStorage&, NodeStorage const&) = nullptr;
        void (*initialize_fn)(void const*, size_t, NodeStorage&) = nullptr;
        void (*release_fn)(void const*, size_t, NodeStorage&) = nullptr;
        void (*default_construct_state_fn)(void*) = nullptr;
        void (*destroy_state_fn)(void*) = nullptr;
        std::string (*identity_fn)(void const*) = nullptr;
    };

    namespace details {
        struct NodeLayoutNodeRegistration {
            void const* node = nullptr;
            void const* node_type = nullptr;
            char const* node_type_name = nullptr;
            size_t state_size = 0;
            size_t state_alignment = 1;
            bool has_state = false;
            NodeLifecycleCallbacks lifecycle {};
        };

        struct NodeLayoutArrayDeclaration {
            size_t owner_node = 0;
            ptrdiff_t state_field_offset = 0;
            void const* element_type = nullptr;
            char const* element_type_name = nullptr;
            size_t element_size = 0;
            size_t element_alignment = 1;
            size_t element_count = 0;
            void (*assign_span_fn)(
                void* state_base, ptrdiff_t field_offset, void* data,
                size_t count) = nullptr;
            void (*read_span_fn)(
                void const* state_base, ptrdiff_t field_offset, void*& data,
                size_t& count) = nullptr;
        };

        struct NodeLayoutArrayStorage {
            void const* data = nullptr;
            size_t count = 0;
        };

        template<typename A>
        void const* node_layout_type_token()
        {
            static int token = 0;
            return &token;
        }

        size_t register_node(
            NodeLayoutBuilder&, NodeLayoutNodeRegistration const&);
        void allocate_node_state(
            NodeLayoutBuilder&, size_t node_index, size_t size, size_t alignment);
        void declare_local_array(
            NodeLayoutBuilder&, NodeLayoutArrayDeclaration const&);
        size_t declare_nested_node_states(
            NodeLayoutBuilder&, size_t node_index, ptrdiff_t state_field_offset);
        void finalize_nested_node_states(
            NodeLayoutBuilder&, size_t region_index,
            std::vector<size_t> nested_node_indices);
        void declare_export_array(
            NodeLayoutBuilder&, std::string id,
            NodeLayoutArrayDeclaration const&);
        void declare_import_array(
            NodeLayoutBuilder&, std::string id,
            NodeLayoutArrayDeclaration const&);
        void require_export_array(
            NodeLayoutBuilder&, size_t node_index, std::string id,
            void const* element_type, size_t element_size);
        bool has_import_array(
            NodeLayoutBuilder const&, std::string const&, void const* element_type);
        bool has_export_array(
            NodeLayoutBuilder const&, std::string const&, void const* element_type);
        void override_node_state_structure(
            NodeLayoutBuilder&, size_t node_index, NodeStateStructure const&);
        size_t node_layout_max_block_size(NodeLayoutBuilder const&);
        size_t node_layout_event_port_buffer_base_multiplier(
            NodeLayoutBuilder const&);

        void* node_storage_state_ptr(NodeStorage const&, size_t node_index);
        ResourceContext const& node_storage_resources(NodeStorage const&);
        size_t node_storage_max_block_size(NodeStorage const&);
        size_t node_storage_default_silence_ttl_samples(NodeStorage const&);
        NodeLayoutArrayStorage resolve_exported_array_storage(
            NodeStorage const&, std::string const&, void const* element_type);

    }

    template<typename Node>
    struct DeclarationContext;

    template<typename Node>
    struct InitializationContext;

    template<typename Node>
    struct ReleaseContext;

    template<typename Node>
    struct MoveContext;

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

        template<typename Node>
        NodeLifecycleCallbacks make_lifecycle_callbacks();

        template<auto NodeValue>
        NodeLifecycleCallbacks make_reflected_lifecycle_callbacks();

        template<typename Node>
        NodeLayoutNodeRegistration make_node_registration(Node const& node)
        {
            NodeLayoutNodeRegistration registration;
            if constexpr (!std::is_empty_v<Node>) {
                registration.node = std::addressof(node);
            }
            registration.node_type = node_layout_type_token<Node>();
            registration.node_type_name = typeid(Node).name();
            registration.has_state = !std::is_void_v<typename NodeState<Node>::Type>;
            if constexpr (!std::is_void_v<typename NodeState<Node>::Type>) {
                using State = typename NodeState<Node>::Type;
                registration.state_size = sizeof(State);
                registration.state_alignment = alignof(State);
            }
            registration.lifecycle = make_lifecycle_callbacks<Node>();
            return registration;
        }

        template<auto NodeValue>
        NodeLayoutNodeRegistration make_reflected_node_registration()
        {
            using Node = std::remove_cvref_t<decltype(NodeValue)>;
            NodeLayoutNodeRegistration registration;
            registration.node_type = node_layout_type_token<Node>();
            registration.node_type_name = typeid(Node).name();
            registration.has_state = !std::is_void_v<typename NodeState<Node>::Type>;
            if constexpr (!std::is_void_v<typename NodeState<Node>::Type>) {
                using State = typename NodeState<Node>::Type;
                registration.state_size = sizeof(State);
                registration.state_alignment = alignof(State);
            }
            registration.lifecycle = make_reflected_lifecycle_callbacks<NodeValue>();
            return registration;
        }

        inline ptrdiff_t node_layout_field_offset(
            void const* state_base, void const* field)
        {
            auto const base = reinterpret_cast<uintptr_t>(state_base);
            auto const field_address = reinterpret_cast<uintptr_t>(field);
            return static_cast<ptrdiff_t>(field_address - base);
        }
    }

    template<typename Node>
    struct DeclarationContext
    {
        template<typename>
        friend struct DeclarationContext;

        using State = typename NodeState<Node>::Type;

    private:
        NodeLayoutBuilder* _builder = nullptr;
        size_t _node_index = 0;
        State const* _state_marker = nullptr;
        mutable std::vector<size_t> _direct_nested_node_indices;
        mutable std::optional<size_t> _nested_nodes_region_index;

    public:
        explicit DeclarationContext(NodeLayoutBuilder& builder, Node const& node)
            : _builder(&builder)
            , _node_index(details::register_node(
                builder, details::make_node_registration(node)))
        {
            if constexpr (!std::is_void_v<State>) {
                details::allocate_node_state(
                    builder, _node_index, sizeof(State), alignof(State));
                _state_marker = reinterpret_cast<State const*>(uintptr_t { 0x10000 });
            }
        }

        template<auto NodeValue>
            requires std::same_as<
                std::remove_cvref_t<decltype(NodeValue)>,
                Node>
        explicit DeclarationContext(
            NodeLayoutBuilder& builder,
            std::integral_constant<decltype(NodeValue), NodeValue>)
            : _builder(&builder)
            , _node_index(details::register_node(
                builder, details::make_reflected_node_registration<NodeValue>()))
        {
            if constexpr (!std::is_void_v<State>) {
                details::allocate_node_state(
                    builder, _node_index, sizeof(State), alignof(State));
                _state_marker = reinterpret_cast<State const*>(uintptr_t { 0x10000 });
            }
        }

        ~DeclarationContext()
        {
            if (_nested_nodes_region_index) {
                details::finalize_nested_node_states(
                    *_builder,
                    *_nested_nodes_region_index,
                    std::move(_direct_nested_node_indices));
            }
        }

        template<typename Node2>
        DeclarationContext(
            DeclarationContext<Node2> const& ctx, Node const& node)
            : DeclarationContext<Node>(*ctx._builder, node)
        {
            ctx._direct_nested_node_indices.push_back(_node_index);
        }

        NoCopy<State> const& state() const
        requires(!std::is_void_v<State>)
        {
            return reinterpret_cast<NoCopy<State> const&>(*_state_marker);
        }

        template<typename A>
        void local_array(std::span<A> const& span, size_t count) const
        {
            details::declare_local_array(*_builder, {
                .owner_node = _node_index,
                .state_field_offset =
                    details::node_layout_field_offset(_state_marker, &span),
                .element_type = details::node_layout_type_token<A>(),
                .element_type_name = typeid(A).name(),
                .element_size = sizeof(A),
                .element_alignment = alignof(A),
                .element_count = count,
                .assign_span_fn = [](void* state_base, ptrdiff_t field_offset,
                                     void* data, size_t count_value) {
                    auto& span_ref = *reinterpret_cast<std::span<A>*>(
                        static_cast<std::byte*>(state_base) + field_offset);
                    span_ref = { static_cast<A*>(data), count_value };
                },
            });
        }

        template<typename A>
        void export_array(std::string id, std::span<A> const& span) const
        {
            details::declare_export_array(*_builder, std::move(id), {
                .owner_node = _node_index,
                .state_field_offset = details::node_layout_field_offset(
                    reinterpret_cast<void const*>(uintptr_t { 0x10000 }), &span),
                .element_type = details::node_layout_type_token<A>(),
                .element_type_name = typeid(A).name(),
                .element_size = sizeof(A),
                .element_alignment = alignof(A),
                .read_span_fn = [](void const* state_base,
                                   ptrdiff_t field_offset, void*& data,
                                   size_t& count_value) {
                    auto const& span_ref = *reinterpret_cast<std::span<A> const*>(
                        static_cast<std::byte const*>(state_base) + field_offset);
                    data = span_ref.data();
                    count_value = span_ref.size();
                },
            });
        }

        template<typename A>
        void import_array(std::string id, std::span<A> const& span) const
        {
            details::declare_import_array(*_builder, std::move(id), {
                .owner_node = _node_index,
                .state_field_offset = details::node_layout_field_offset(
                    reinterpret_cast<void const*>(uintptr_t { 0x10000 }), &span),
                .element_type = details::node_layout_type_token<A>(),
                .element_type_name = typeid(A).name(),
                .element_size = sizeof(A),
                .element_alignment = alignof(A),
                .assign_span_fn = [](void* state_base, ptrdiff_t field_offset,
                                     void* data, size_t count_value) {
                    auto& span_ref = *reinterpret_cast<std::span<A>*>(
                        static_cast<std::byte*>(state_base) + field_offset);
                    span_ref = { static_cast<A*>(data), count_value };
                },
            });
        }

        template<typename A>
        void require_export_array(std::string id) const
        {
            details::require_export_array(
                *_builder,
                _node_index,
                std::move(id),
                details::node_layout_type_token<A>(),
                sizeof(A));
        }

        template<typename A>
        bool has_import_array(std::string const& id) const
        {
            return details::has_import_array(
                *_builder, id, details::node_layout_type_token<A>());
        }

        template<typename A>
        bool has_export_array(std::string const& id) const
        {
            return details::has_export_array(
                *_builder, id, details::node_layout_type_token<A>());
        }

        void nested_node_states(
            std::span<std::span<std::byte>> const& nodes) const
        {
            IV_ASSERT(
                !_nested_nodes_region_index.has_value(),
                "nested_node_states must only be declared once per node");
            _nested_nodes_region_index = details::declare_nested_node_states(
                *_builder,
                _node_index,
                details::node_layout_field_offset(_state_marker, &nodes));
        }

        void declare_reflected_child(
            void const* node_data,
            NodeStateStructure const* state_structure,
            size_t (*declare)(
                void const*, NodeStateStructure const*, NodeLayoutBuilder&)) const
        {
            IV_ASSERT(node_data, "reflected child node data cannot be null");
            IV_ASSERT(
                declare, "reflected child declaration callback cannot be null");
            _direct_nested_node_indices.push_back(
                declare(node_data, state_structure, *_builder));
        }

        size_t max_block_size() const
        {
            return details::node_layout_max_block_size(*_builder);
        }

        size_t event_port_buffer_base_multiplier() const
        {
            return details::node_layout_event_port_buffer_base_multiplier(
                *_builder);
        }

        size_t node_index() const
        {
            return _node_index;
        }

        size_t pending_direct_nested_node_count() const
        {
            return _direct_nested_node_indices.size();
        }
    };

    template<typename Node>
    struct InitializationContext {
        template<typename>
        friend struct InitializationContext;

        using State = typename NodeState<Node>::Type;

    private:
        NodeStorage* _storage = nullptr;
        void* _state = nullptr;

    public:
        ResourceContext const& resources;

        explicit InitializationContext(
            NodeStorage& storage, void* state, ResourceContext const& resources_)
            : _storage(&storage)
            , _state(state)
            , resources(resources_)
        {}

        template<typename Node2>
        InitializationContext(InitializationContext<Node2> const& ctx)
            : _storage(ctx._storage)
            , _state(ctx._state)
            , resources(ctx.resources)
        {}

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>)
        {
            return *static_cast<State*>(_state);
        }

        NodeStorage& storage() const
        {
            return *_storage;
        }

        size_t max_block_size() const
        {
            return details::node_storage_max_block_size(*_storage);
        }

        size_t default_silence_ttl_samples() const
        {
            return details::node_storage_default_silence_ttl_samples(*_storage);
        }

        template<typename A>
        std::span<A const> resolve_exported_array_storage(
            std::string const& id) const
        {
            auto const storage = details::resolve_exported_array_storage(
                *_storage, id, details::node_layout_type_token<A>());
            return { static_cast<A const*>(storage.data), storage.count };
        }
    };

    template<typename Node>
    struct ReleaseContext {
        template<typename>
        friend struct ReleaseContext;

        using State = typename NodeState<Node>::Type;

    private:
        NodeStorage* _storage = nullptr;
        void* _state = nullptr;

    public:
        ResourceContext const& resources;

        explicit ReleaseContext(
            NodeStorage& storage, void* state, ResourceContext const& resources_)
            : _storage(&storage)
            , _state(state)
            , resources(resources_)
        {}

        template<typename Node2>
        ReleaseContext(ReleaseContext<Node2> const& ctx)
            : _storage(ctx._storage)
            , _state(ctx._state)
            , resources(ctx.resources)
        {}

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>)
        {
            return *static_cast<State*>(_state);
        }
    };

    template<typename Node>
    struct MoveContext {
        template<typename>
        friend struct MoveContext;

        using State = typename NodeState<Node>::Type;

    private:
        NodeStorage* _storage = nullptr;
        void* _state = nullptr;
        NodeStorage const* _previous_storage = nullptr;
        void* _previous_state = nullptr;

    public:
        ResourceContext const& resources;

        explicit MoveContext(
            NodeStorage& storage,
            void* state,
            NodeStorage const& previous_storage,
            void* previous_state,
            ResourceContext const& resources_)
            : _storage(&storage)
            , _state(state)
            , _previous_storage(&previous_storage)
            , _previous_state(previous_state)
            , resources(resources_)
        {}

        template<typename Node2>
        MoveContext(MoveContext<Node2> const& ctx)
            : _storage(ctx._storage)
            , _state(ctx._state)
            , _previous_storage(ctx._previous_storage)
            , _previous_state(ctx._previous_state)
            , resources(ctx.resources)
        {}

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>)
        {
            return *static_cast<State*>(_state);
        }

        std::add_lvalue_reference_t<State> previous_state() const
        requires(!std::is_void_v<State>)
        {
            return *static_cast<State*>(_previous_state);
        }
    };

    namespace details {
        template<typename Node>
        NodeLifecycleCallbacks make_lifecycle_callbacks()
        {
            NodeLifecycleCallbacks callbacks;

            if constexpr (!std::is_void_v<typename NodeState<Node>::Type>) {
                using State = typename NodeState<Node>::Type;
                static_assert(
                    std::is_default_constructible_v<State>,
                    "Node::State must be default constructible");
                callbacks.default_construct_state_fn = [](void* ptr) {
                    new (ptr) State();
                };
                callbacks.destroy_state_fn = [](void* ptr) {
                    std::destroy_at(static_cast<State*>(ptr));
                };
            }

            if constexpr (has_move<Node>) {
                callbacks.move_fn = [](
                    void const* node_ptr,
                    size_t node_index,
                    size_t previous_node_index,
                    NodeStorage& storage,
                    NodeStorage const& previous_storage) {
                    void* state = node_storage_state_ptr(storage, node_index);
                    void* previous_state =
                        node_storage_state_ptr(previous_storage, previous_node_index);
                    if constexpr (std::is_empty_v<Node>) {
                        (void)node_ptr;
                        Node node {};
                        node.move(MoveContext<Node>(
                            storage,
                            state,
                            previous_storage,
                            previous_state,
                            node_storage_resources(storage)));
                    } else {
                        auto const& node = *static_cast<Node const*>(node_ptr);
                        node.move(MoveContext<Node>(
                            storage,
                            state,
                            previous_storage,
                            previous_state,
                            node_storage_resources(storage)));
                    }
                };
            }

            if constexpr (requires(Node const& node) {
                { node.identity() } -> std::convertible_to<std::string>;
            }) {
                callbacks.identity_fn = [](void const* node_ptr) -> std::string {
                    if constexpr (std::is_empty_v<Node>) {
                        (void)node_ptr;
                        Node node {};
                        return std::string(node.identity());
                    } else {
                        return std::string(
                            static_cast<Node const*>(node_ptr)->identity());
                    }
                };
            }

            if constexpr (has_initialize<Node>) {
                callbacks.initialize_fn = [](
                    void const* node_ptr, size_t node_index, NodeStorage& storage) {
                    void* state = node_storage_state_ptr(storage, node_index);
                    if constexpr (std::is_empty_v<Node>) {
                        (void)node_ptr;
                        Node node {};
                        node.initialize(InitializationContext<Node>(
                            storage, state, node_storage_resources(storage)));
                    } else {
                        static_cast<Node const*>(node_ptr)->initialize(
                            InitializationContext<Node>(
                                storage, state, node_storage_resources(storage)));
                    }
                };
            }

            if constexpr (has_release<Node>) {
                callbacks.release_fn = [](
                    void const* node_ptr, size_t node_index, NodeStorage& storage) {
                    void* state = node_storage_state_ptr(storage, node_index);
                    if constexpr (std::is_empty_v<Node>) {
                        (void)node_ptr;
                        Node node {};
                        node.release(ReleaseContext<Node>(
                            storage, state, node_storage_resources(storage)));
                    } else {
                        static_cast<Node const*>(node_ptr)->release(
                            ReleaseContext<Node>(
                                storage, state, node_storage_resources(storage)));
                    }
                };
            }

            return callbacks;
        }

        template<auto NodeValue>
        NodeLifecycleCallbacks make_reflected_lifecycle_callbacks()
        {
            using Node = std::remove_cvref_t<decltype(NodeValue)>;
            NodeLifecycleCallbacks callbacks;

            if constexpr (!std::is_void_v<typename NodeState<Node>::Type>) {
                using State = typename NodeState<Node>::Type;
                static_assert(
                    std::is_default_constructible_v<State>,
                    "Node::State must be default constructible");
                callbacks.default_construct_state_fn = [](void* ptr) {
                    new (ptr) State();
                };
                callbacks.destroy_state_fn = [](void* ptr) {
                    std::destroy_at(static_cast<State*>(ptr));
                };
            }

            if constexpr (requires(MoveContext<Node> ctx) {
                NodeValue.move(ctx);
            }) {
                callbacks.move_fn = [](
                    void const*,
                    size_t node_index,
                    size_t previous_node_index,
                    NodeStorage& storage,
                    NodeStorage const& previous_storage) {
                    NodeValue.move(MoveContext<Node>(
                        storage,
                        node_storage_state_ptr(storage, node_index),
                        previous_storage,
                        node_storage_state_ptr(
                            previous_storage, previous_node_index),
                        node_storage_resources(storage)));
                };
            }

            if constexpr (requires {
                { NodeValue.identity() } -> std::convertible_to<std::string>;
            }) {
                callbacks.identity_fn = [](void const*) -> std::string {
                    return std::string(NodeValue.identity());
                };
            }

            if constexpr (requires(InitializationContext<Node> ctx) {
                NodeValue.initialize(ctx);
            }) {
                callbacks.initialize_fn = [](
                    void const*, size_t node_index, NodeStorage& storage) {
                    NodeValue.initialize(InitializationContext<Node>(
                        storage,
                        node_storage_state_ptr(storage, node_index),
                        node_storage_resources(storage)));
                };
            }

            if constexpr (requires(ReleaseContext<Node> ctx) {
                NodeValue.release(ctx);
            }) {
                callbacks.release_fn = [](
                    void const*, size_t node_index, NodeStorage& storage) {
                    NodeValue.release(ReleaseContext<Node>(
                        storage,
                        node_storage_state_ptr(storage, node_index),
                        node_storage_resources(storage)));
                };
            }

            return callbacks;
        }
    }
}
