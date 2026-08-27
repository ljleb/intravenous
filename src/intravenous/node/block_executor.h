#pragma once

#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/compat.h>
#include <intravenous/node/lifecycle.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace iv {
class BlockNodeExecutor {
    static constexpr size_t DEFAULT_SILENCE_TTL_BLOCKS = 16;

public:
    struct PreparedState {
        TypeErasedNode root;
        ResourceContext resources;
        NodeLayout layout;
        NodeStorage storage;
        size_t resolved_default_silence_ttl_samples = 0;

        PreparedState() = default;
        PreparedState(PreparedState&& other) noexcept
          : root(std::move(other.root)),
            resources(std::move(other.resources)),
            layout(std::move(other.layout)),
            storage(std::move(other.storage)),
            resolved_default_silence_ttl_samples(
                other.resolved_default_silence_ttl_samples)
        {
            storage.layout = &layout;
            storage.resources = &resources;
        }
        PreparedState& operator=(PreparedState&& other) noexcept
        {
            if (this == &other) return *this;
            try {
                storage.release();
            } catch (...) {
            }
            storage.destroy_constructed_states();
            root = std::move(other.root);
            resources = std::move(other.resources);
            layout = std::move(other.layout);
            storage = std::move(other.storage);
            resolved_default_silence_ttl_samples =
                other.resolved_default_silence_ttl_samples;
            storage.layout = &layout;
            storage.resources = &resources;
            return *this;
        }
        PreparedState(PreparedState const&) = delete;
        PreparedState& operator=(PreparedState const&) = delete;
    };

    class PreparedReload {
        friend class BlockNodeExecutor;

        PreparedState state_ {};
        NodeStorage::PreparedMigration migration_ {};

        void rebind()
        {
            state_.storage.layout = &state_.layout;
            state_.storage.resources = &state_.resources;
            migration_.current = &state_.storage;
        }

        PreparedReload(
            PreparedState state,
            NodeStorage::PreparedMigration migration)
          : state_(std::move(state)), migration_(std::move(migration))
        {
            rebind();
        }

    public:
        PreparedReload() = default;
        PreparedReload(PreparedReload&& other) noexcept
          : state_(std::move(other.state_)),
            migration_(std::move(other.migration_))
        {
            rebind();
        }
        PreparedReload& operator=(PreparedReload&& other) noexcept
        {
            if (this == &other) return *this;
            state_ = std::move(other.state_);
            migration_ = std::move(other.migration_);
            rebind();
            return *this;
        }
        PreparedReload(PreparedReload const&) = delete;
        PreparedReload& operator=(PreparedReload const&) = delete;
    };

    class RetiredState {
        friend class BlockNodeExecutor;

        PreparedState state_ {};

        void rebind()
        {
            state_.storage.layout = &state_.layout;
            state_.storage.resources = &state_.resources;
        }

        explicit RetiredState(PreparedState state)
          : state_(std::move(state))
        {
            rebind();
        }

    public:
        RetiredState() = default;
        RetiredState(RetiredState&& other) noexcept
          : state_(std::move(other.state_))
        {
            rebind();
        }
        RetiredState& operator=(RetiredState&& other) noexcept
        {
            if (this == &other) return *this;
            state_ = std::move(other.state_);
            rebind();
            return *this;
        }
        RetiredState(RetiredState const&) = delete;
        RetiredState& operator=(RetiredState const&) = delete;
    };

private:

    TypeErasedNode root_;
    ResourceContext resources_;
    NodeLayout layout_;
    NodeStorage storage_;
    size_t block_size_ = 0;
    size_t sample_rate_ = 48000;
    size_t default_silence_ttl_samples_ = 0;
    size_t event_port_buffer_base_multiplier_ = DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER;

    void rebind_storage_metadata()
    {
        storage_.layout = &layout_;
        storage_.resources = &resources_;
    }

    static void validate_root_interface(TypeErasedNode const& root)
    {
        if (get_num_inputs(root) != 0 || get_num_outputs(root) != 0) {
            throw std::logic_error("BlockNodeExecutor root must have 0 public inputs and outputs");
        }
    }

    static size_t resolve_default_silence_ttl_samples(
        size_t block_size,
        std::optional<size_t> requested_ttl_samples)
    {
        if (requested_ttl_samples.has_value()) {
            return *requested_ttl_samples;
        }
        return block_size * DEFAULT_SILENCE_TTL_BLOCKS;
    }

