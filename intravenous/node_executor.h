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
#include <vector>

namespace iv {
    class NodeExecutor {
        TypeErasedNode _root;
        std::vector<ModuleRef> _module_refs;
        ResourceContext _resources;
        ExecutionTargets _execution_targets;
        NodeLayout _layout;
        NodeStorage _storage;
        size_t _max_block_size;
        bool _shutdown_requested = false;

        static size_t choose_block_size(TypeErasedNode const& root, ExecutionTargets const& execution_targets)
        {
            size_t block_size = root.max_block_size();
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
            do_declare(root, ctx);
            return std::move(builder).build();
        }

    public:
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

            NodeLayout layout = make_layout(root, root.max_block_size());
            NodeStorage storage = layout.create_storage(resources);
            storage.initialize(nullptr, &execution_targets);
            size_t max_block_size = choose_block_size(root, execution_targets);

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
            _root(std::move(root)),
            _module_refs(std::move(module_refs)),
            _resources(std::move(resources)),
            _execution_targets(std::move(execution_targets)),
            _layout(std::move(layout)),
            _storage(std::move(storage)),
            _max_block_size(max_block_size)
        {
            if (get_num_inputs(_root) != 0 || get_num_outputs(_root) != 0) {
                throw std::logic_error("NodeExecutor root must have 0 public inputs and outputs");
            }
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

            _root.tick_block({
                TickContext<TypeErasedNode> {
                    .inputs = {},
                    .outputs = {},
                    .buffer = _storage.buffer(),
                },
                index,
                block_size
            });

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

            NodeLayout next_layout = make_layout(root, root.max_block_size());
            NodeStorage next_storage = next_layout.create_storage(_resources);
            next_storage.initialize(nullptr, &_execution_targets);

            _storage.release(&_execution_targets);

            _root = std::move(root);
            _module_refs = std::move(module_refs);
            _layout = std::move(next_layout);
            _storage = std::move(next_storage);
            _max_block_size = choose_block_size(_root, _execution_targets);
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
