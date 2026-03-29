#pragma once

#include "basic_nodes/type_erased.h"
#include "execution_targets.h"
#include "node_lifecycle.h"
#include "module/loader.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {
    class NodeExecutor {
        std::vector<ModuleRef> _module_refs;
        TypeErasedNode _root;
        ResourceContext _resources;
        ExecutionTargets _execution_targets;
        NodeLayout _layout;
        NodeStorage _storage;
        size_t _max_block_size;
        bool _shutdown_requested = false;

        void rebind_storage_metadata()
        {
            _storage.layout = &_layout;
            _storage.resources = &_resources;
        }

        static size_t choose_block_size(TypeErasedNode const& root, ExecutionTargets const& execution_targets)
        {
            size_t block_size = std::min(root.max_block_size(), execution_targets.preferred_block_size_hint());
            for (auto const& target : execution_targets.all()) {
                block_size = std::min(block_size, target.preferred_block_size());
            }
            validate_max_block_size(block_size, "NodeExecutor max block size must be a power of 2");
            return block_size;
        }

        static NodeLayout make_layout(TypeErasedNode const& root, size_t max_block_size)
        {
            NodeLayoutBuilder builder(max_block_size);
            DeclarationContext<TypeErasedNode> ctx(builder, root);
            root.declare(ctx);
            return std::move(builder).build();
        }

    public:
        ~NodeExecutor()
        {
            try {
                _storage.release(&_execution_targets);
            } catch (std::exception const& e) {
                debug_log(std::string("node teardown release failed: ") + e.what());
            } catch (...) {
                debug_log("node teardown release failed");
            }
        }

        NodeExecutor(NodeExecutor&& other) noexcept
        : _module_refs(std::move(other._module_refs))
        , _root(std::move(other._root))
        , _resources(std::move(other._resources))
        , _execution_targets(std::move(other._execution_targets))
        , _layout(std::move(other._layout))
        , _storage(std::move(other._storage))
        , _max_block_size(other._max_block_size)
        , _shutdown_requested(other._shutdown_requested)
        {
            rebind_storage_metadata();
        }

        NodeExecutor& operator=(NodeExecutor&& other) noexcept
        {
            if (this == &other) {
                return *this;
            }

            try {
                _storage.release(&_execution_targets);
            } catch (std::exception const& e) {
                debug_log(std::string("node move-assignment release failed: ") + e.what());
            } catch (...) {
                debug_log("node move-assignment release failed");
            }

            _module_refs = std::move(other._module_refs);
            _root = std::move(other._root);
            _resources = std::move(other._resources);
            _execution_targets = std::move(other._execution_targets);
            _layout = std::move(other._layout);
            _storage = std::move(other._storage);
            _max_block_size = other._max_block_size;
            _shutdown_requested = other._shutdown_requested;
            rebind_storage_metadata();
            return *this;
        }
        NodeExecutor(NodeExecutor const&) = delete;
        NodeExecutor& operator=(NodeExecutor const&) = delete;

        static NodeExecutor create(
            TypeErasedNode root,
            ResourceContext resources,
            ExecutionTargets execution_targets,
            std::vector<ModuleRef> module_refs = {}
        )
        {
            if (get_num_inputs(root) != 0 || get_num_outputs(root) != 0) {
                throw std::logic_error("NodeExecutor root must have 0 public inputs and outputs");
            }

            size_t max_block_size = choose_block_size(root, execution_targets);
            NodeLayout layout;
            try {
                layout = make_layout(root, max_block_size);
            } catch (...) {
                throw std::runtime_error("failed to create node executor: make_layout");
            }

            NodeStorage storage;
            try {
                storage = layout.create_storage(resources);
            } catch (...) {
                throw std::runtime_error("failed to create node executor: create_storage");
            }

            try {
                storage.initialize(nullptr, &execution_targets);
            } catch (std::exception const& e) {
                throw std::runtime_error(std::string("failed to create node executor: initialize_storage: ") + e.what());
            } catch (...) {
                throw std::runtime_error("failed to create node executor: initialize_storage");
            }

            return NodeExecutor(
                std::move(root),
                std::move(module_refs),
                std::move(resources),
                std::move(execution_targets),
                std::move(layout),
                std::move(storage),
                max_block_size
            );
        }

        NodeExecutor(
            TypeErasedNode root,
            std::vector<ModuleRef> module_refs,
            ResourceContext resources,
            ExecutionTargets execution_targets,
            NodeLayout layout,
            NodeStorage storage,
            size_t max_block_size
        ) :
            _module_refs(std::move(module_refs)),
            _root(std::move(root)),
            _resources(std::move(resources)),
            _execution_targets(std::move(execution_targets)),
            _layout(std::move(layout)),
            _storage(std::move(storage)),
            _max_block_size(max_block_size)
        {
            if (get_num_inputs(_root) != 0 || get_num_outputs(_root) != 0) {
                throw std::logic_error("NodeExecutor root must have 0 public inputs and outputs");
            }
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

            auto targets = _execution_targets.all();
            for (auto const& target : targets) {
                target.begin_block(index, block_size);
            }

            try {
                _root.tick_block({
                    TickContext<TypeErasedNode> {
                        .inputs = {},
                        .outputs = {},
                        .buffer = _storage.buffer(),
                    },
                    index,
                    block_size
                });
            } catch (std::exception const& e) {
                throw std::runtime_error(std::string("node executor tick failed: ") + e.what());
            } catch (...) {
                throw std::runtime_error("node executor tick failed");
            }

            for (auto const& target : targets) {
                target.end_block(index, block_size);
            }
        }

        void request_shutdown()
        {
            _shutdown_requested = true;
            _execution_targets.request_shutdown();
        }

        bool is_shutdown_requested() const
        {
            return _shutdown_requested || _execution_targets.is_shutdown_requested();
        }

        void reload(TypeErasedNode root, std::vector<ModuleRef> module_refs = {})
        {
            if (get_num_inputs(root) != 0 || get_num_outputs(root) != 0) {
                throw std::logic_error("NodeExecutor root must have 0 public inputs and outputs");
            }

            size_t next_max_block_size = choose_block_size(root, _execution_targets);
            NodeLayout next_layout;
            try {
                next_layout = make_layout(root, next_max_block_size);
            } catch (...) {
                throw std::runtime_error("failed to reload node executor: make_layout");
            }

            NodeStorage next_storage;
            try {
                next_storage = next_layout.create_storage(_resources);
            } catch (...) {
                throw std::runtime_error("failed to reload node executor: create_storage");
            }

            try {
                next_storage.initialize(nullptr, &_execution_targets);
            } catch (std::exception const& e) {
                throw std::runtime_error(std::string("failed to reload node executor: initialize_storage: ") + e.what());
            } catch (...) {
                throw std::runtime_error("failed to reload node executor: initialize_storage");
            }

            _storage.release(&_execution_targets);

            _root = std::move(root);
            _module_refs = std::move(module_refs);
            _layout = std::move(next_layout);
            _storage = std::move(next_storage);
            rebind_storage_metadata();
            _max_block_size = next_max_block_size;
        }

        void reload(ModuleLoader::LoadedGraph loaded_graph)
        {
            reload(std::move(loaded_graph.root), std::move(loaded_graph.module_refs));
        }

        void execute(std::function<std::optional<ModuleLoader::LoadedGraph>()> poll_reload = {})
        {
            for (size_t global_index = 0; !is_shutdown_requested(); global_index += _max_block_size) {
                tick_block(global_index, _max_block_size);

                if (poll_reload) {
                    auto next_reload = poll_reload();
                    if (next_reload.has_value()) {
                        reload(std::move(*next_reload));
                    }
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

        ExecutionTargets& execution_targets()
        {
            return _execution_targets;
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
