#pragma once
#include "compat.h"
#include <algorithm>
#include <concepts>
#include <span>
#include <cassert>
#include <cstddef>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <array>
#include <vector>
#include <iostream>


namespace iv {
	struct Sample {
        using storage = float;
        storage value{};

        constexpr Sample() = default;

        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        constexpr Sample(T v) : value(static_cast<float>(v)) {}

        constexpr operator float() const noexcept {
            return value;
        }

        constexpr Sample& operator+=(Sample other) noexcept { value += other.value; return *this; }
        constexpr Sample& operator-=(Sample other) noexcept { value -= other.value; return *this; }
        constexpr Sample& operator*=(Sample other) noexcept { value *= other.value; return *this; }
        constexpr Sample& operator/=(Sample other) noexcept { value /= other.value; return *this; }

        friend constexpr Sample operator+(Sample a, Sample b) noexcept { return a.value + b.value; }
        friend constexpr Sample operator-(Sample a, Sample b) noexcept { return a.value - b.value; }
        friend constexpr Sample operator*(Sample a, Sample b) noexcept { return a.value * b.value; }
        friend constexpr Sample operator/(Sample a, Sample b) noexcept { return a.value / b.value; }

        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator+(Sample a, T b) noexcept {
            return a.value + static_cast<float>(b);
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator-(Sample a, T b) noexcept {
            return a.value - static_cast<float>(b);
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator*(Sample a, T b) noexcept {
            return a.value * static_cast<float>(b);
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator/(Sample a, T b) noexcept {
            return a.value / static_cast<float>(b);
        }

        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator+(T a, Sample b) noexcept {
            return static_cast<float>(a) + b.value;
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator-(T a, Sample b) noexcept {
            return static_cast<float>(a) - b.value;
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator*(T a, Sample b) noexcept {
            return static_cast<float>(a) * b.value;
        }
        template <typename T>
            requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        friend constexpr Sample operator/(T a, Sample b) noexcept {
            return static_cast<float>(a) / b.value;
        }

        constexpr Sample& operator++() noexcept {
            ++value;
            return *this;
        }
        constexpr Sample operator++(int) noexcept {
            Sample tmp = *this;
            ++(*this);
            return tmp;
        }

        constexpr Sample& operator--() noexcept {
            --value;
            return *this;
        }
        constexpr Sample operator--(int) noexcept {
            Sample tmp = *this;
            --(*this);
            return tmp;
        }
    };

    inline constexpr size_t MAX_BLOCK_SIZE = size_t(1) << (std::numeric_limits<size_t>::digits - 1);

    IV_FORCEINLINE bool is_power_of_2(size_t n)
    {
        return n && !(n & (n - 1));
    }

    template<typename A>
    requires std::unsigned_integral<A>
    IV_FORCEINLINE A next_power_of_2(A x)
    {
        x--;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        if constexpr (sizeof(A) >= 2) x |= x >> 8;
        if constexpr (sizeof(A) >= 4) x |= x >> 16;
        if constexpr (sizeof(A) >= 8) x |= x >> 32;
        if constexpr (sizeof(A) >= 16) x |= x >> 64;
        x++;

        return x;
    }

    IV_FORCEINLINE constexpr bool is_valid_block_size(size_t block_size)
    {
        return block_size != 0 && block_size <= MAX_BLOCK_SIZE && is_power_of_2(block_size);
    }

    IV_FORCEINLINE void validate_block_size(size_t block_size, char const* message = "block size must be a power of 2")
    {
        if (!is_valid_block_size(block_size)) {
            throw std::logic_error(message);
        }
    }

    IV_FORCEINLINE void validate_max_block_size(size_t block_size, char const* message = "max_block_size must be a power of 2")
    {
        if (block_size == MAX_BLOCK_SIZE) {
            return;
        }
        validate_block_size(block_size, message);
    }

    IV_FORCEINLINE size_t prev_power_of_2(size_t n)
    {
        if (n == 0) {
            return 0;
        }
        size_t power = 1;
        while ((power << 1) != 0 && (power << 1) <= n) {
            power <<= 1;
        }
        return power;
    }

    template<typename A>
    struct BlockView {
        std::span<A> first {};
        std::span<A> second {};

        constexpr size_t size() const
        {
            return first.size() + second.size();
        }

        constexpr bool empty() const
        {
            return size() == 0;
        }

        constexpr A operator[](size_t index) const
        {
            return index < first.size()
                ? first[index]
                : second[index - first.size()];
        }

        struct iterator {
            using value_type = A;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;
            using iterator_concept = std::forward_iterator_tag;
            using reference = A;
            using pointer = void;

            A const* first_ptr = nullptr;
            size_t split = 0;
            std::ptrdiff_t second_offset = 0;
            size_t index = 0;

            constexpr reference operator*() const
            {
                return index < split
                    ? first_ptr[index]
                    : (first_ptr + second_offset)[index - split];
            }

            constexpr iterator& operator++()
            {
                ++index;
                return *this;
            }

            constexpr iterator operator++(int)
            {
                auto tmp = *this;
                ++*this;
                return tmp;
            }

            constexpr bool operator==(iterator const&) const = default;
        };

        constexpr iterator begin() const
        {
            return iterator{
                first.data(),
                first.size(),
                second.data() - first.data(),
                0
            };
        }

        constexpr iterator end() const
        {
            return iterator{
                first.data(),
                first.size(),
                second.data() - first.data(),
                size()
            };
        }
    };

    IV_FORCEINLINE constexpr BlockView<Sample> make_block_view(
        std::span<Sample> buffer,
        size_t start,
        size_t count
    )
    {
        if (count == 0) {
            return {};
        }

        size_t const first_size = std::min(count, buffer.size() - start);
        return {
            buffer.subspan(start, first_size),
            buffer.subspan(0, count - first_size),
        };
    }

    struct SharedPortData {
        std::span<Sample> buffer;
        size_t latency;

        constexpr explicit SharedPortData(
            std::span<Sample> buffer = {},
            size_t latency = 0
        ) :
            buffer(buffer),
            latency(latency)
        {}
    };

    class InputPort {
        SharedPortData& _shared_data;
        size_t _history;
        size_t _read_position = 0;

        friend void advance_input(InputPort&, size_t);
        friend void advance_inputs(std::span<InputPort>, size_t);

        IV_FORCEINLINE constexpr size_t current_read_position() const
        {
            return _read_position & (buffer_size() - 1);
        }

    private:
        IV_FORCEINLINE constexpr void advance(size_t amount = 1)
        {
            _read_position = (_read_position + amount) & (buffer_size() - 1);
        }

    public:
        explicit InputPort(
            SharedPortData& shared_data,
            size_t history
        ) :
            _shared_data(shared_data),
            _history(history)
        {
            assert(is_power_of_2(_shared_data.buffer.size()) && "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const
        {
            if (offset > _history) return 0.0f;
            size_t const idx = (current_read_position() + buffer_size() - offset) & (buffer_size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr BlockView<Sample> get_block(size_t block_size, size_t sample_offset = 0) const
        {
            if (sample_offset > block_size) {
                return {};
            }

            size_t const start = (current_read_position() + sample_offset) & (buffer_size() - 1);
            return make_block_view(_shared_data.buffer, start, block_size - sample_offset);
        }

        IV_FORCEINLINE constexpr size_t latency() const
        {
            return _shared_data.latency;
        }

        IV_FORCEINLINE constexpr size_t buffer_size() const
        {
            return _shared_data.buffer.size();
        }
    };

    class OutputPort {
        SharedPortData& _shared_data;
        size_t _history;
        size_t _position = 0;

    public:
        explicit OutputPort(SharedPortData& shared_data, size_t history) :
            _shared_data(shared_data),
            _history(history)
        {
            assert(is_power_of_2(_shared_data.buffer.size()) && "buffer size should be a power of 2");
        }

        IV_FORCEINLINE constexpr Sample get(size_t offset = 0) const
        {
            if (offset > _shared_data.latency + _history) return 0.0f;
            size_t const idx = (
                _position + _shared_data.latency + buffer_size() - 1 - offset
            ) & (buffer_size() - 1);
            return _shared_data.buffer[idx];
        }

        IV_FORCEINLINE constexpr BlockView<Sample> get_block(size_t block_size, size_t sample_offset = 0) const
        {
            if (sample_offset > block_size) {
                return {};
            }

            size_t const start = (
                _position + _shared_data.latency + buffer_size() - block_size + sample_offset
            ) & (buffer_size() - 1);
            return make_block_view(_shared_data.buffer, start, block_size - sample_offset);
        }

        IV_FORCEINLINE constexpr void push(Sample value)
        {
            size_t const idx = (_position + _shared_data.latency) & (buffer_size() - 1);
            _shared_data.buffer[idx] = value;
            _position = (_position + 1) & (buffer_size() - 1);
        }

        IV_FORCEINLINE constexpr void push_block(std::span<Sample const> samples)
        {
            for (Sample const& sample : samples) {
                push(sample);
            }
        }

        IV_FORCEINLINE constexpr void push_block(BlockView<Sample> samples)
        {
            push_block(samples.first);
            push_block(samples.second);
        }

        IV_FORCEINLINE constexpr void update(Sample value, size_t offset = 0)
        {
            if (offset > _shared_data.latency) return;
            size_t const idx = (_position + _shared_data.latency + buffer_size() - offset) & (buffer_size() - 1);
            _shared_data.buffer[idx] = value;
        }

        IV_FORCEINLINE constexpr size_t position() const
        {
            return _position;
        }

        IV_FORCEINLINE constexpr size_t buffer_size() const
        {
            return _shared_data.buffer.size();
        }
    };

    struct InputConfig {
        std::string name {};
        size_t history = 0;
        Sample default_value = 0.0;
    };

    struct OutputConfig {
        std::string name {};
        size_t latency = 0;
        size_t history = 0;
    };

    template<typename Node>
    struct NodeStateType {
        using Type = void;
    };

    namespace details {
        template<typename Node>
        concept has_State = requires {
            typename Node::State;
        };
    }

    template<typename Node>
    requires(details::has_State<Node>)
    struct NodeStateType<Node> {
        using Type = typename Node::State;
    };

    template<typename Node>
    struct NodeState {
        std::span<InputPort> inputs;
        std::span<OutputPort> outputs;
        std::span<std::byte> buffer;

        using State = typename NodeStateType<Node>::Type;

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>)
        {
            void* ptr = buffer.data();
            size_t space = buffer.size();
            return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), ptr, space));
        }
    };

    template<typename Node>
    struct TickContext : public NodeState<Node> {
        size_t index;

        TickContext(NodeState<Node> base, size_t index)
        : NodeState<Node>(base), index(index)
        {}
    };

    template<typename Node>
    struct TickBlockContext : public NodeState<Node> {
        size_t index;
        size_t block_size;

        TickBlockContext(
            NodeState<Node> base,
            size_t index,
            size_t block_size
        )
        : NodeState<Node>(base)
        , index(index)
        , block_size(block_size)
        {}
    };

    template<typename A>
    struct NoCopy : public A
    {
        NoCopy(NoCopy const&) = delete;
        NoCopy(NoCopy&&) = delete;
    };

    template<>
    struct NoCopy<void>
    {
        NoCopy(NoCopy const&) = delete;
        NoCopy(NoCopy&&) = delete;
    };

    struct NodeLayout {
    };

    struct NodeLayoutBuilder {
        template<typename Node, typename A>
        A const* local_object(Node const& node);

        template<typename Node, typename Marker, typename A>
        void local_array(Node const& node, Marker const*, std::span<A> const*, size_t);

        template<typename Node, typename A>
        void export_array(Node const& node, std::string id, std::span<A> value);

        template<typename Node, typename A>
        void import_array(Node const& node, std::string id, std::span<A> const&);

        template<typename A>
        bool has_import_array(std::string const& id) const;

        template<typename A>
        bool has_export_array(std::string const& id) const;

        template<typename A>
        std::span<A> get_export_array(std::string const& id) const;

        template<typename A>
        void bind_import_array(std::string const& id, std::span<A> value);

        size_t max_block_size() const;

        NodeLayout build() &&;
    };

    struct ResourceContext {
        struct VstResources {
            template<typename Descriptor>
            void* create(Descriptor const& descriptor) const;
        };

        VstResources const& vst;
    };

    template<typename Node>
    struct DeclarationContext
    {
        template<typename>
        friend struct DeclarationContext;

        using State = typename NodeStateType<Node>::Type;

    private:
        NodeLayoutBuilder* _builder;
        Node const* _node;
        State const* _state_marker;

    public:
        explicit DeclarationContext(NodeLayoutBuilder& builder, Node const& node)
        : _builder(&builder)
        , _node(&node)
        {
            if constexpr (details::has_State<Node>)
            {
                _state_marker = _builder->template local_object<Node, State>(node);
            }
            else
            {
                _state_marker = nullptr;
            }
        }

        template<typename Node2>
        DeclarationContext(DeclarationContext<Node2> const& ctx, Node const& node)
        : DeclarationContext<Node>(*ctx._builder, node)
        {}

        NoCopy<State> const& state() const
        requires(!std::is_void_v<State>)
        {
            return reinterpret_cast<NoCopy<State> const&>(*_state_marker);
        }

        template<typename A>
        void local_array(std::span<A> const& span, size_t count) const
        {
            _builder->local_array(*_node, _state_marker, &span, count);
        }

        template<typename A>
        void export_array(std::string id, std::span<A> const& span) const
        {
            _builder->export_array(*_node, std::move(id), span);
        }

        template<typename A>
        void import_array(std::string id, std::span<A> const& span) const
        {
            _builder->import_array(*_node, std::move(id), span);
        }

        size_t max_block_size() const
        {
            return _builder->max_block_size();
        }
    };

    template<typename Node>
    struct InitializationContext {
        using State = typename NodeStateType<Node>::Type;

    private:
        NodeLayoutBuilder* _builder;
        void* _state = nullptr;

    public:
        ResourceContext const& resources;

        explicit InitializationContext(NodeLayoutBuilder& builder, void* state, ResourceContext const& resources);

        template<typename Node2>
        InitializationContext(InitializationContext<Node2> const& ctx);

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>)
        ;
    };

    template<typename Node>
    struct ReleaseContext {
        using State = typename NodeStateType<Node>::Type;

    private:
        NodeLayoutBuilder* _builder;
        void* _state = nullptr;

    public:
        ResourceContext const& resources;

        explicit ReleaseContext(NodeLayoutBuilder& builder, void* state, ResourceContext const& resources);

        template<typename Node2>
        ReleaseContext(ReleaseContext<Node2> const& ctx);

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>)
        ;
    };

    namespace details
    {
        template <typename Node>
        concept has_outputs = requires(Node node, std::span<OutputConfig const> outputs)
        {
            outputs = node.outputs();
        };

        template <typename Node>
        concept has_num_outputs = requires(Node node, size_t num_outputs)
        {
            num_outputs = node.num_outputs();
        };

        template <typename Node>
        concept has_inputs = requires(Node node, std::span<InputConfig const> inputs)
        {
            inputs = node.inputs();
        };

        template <typename Node>
        concept has_num_inputs = requires(Node node, size_t num_inputs)
        {
            num_inputs = node.num_inputs();
        };

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
        concept has_tick = requires(Node node, TickContext<Node> state)
        {
            node.tick(state);
        };

        template <typename Node>
        concept has_tick_block = requires(Node node, TickBlockContext<Node> state)
        {
            node.tick_block(state);
        };

        template <typename Node>
        concept has_internal_latency = requires(Node node, size_t internal_latency)
        {
            internal_latency = node.internal_latency();
        };

        template <typename Node>
        concept has_max_block_size_method = requires(Node node, size_t block_size)
        {
            block_size = node.max_block_size();
        };
    }

    template<typename Node>
    constexpr auto get_outputs(Node const& node)
    {
        if constexpr (details::has_outputs<Node>)
        {
            return node.outputs();
        }
        else
        {
            return std::span<OutputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_outputs(Node const& node)
    {
        if constexpr (details::has_num_outputs<Node>)
        {
            return node.num_outputs();
        }
        else
        {
            return get_outputs(node).size();
        }
    }

    template<typename Node>
    constexpr auto get_inputs(Node const& node)
    {
        if constexpr (details::has_inputs<Node>)
        {
            return node.inputs();
        }
        else
        {
            return std::span<InputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_inputs(Node const& node)
    {
        if constexpr (details::has_num_inputs<Node>)
        {
            return node.num_inputs();
        }
        else
        {
            return get_inputs(node).size();
        }
    }

    template<typename Node, typename Ctx>
    constexpr void do_declare(Node const& node, Ctx& ctx)
    {
        DeclarationContext<Node> node_ctx(ctx, node);
        if constexpr (details::has_declare<Node>)
        {
            node.declare(node_ctx);
        }
    }

    template<typename Node, typename Ctx>
    constexpr void do_initialize(Node const& node, Ctx& ctx)
    {
        InitializationContext<Node> node_ctx(ctx);
        if constexpr (details::has_initialize<Node>)
        {
            node.initialize(node_ctx);
        }
    }

    template<typename Node, typename Ctx>
    constexpr void do_release(Node const& node, Ctx& ctx)
    {
        ReleaseContext<Node> node_ctx(ctx);
        if constexpr (details::has_release<Node>)
        {
            node.release(node_ctx);
        }
    }

    template<typename Node>
    constexpr size_t get_internal_latency(Node const& node)
    {
        if constexpr (details::has_internal_latency<Node>)
        {
            return node.internal_latency();
        }
        else
        {
            return 0;
        }
    }

    template<typename Node>
    constexpr size_t get_max_block_size(Node const& node)
    {
        if constexpr (details::has_max_block_size_method<Node>)
        {
            return node.max_block_size();
        }
        else
        {
            return MAX_BLOCK_SIZE;
        }
    }

    IV_FORCEINLINE void advance_input(InputPort& input, size_t amount = 1)
    {
        input.advance(amount);
    }

    IV_FORCEINLINE void advance_inputs(std::span<InputPort> inputs, size_t amount)
    {
        for (InputPort& input : inputs) {
            advance_input(input, amount);
        }
    }

    template<typename Node>
    void do_tick_block(Node& node, TickBlockContext<Node> const& state);

    template<typename Node>
    void do_tick(Node& node, TickContext<Node> const& ctx)
    {
        if constexpr (details::has_tick<Node>)
        {
            node.tick(ctx);
            advance_inputs(ctx.inputs, 1);
        }
        else if constexpr (details::has_tick_block<Node>)
        {
            do_tick_block(node, {
                static_cast<NodeState<Node> const&>(ctx),
                ctx.index,
                1,
            });
        }
        else
        {
            static_assert(details::has_tick<Node> || details::has_tick_block<Node>, "node must implement tick() or tick_block()");
        }
    }

    template<typename Node>
    void do_tick_block(Node& node, TickBlockContext<Node> const& ctx)
    {
        if (ctx.block_size == 0) {
            return;
        }
        validate_block_size(ctx.block_size);

        if constexpr (details::has_tick_block<Node>)
        {
            node.tick_block(ctx);
            advance_inputs(ctx.inputs, ctx.block_size);
        }
        else
        {
            for (size_t i = 0; i < ctx.block_size; ++i) {
                do_tick(node, {
                    static_cast<NodeState<Node> const&>(ctx),
                    ctx.index + i,
                });
            }
        }
    }

    template <typename Node>
    void info(Node&& node) {
        std::cout << "internal latency: " << get_internal_latency(node) << "\n";

        auto const block_size = get_max_block_size(node);
        std::cout << "max block size: " << ((block_size == MAX_BLOCK_SIZE) ? std::string("unbounded") : std::to_string(block_size)) << "\n";
        std::cout << "params:\n";

        for (auto const& in : get_inputs(node)) {
            std::cout << "    in: " << in.name << " (" << in.default_value << ")\n";
        }
        for (auto const& out : get_outputs(node)) {
            std::cout << "    out: " << out.name << "\n";
        }
    }
}