    static NodeLayout make_layout(
        TypeErasedNode const& root,
        size_t block_size,
        size_t default_silence_ttl_samples,
        size_t event_port_buffer_base_multiplier)
    {
        NodeLayoutBuilder builder(
            block_size,
            default_silence_ttl_samples,
            event_port_buffer_base_multiplier);
        {
            DeclarationContext<TypeErasedNode> ctx(builder, root);
            root.declare(ctx);
        }
        return std::move(builder).build();
    }

    static PreparedState prepare_state(
        TypeErasedNode root,
        ResourceContext resources,
        size_t block_size,
        char const* operation,
        bool initialize_storage,
        std::optional<size_t> default_silence_ttl_samples = std::nullopt,
        size_t event_port_buffer_base_multiplier = DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER)
    {
        validate_root_interface(root);
        if (block_size == 0) {
            throw std::logic_error("BlockNodeExecutor block size must be non-zero");
        }
        validate_block_size(block_size, "BlockNodeExecutor block size must be a power of 2");
        if (block_size > root.max_block_size()) {
            throw std::logic_error("BlockNodeExecutor block size exceeds root max block size");
        }

        PreparedState prepared;
        prepared.root = std::move(root);
        prepared.resources = std::move(resources);
        prepared.resolved_default_silence_ttl_samples = resolve_default_silence_ttl_samples(
            block_size,
            default_silence_ttl_samples);

        try {
            prepared.layout = make_layout(
                prepared.root,
                block_size,
                prepared.resolved_default_silence_ttl_samples,
                event_port_buffer_base_multiplier);
        } catch (std::exception const& e) {
            throw std::runtime_error(wrap_exception(
                std::string("failed to ") + operation + " block executor: make_layout",
                e));
        } catch (...) {
            throw std::runtime_error(
                std::string("failed to ") + operation + " block executor: make_layout");
        }

        try {
            prepared.storage = prepared.layout.create_storage(prepared.resources);
        } catch (std::exception const& e) {
            throw std::runtime_error(wrap_exception(
                std::string("failed to ") + operation + " block executor: create_storage",
                e));
        } catch (...) {
            throw std::runtime_error(
                std::string("failed to ") + operation + " block executor: create_storage");
        }

        if (initialize_storage) {
            try {
                prepared.storage.initialize();
            } catch (std::exception const& e) {
                throw std::runtime_error(wrap_exception(
                    std::string("failed to ") + operation + " block executor: initialize_storage",
                    e));
            } catch (...) {
                throw std::runtime_error(
                    std::string("failed to ") + operation + " block executor: initialize_storage");
            }
        }

        return prepared;
    }

public:
    ~BlockNodeExecutor()
    {
        try {
            storage_.release();
        } catch (...) {
        }
    }

    BlockNodeExecutor(BlockNodeExecutor&& other) noexcept
      : root_(std::move(other.root_))
      , resources_(std::move(other.resources_))
      , layout_(std::move(other.layout_))
      , storage_(std::move(other.storage_))
      , block_size_(other.block_size_)
      , sample_rate_(other.sample_rate_)
      , default_silence_ttl_samples_(other.default_silence_ttl_samples_)
      , event_port_buffer_base_multiplier_(other.event_port_buffer_base_multiplier_)
    {
        rebind_storage_metadata();
    }

    BlockNodeExecutor& operator=(BlockNodeExecutor&& other) noexcept
    {
        if (this == &other) return *this;

        try {
            storage_.release();
        } catch (...) {
        }
        storage_.destroy_constructed_states();

        root_ = std::move(other.root_);
        resources_ = std::move(other.resources_);
        layout_ = std::move(other.layout_);
        storage_ = std::move(other.storage_);
        block_size_ = other.block_size_;
        sample_rate_ = other.sample_rate_;
        default_silence_ttl_samples_ = other.default_silence_ttl_samples_;
        event_port_buffer_base_multiplier_ = other.event_port_buffer_base_multiplier_;
        rebind_storage_metadata();
        return *this;
    }

    BlockNodeExecutor(BlockNodeExecutor const&) = delete;
    BlockNodeExecutor& operator=(BlockNodeExecutor const&) = delete;

