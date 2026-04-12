#pragma once

#include "basic_nodes/type_erased.h"
#include "orchestrator/device_orchestrator.h"
#include "orchestrator/orchestrator_builder.h"
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
        static constexpr size_t DEFAULT_SILENCE_TTL_BLOCKS = 16;

        struct PreparedState {
            TypeErasedNode root;
            ResourceContext resources;
            NodeLayout layout;
            NodeStorage storage;
            size_t max_block_size = 0;
            size_t resolved_default_silence_ttl_samples = 0;
        };

        std::vector<ModuleRef> _module_refs;
        TypeErasedNode _root;
        ResourceContext _resources;
        DeviceOrchestrator _orchestrator;
        NodeLayout _layout;
        NodeStorage _storage;
        size_t _max_block_size;
        size_t _default_silence_ttl_samples = 0;
        size_t _event_port_buffer_base_multiplier = DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER;
        bool _shutdown_requested = false;

        void rebind_storage_metadata()
        {
            _storage.layout = &_layout;
            _storage.resources = &_resources;
        }

        static void validate_root_interface(TypeErasedNode const& root)
        {
            if (get_num_inputs(root) != 0 || get_num_outputs(root) != 0) {
                throw std::logic_error("NodeExecutor root must have 0 public inputs and outputs");
            }
        }

        static size_t choose_block_size(TypeErasedNode const& root, OrchestratorBuilder const& orchestrator_builder)
        {
            size_t block_size = std::min(root.max_block_size(), orchestrator_builder.preferred_block_size_hint());
            block_size = std::bit_floor(block_size);
            if (block_size == 0) {
                throw std::logic_error("NodeExecutor max block size must be non-zero");
            }
            validate_max_block_size(block_size, "NodeExecutor max block size must be a power of 2");
            return block_size;
        }

        static size_t resolve_default_silence_ttl_samples(size_t max_block_size, std::optional<size_t> requested_ttl_samples)
        {
            if (requested_ttl_samples.has_value()) {
                return *requested_ttl_samples;
            }
            return max_block_size * DEFAULT_SILENCE_TTL_BLOCKS;
        }

        static NodeLayout make_layout(
            TypeErasedNode const& root,
            size_t max_block_size,
            size_t default_silence_ttl_samples,
            size_t event_port_buffer_base_multiplier
        )
        {
            NodeLayoutBuilder builder(max_block_size, default_silence_ttl_samples, event_port_buffer_base_multiplier);
            {
                DeclarationContext<TypeErasedNode> ctx(builder, root);
                root.declare(ctx);
            }
            return std::move(builder).build();
        }

        static PreparedState prepare_state(
            TypeErasedNode root,
            ResourceContext resources,
            OrchestratorBuilder& orchestrator_builder,
            char const* operation,
            bool initialize_storage = true,
            std::optional<size_t> default_silence_ttl_samples = std::nullopt,
            size_t event_port_buffer_base_multiplier = DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER
        )
        {
            validate_root_interface(root);

            PreparedState prepared;
            prepared.root = std::move(root);
            prepared.resources = std::move(resources);
            prepared.max_block_size = choose_block_size(prepared.root, orchestrator_builder);
            prepared.resolved_default_silence_ttl_samples = resolve_default_silence_ttl_samples(
                prepared.max_block_size,
                default_silence_ttl_samples
            );
            try {
                prepared.layout = make_layout(
                    prepared.root,
                    prepared.max_block_size,
                    prepared.resolved_default_silence_ttl_samples,
                    event_port_buffer_base_multiplier
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
                    prepared.storage.initialize(nullptr, &orchestrator_builder);
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
                        .buffer = _storage.buffer(),
                    },
                    index,
                    block_size
                });
            } catch (std::exception const& e) {
                _shutdown_requested = true;
                throw std::runtime_error(std::string("node executor tick failed: ") + e.what());
            } catch (...) {
                _shutdown_requested = true;
                throw std::runtime_error("node executor tick failed");
            }
        }

        void validate_tick_block_size(size_t block_size) const
        {
            if (block_size == 0) {
                return;
            }
            validate_block_size(block_size, "NodeExecutor block size must be a power of 2");
            if (block_size > _max_block_size) {
                throw std::logic_error("NodeExecutor block size exceeds max block size");
            }
        }

    public:
        ~NodeExecutor()
        {
            try {
                auto builder = std::move(_orchestrator).to_builder();
                _storage.release(&builder);
            } catch (...) {
            }
        }

        NodeExecutor(NodeExecutor&& other) noexcept
        : _module_refs(std::move(other._module_refs))
        , _root(std::move(other._root))
        , _resources(std::move(other._resources))
        , _orchestrator(std::move(other._orchestrator))
        , _layout(std::move(other._layout))
        , _storage(std::move(other._storage))
        , _max_block_size(other._max_block_size)
        , _default_silence_ttl_samples(other._default_silence_ttl_samples)
        , _event_port_buffer_base_multiplier(other._event_port_buffer_base_multiplier)
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
                auto builder = std::move(_orchestrator).to_builder();
                _storage.release(&builder);
            } catch (...) {
            }

            _module_refs = std::move(other._module_refs);
            _root = std::move(other._root);
            _resources = std::move(other._resources);
            _orchestrator = std::move(other._orchestrator);
            _layout = std::move(other._layout);
            _storage = std::move(other._storage);
            _max_block_size = other._max_block_size;
            _default_silence_ttl_samples = other._default_silence_ttl_samples;
            _event_port_buffer_base_multiplier = other._event_port_buffer_base_multiplier;
            _shutdown_requested = other._shutdown_requested;
            rebind_storage_metadata();
            return *this;
        }
        NodeExecutor(NodeExecutor const&) = delete;
        NodeExecutor& operator=(NodeExecutor const&) = delete;

        static NodeExecutor create(
            TypeErasedNode root,
            ResourceContext resources,
            OrchestratorBuilder orchestrator_builder = {},
            std::vector<ModuleRef> module_refs = {},
            std::optional<size_t> default_silence_ttl_samples = std::nullopt,
            size_t event_port_buffer_base_multiplier = DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER
        )
        {
            auto prepared = prepare_state(
                std::move(root),
                resources,
                orchestrator_builder,
                "create",
                true,
                default_silence_ttl_samples,
                event_port_buffer_base_multiplier
            );
            auto device_orchestrator = std::move(orchestrator_builder).build();

            return NodeExecutor(
                std::move(prepared.root),
                std::move(module_refs),
                std::move(prepared.resources),
                std::move(device_orchestrator),
                std::move(prepared.layout),
                std::move(prepared.storage),
                prepared.max_block_size,
                prepared.resolved_default_silence_ttl_samples,
                event_port_buffer_base_multiplier
            );
        }

        NodeExecutor(
            TypeErasedNode root,
            std::vector<ModuleRef> module_refs,
            ResourceContext resources,
            DeviceOrchestrator device_orchestrator,
            NodeLayout layout,
            NodeStorage storage,
            size_t max_block_size,
            size_t default_silence_ttl_samples,
            size_t event_port_buffer_base_multiplier
        ) :
            _module_refs(std::move(module_refs)),
            _root(std::move(root)),
            _resources(std::move(resources)),
            _orchestrator(std::move(device_orchestrator)),
            _layout(std::move(layout)),
            _storage(std::move(storage)),
            _max_block_size(max_block_size),
            _default_silence_ttl_samples(default_silence_ttl_samples),
            _event_port_buffer_base_multiplier(event_port_buffer_base_multiplier)
        {
            validate_root_interface(_root);
            rebind_storage_metadata();
        }

        void request_shutdown()
        {
            _shutdown_requested = true;
            _orchestrator.request_shutdown();
        }

        bool is_shutdown_requested() const
        {
            return _shutdown_requested;
        }

        void reload(TypeErasedNode root, std::vector<ModuleRef> module_refs = {})
        {
            auto builder = std::move(_orchestrator).to_builder();
            auto prepared = prepare_state(
                std::move(root),
                _resources,
                builder,
                "reload",
                false,
                _default_silence_ttl_samples,
                _event_port_buffer_base_multiplier
            );

            _storage.release(&builder);
            try {
                prepared.storage.initialize(nullptr, &builder);
            } catch (std::exception const& e) {
                throw std::runtime_error(std::string("failed to reload node executor: initialize_storage: ") + e.what());
            } catch (...) {
                throw std::runtime_error("failed to reload node executor: initialize_storage");
            }

            _root = std::move(prepared.root);
            _module_refs = std::move(module_refs);
            _layout = std::move(prepared.layout);
            _storage = std::move(prepared.storage);
            _orchestrator = std::move(builder).build();
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
            std::optional<ModuleLoader::LoadedGraph> pending_reload;
            auto const operation_pending = [&]() -> bool {
                if (is_shutdown_requested()) {
                    return true;
                }
                if (!pending_reload.has_value() && poll_reload) {
                    pending_reload = poll_reload();
                }
                return pending_reload.has_value();
            };

            _orchestrator.wait_for_block();

            for (;;) {
                validate_tick_block_size(_max_block_size);
                while (!operation_pending()) {
                    run_block(block_index, _max_block_size);
                    _orchestrator.sync_block(block_index, _max_block_size);
                    block_index += _max_block_size;
                }

                if (is_shutdown_requested()) {
                    return;
                }
                reload(std::move(*pending_reload));
                pending_reload.reset();
                _orchestrator.wait_for_block();
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

        DeviceOrchestrator& device_orchestrator()
        {
            return _orchestrator;
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
