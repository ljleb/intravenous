#pragma once

#include "basic_nodes/type_erased.h"
#include "execution_targets.h"
#include "node_lifecycle.h"
#include "module/loader.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {
    class NodeExecutor {
        struct PreparedState {
            TypeErasedNode root;
            ResourceContext resources;
            std::unique_ptr<EventStreamStorage> event_stream_storage;
            NodeLayout layout;
            NodeStorage storage;
            size_t max_block_size = 0;
        };

        std::vector<ModuleRef> _module_refs;
        TypeErasedNode _root;
        ResourceContext _resources;
        std::unique_ptr<EventStreamStorage> _event_stream_storage;
        ExecutionTargetRegistry* _execution_target_registry = nullptr;
        NodeLayout _layout;
        NodeStorage _storage;
        size_t _max_block_size;
        size_t _default_silence_ttl_samples = std::numeric_limits<size_t>::max();
        size_t _executor_id = 0;
        bool _shutdown_requested = false;

        void rebind_storage_metadata()
        {
            _storage.layout = &_layout;
            _storage.resources = &_resources;
            _resources.event_streams = _event_stream_storage.get();
        }

        static void validate_root_interface(TypeErasedNode const& root)
        {
            if (get_num_inputs(root) != 0 || get_num_outputs(root) != 0) {
                throw std::logic_error("NodeExecutor root must have 0 public inputs and outputs");
            }
        }

        static size_t choose_block_size(TypeErasedNode const& root, ExecutionTargetRegistry const& execution_target_registry, size_t executor_id)
        {
            size_t block_size = std::min(root.max_block_size(), execution_target_registry.preferred_block_size_hint());
            execution_target_registry.for_each_target(executor_id, [&](auto const& target) {
                block_size = std::min(block_size, target.preferred_block_size());
            });
            block_size = std::bit_floor(block_size);
            if (block_size == 0) {
                throw std::logic_error("NodeExecutor max block size must be non-zero");
            }
            validate_max_block_size(block_size, "NodeExecutor max block size must be a power of 2");
            return block_size;
        }

        static NodeLayout make_layout(TypeErasedNode const& root, size_t max_block_size, size_t default_silence_ttl_samples)
        {
            NodeLayoutBuilder builder(max_block_size, default_silence_ttl_samples);
            {
                DeclarationContext<TypeErasedNode> ctx(builder, root);
                root.declare(ctx);
            }
            return std::move(builder).build();
        }

        static PreparedState prepare_state(
            TypeErasedNode root,
            ResourceContext resources,
            ExecutionTargetRegistry& execution_target_registry,
            size_t executor_id,
            char const* operation,
            bool initialize_storage = true,
            EventStreamStorage* event_stream_storage = nullptr,
            size_t default_silence_ttl_samples = std::numeric_limits<size_t>::max()
        )
        {
            validate_root_interface(root);

            PreparedState prepared;
            prepared.root = std::move(root);
            prepared.resources = std::move(resources);
            if (event_stream_storage) {
                prepared.resources.event_streams = event_stream_storage;
            } else {
                prepared.event_stream_storage = std::make_unique<EventStreamStorage>();
                prepared.resources.event_streams = prepared.event_stream_storage.get();
            }
            prepared.max_block_size = choose_block_size(prepared.root, execution_target_registry, executor_id);
            try {
                prepared.layout = make_layout(
                    prepared.root,
                    prepared.max_block_size,
                    default_silence_ttl_samples
                );
            } catch (std::exception const& e) {
                throw std::runtime_error(std::string("failed to ") + operation + " node executor: make_layout: " + e.what());
            } catch (...) {
                throw std::runtime_error(std::string("failed to ") + operation + " node executor: make_layout");
            }

            try {
                prepared.storage = prepared.layout.create_storage(prepared.resources);
            } catch (std::exception const& e) {
                throw std::runtime_error(std::string("failed to ") + operation + " node executor: create_storage: " + e.what());
            } catch (...) {
                throw std::runtime_error(std::string("failed to ") + operation + " node executor: create_storage");
            }

            if (initialize_storage) {
                try {
                    prepared.storage.initialize(nullptr, &execution_target_registry, executor_id);
                } catch (std::exception const& e) {
                    throw std::runtime_error(std::string("failed to ") + operation + " node executor: initialize_storage: " + e.what());
                } catch (...) {
                    throw std::runtime_error(std::string("failed to ") + operation + " node executor: initialize_storage");
                }
            }

            return prepared;
        }

        void run_block(size_t index, size_t block_size)
        {
            try {
                _root.tick_block({
                    TickContext<TypeErasedNode> {
                        .inputs = {},
                        .outputs = {},
                        .event_inputs = {},
                        .event_outputs = {},
                        .event_streams = _event_stream_storage.get(),
                        .buffer = _storage.buffer(),
                    },
                    index,
                    block_size
                });
            } catch (std::exception const& e) {
                _execution_target_registry->request_shutdown(_executor_id);
                throw std::runtime_error(std::string("node executor tick failed: ") + e.what());
            } catch (...) {
                _execution_target_registry->request_shutdown(_executor_id);
                throw std::runtime_error("node executor tick failed");
            }
        }

    public:
        ~NodeExecutor()
        {
            try {
                _storage.release(_execution_target_registry, _executor_id);
            } catch (...) {
            }
            if (_execution_target_registry) {
                _execution_target_registry->unregister_executor(_executor_id);
            }
        }

        NodeExecutor(NodeExecutor&& other) noexcept
        : _module_refs(std::move(other._module_refs))
        , _root(std::move(other._root))
        , _resources(std::move(other._resources))
        , _event_stream_storage(std::move(other._event_stream_storage))
        , _execution_target_registry(other._execution_target_registry)
        , _layout(std::move(other._layout))
        , _storage(std::move(other._storage))
        , _max_block_size(other._max_block_size)
        , _default_silence_ttl_samples(other._default_silence_ttl_samples)
        , _executor_id(other._executor_id)
        , _shutdown_requested(other._shutdown_requested)
        {
            other._execution_target_registry = nullptr;
            other._executor_id = 0;
            rebind_storage_metadata();
        }

        NodeExecutor& operator=(NodeExecutor&& other) noexcept
        {
            if (this == &other) {
                return *this;
            }

            try {
                _storage.release(_execution_target_registry, _executor_id);
            } catch (...) {
            }
            if (_execution_target_registry) {
                _execution_target_registry->unregister_executor(_executor_id);
            }

            _module_refs = std::move(other._module_refs);
            _root = std::move(other._root);
            _resources = std::move(other._resources);
            _event_stream_storage = std::move(other._event_stream_storage);
            _execution_target_registry = other._execution_target_registry;
            _layout = std::move(other._layout);
            _storage = std::move(other._storage);
            _max_block_size = other._max_block_size;
            _default_silence_ttl_samples = other._default_silence_ttl_samples;
            _executor_id = other._executor_id;
            _shutdown_requested = other._shutdown_requested;
            other._execution_target_registry = nullptr;
            other._executor_id = 0;
            rebind_storage_metadata();
            return *this;
        }
        NodeExecutor(NodeExecutor const&) = delete;
        NodeExecutor& operator=(NodeExecutor const&) = delete;

        static NodeExecutor create(
            TypeErasedNode root,
            ResourceContext resources,
            ExecutionTargetRegistry& execution_target_registry,
            size_t executor_id,
            std::vector<ModuleRef> module_refs = {},
            size_t default_silence_ttl_samples = std::numeric_limits<size_t>::max()
        )
        {
            execution_target_registry.register_executor(executor_id);
            try {
                auto prepared = prepare_state(
                    std::move(root),
                    resources,
                    execution_target_registry,
                    executor_id,
                    "create",
                    true,
                    nullptr,
                    default_silence_ttl_samples
                );
                execution_target_registry.validate_executor_block_size(executor_id, prepared.max_block_size);

                return NodeExecutor(
                    std::move(prepared.root),
                    std::move(module_refs),
                    std::move(prepared.resources),
                    std::move(prepared.event_stream_storage),
                    execution_target_registry,
                    std::move(prepared.layout),
                    std::move(prepared.storage),
                    prepared.max_block_size,
                    default_silence_ttl_samples,
                    executor_id
                );
            } catch (...) {
                execution_target_registry.unregister_executor(executor_id);
                throw;
            }
        }

        static NodeExecutor create(
            TypeErasedNode root,
            ResourceContext resources,
            ExecutionTargetRegistry& execution_target_registry,
            size_t executor_id,
            size_t default_silence_ttl_samples
        )
        {
            return create(
                std::move(root),
                std::move(resources),
                execution_target_registry,
                executor_id,
                {},
                default_silence_ttl_samples
            );
        }

        NodeExecutor(
            TypeErasedNode root,
            std::vector<ModuleRef> module_refs,
            ResourceContext resources,
            std::unique_ptr<EventStreamStorage> event_stream_storage,
            ExecutionTargetRegistry& execution_target_registry,
            NodeLayout layout,
            NodeStorage storage,
            size_t max_block_size,
            size_t default_silence_ttl_samples,
            size_t executor_id
        ) :
            _module_refs(std::move(module_refs)),
            _root(std::move(root)),
            _resources(std::move(resources)),
            _event_stream_storage(std::move(event_stream_storage)),
            _execution_target_registry(&execution_target_registry),
            _layout(std::move(layout)),
            _storage(std::move(storage)),
            _max_block_size(max_block_size),
            _default_silence_ttl_samples(default_silence_ttl_samples),
            _executor_id(executor_id)
        {
            validate_root_interface(_root);
            rebind_storage_metadata();
        }

        void tick_block(size_t index, size_t block_size)
        {
            if (block_size == 0) {
                return;
            }
            validate_block_size(block_size, "NodeExecutor block size must be a power of 2");
            if (block_size > _max_block_size) {
                throw std::logic_error("NodeExecutor block size exceeds max block size");
            }

            _execution_target_registry->sync_block(_executor_id, std::nullopt, index, block_size, false);
            run_block(index, block_size);
            _execution_target_registry->sync_block(_executor_id, index, index + block_size, block_size, false);
        }

        void request_shutdown()
        {
            _shutdown_requested = true;
            if (_execution_target_registry) {
                _execution_target_registry->request_shutdown(_executor_id);
            }
        }

        bool is_shutdown_requested() const
        {
            return _shutdown_requested || (_execution_target_registry && _execution_target_registry->is_shutdown_requested(_executor_id));
        }

        void reload(TypeErasedNode root, std::vector<ModuleRef> module_refs = {})
        {
            auto prepared = prepare_state(
                std::move(root),
                _resources,
                *_execution_target_registry,
                _executor_id,
                "reload",
                false,
                _event_stream_storage.get(),
                _default_silence_ttl_samples
            );
            _execution_target_registry->validate_executor_block_size(_executor_id, prepared.max_block_size);

            _execution_target_registry->clear_audio_state_for_executor(_executor_id);
            _storage.release(_execution_target_registry, _executor_id);
            try {
                prepared.storage.initialize(nullptr, _execution_target_registry, _executor_id);
            } catch (std::exception const& e) {
                throw std::runtime_error(std::string("failed to reload node executor: initialize_storage: ") + e.what());
            } catch (...) {
                throw std::runtime_error("failed to reload node executor: initialize_storage");
            }

            _root = std::move(prepared.root);
            _module_refs = std::move(module_refs);
            _layout = std::move(prepared.layout);
            _storage = std::move(prepared.storage);
            rebind_storage_metadata();
            _max_block_size = prepared.max_block_size;
        }

        void reload(ModuleLoader::LoadedGraph loaded_graph)
        {
            reload(std::move(loaded_graph.root), std::move(loaded_graph.module_refs));
        }

        void execute(std::function<std::optional<ModuleLoader::LoadedGraph>()> poll_reload = {})
        {
            size_t block_index = 0;
            if (!_execution_target_registry->sync_block(_executor_id, std::nullopt, block_index, _max_block_size)) {
                return;
            }
            while (!is_shutdown_requested()) {
                run_block(block_index, _max_block_size);

                if (poll_reload) {
                    auto next_reload = poll_reload();
                    if (next_reload.has_value()) {
                        reload(std::move(*next_reload));
                    }
                }

                size_t const completed_block_index = block_index;
                block_index += _max_block_size;
                if (!_execution_target_registry->sync_block(_executor_id, completed_block_index, block_index, _max_block_size)) {
                    break;
                }
            }
        }

        size_t max_block_size() const
        {
            return _max_block_size;
        }

        size_t num_module_refs() const
        {
            return _module_refs.size();
        }

        TypeErasedNode const& root() const
        {
            return _root;
        }

        ExecutionTargetRegistry& execution_target_registry()
        {
            return *_execution_target_registry;
        }

        NodeLayout const& layout() const
        {
            return _layout;
        }

        NodeStorage& storage()
        {
            return _storage;
        }

        ResourceContext const& resources() const
        {
            return _resources;
        }
    };
}