    static BlockNodeExecutor create(
        TypeErasedNode root,
        size_t block_size,
        ResourceContext resources = {},
        std::optional<size_t> default_silence_ttl_samples = std::nullopt,
        size_t event_port_buffer_base_multiplier = DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER,
        size_t sample_rate = 48000)
    {
        if (sample_rate == 0) {
            throw std::logic_error("BlockNodeExecutor sample rate must be non-zero");
        }
        auto prepared = prepare_state(
            std::move(root),
            std::move(resources),
            block_size,
            "create",
            true,
            default_silence_ttl_samples,
            event_port_buffer_base_multiplier);

        return BlockNodeExecutor(
            std::move(prepared.root),
            std::move(prepared.resources),
            std::move(prepared.layout),
            std::move(prepared.storage),
            block_size,
            sample_rate,
            prepared.resolved_default_silence_ttl_samples,
            event_port_buffer_base_multiplier);
    }

    BlockNodeExecutor(
        TypeErasedNode root,
        ResourceContext resources,
        NodeLayout layout,
        NodeStorage storage,
        size_t block_size,
        size_t sample_rate,
        size_t default_silence_ttl_samples,
        size_t event_port_buffer_base_multiplier)
      : root_(std::move(root))
      , resources_(std::move(resources))
      , layout_(std::move(layout))
      , storage_(std::move(storage))
      , block_size_(block_size)
      , sample_rate_(sample_rate)
      , default_silence_ttl_samples_(default_silence_ttl_samples)
      , event_port_buffer_base_multiplier_(event_port_buffer_base_multiplier)
    {
        validate_root_interface(root_);
        if (sample_rate_ == 0) {
            throw std::logic_error("BlockNodeExecutor sample rate must be non-zero");
        }
        rebind_storage_metadata();
    }

    void reload(TypeErasedNode root)
    {
        auto prepared = prepare_reload(std::move(root));
        auto retired = commit_reload(std::move(prepared));
    }

    PreparedReload prepare_reload(
        TypeErasedNode root,
        std::optional<size_t> default_silence_ttl_samples = std::nullopt,
        std::optional<ResourceContext> replacement_resources = std::nullopt)
    {
        auto prepared = prepare_state(
            std::move(root),
            replacement_resources.value_or(resources_),
            block_size_,
            "reload",
            false,
            default_silence_ttl_samples.value_or(
                default_silence_ttl_samples_),
            event_port_buffer_base_multiplier_);
        prepared.storage.layout = &prepared.layout;
        prepared.storage.resources = &prepared.resources;
        auto migration = prepared.storage.prepare_migration_from(storage_);
        return PreparedReload(std::move(prepared), std::move(migration));
    }

    RetiredState commit_reload(PreparedReload prepared)
    {
        try {
            prepared.migration_.commit();
        } catch (std::exception const& e) {
            throw std::runtime_error(wrap_exception(
                "failed to reload block executor: commit_migration",
                e));
        } catch (...) {
            throw std::runtime_error(
                "failed to reload block executor: commit_migration");
        }

        PreparedState retired;
        retired.root = std::move(root_);
        retired.resources = std::move(resources_);
        retired.layout = std::move(layout_);
        retired.storage = std::move(storage_);
        retired.resolved_default_silence_ttl_samples =
            default_silence_ttl_samples_;
        retired.storage.layout = &retired.layout;
        retired.storage.resources = &retired.resources;

        root_ = std::move(prepared.state_.root);
        resources_ = std::move(prepared.state_.resources);
        layout_ = std::move(prepared.state_.layout);
        storage_ = std::move(prepared.state_.storage);
        default_silence_ttl_samples_ =
            prepared.state_.resolved_default_silence_ttl_samples;
        rebind_storage_metadata();
        return RetiredState(std::move(retired));
    }

    void tick_block(size_t index)
    {
        root_.tick_block({
            TickContext<TypeErasedNode> {
                .inputs = {},
                .outputs = {},
                .event_inputs = {},
                .event_outputs = {},
                .sample_rate = sample_rate_,
                .buffer = storage_.buffer(),
            },
            index,
            block_size_,
        });
    }

    size_t block_size() const { return block_size_; }
    size_t sample_rate() const { return sample_rate_; }
    TypeErasedNode const& root() const { return root_; }
    NodeLayout const& layout() const { return layout_; }
    NodeStorage& storage() { return storage_; }
    ResourceContext const& resources() const { return resources_; }
};
}
