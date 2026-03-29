#pragma once

#include "compat.h"
#include "sample.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <concepts>

namespace iv {
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

        template<typename Dst>
        IV_FORCEINLINE constexpr void copy_to(BlockView<Dst> dst) const
        {
            IV_ASSERT(size() == dst.size(), "BlockView::copy_to requires matching block sizes");

            auto src_first = first;
            auto src_second = second;
            auto dst_first = dst.first;
            auto dst_second = dst.second;

            auto copy_partial = [](auto& source, auto& target) {
                size_t const n = std::min(source.size(), target.size());
                std::copy_n(source.data(), n, target.data());
                source = source.subspan(n);
                target = target.subspan(n);
            };

            copy_partial(src_first, dst_first);
            copy_partial(src_first, dst_second);
            copy_partial(src_second, dst_first);
            copy_partial(src_second, dst_second);
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
            IV_ASSERT(is_power_of_2(_shared_data.buffer.size()), "buffer size should be a power of 2");
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
            IV_ASSERT(is_power_of_2(_shared_data.buffer.size()), "buffer size should be a power of 2");
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
            size_t const start = (_position + _shared_data.latency) & (buffer_size() - 1);
            samples.copy_to(make_block_view(_shared_data.buffer, start, samples.size()));
            _position = (_position + samples.size()) & (buffer_size() - 1);
        }

        IV_FORCEINLINE constexpr void push_block(BlockView<Sample const> samples)
        {
            size_t const start = (_position + _shared_data.latency) & (buffer_size() - 1);
            samples.copy_to(make_block_view(_shared_data.buffer, start, samples.size()));
            _position = (_position + samples.size()) & (buffer_size() - 1);
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

}
