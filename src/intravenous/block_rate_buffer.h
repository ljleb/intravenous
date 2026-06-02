#pragma once

#include "ports.h"

#include <algorithm>
#include <stdexcept>
#include <span>
#include <vector>

namespace iv {
    template<typename T>
    class BlockRateBuffer {
        std::vector<T> _storage;
        size_t _read_index = 0;
        size_t _write_index = 0;

        template<typename A>
        static BlockView<A> make_ring_view(std::span<A> buffer, size_t start, size_t count)
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

        void validate_count(size_t count) const
        {
            if (count > capacity()) {
                throw std::logic_error("BlockRateBuffer block size exceeds buffer capacity");
            }
        }

    public:
        BlockRateBuffer() = default;

        explicit BlockRateBuffer(size_t capacity)
        : _storage(next_power_of_2(capacity))
        {}

        size_t capacity() const
        {
            return _storage.size();
        }

        size_t available_count() const
        {
            return _write_index - _read_index;
        }

        size_t free_count() const
        {
            return capacity() - available_count();
        }

        bool can_allocate_write(size_t count) const
        {
            return count <= capacity() && count <= free_count();
        }

        bool can_read(size_t count) const
        {
            return count <= capacity() && count <= available_count();
        }

        BlockView<T> allocate_write(size_t count)
        {
            validate_count(count);
            if (!can_allocate_write(count)) {
                throw std::logic_error("BlockRateBuffer does not have enough free capacity");
            }

            auto block = make_ring_view(
                std::span<T>(_storage),
                _write_index & (capacity() - 1),
                count
            );
            _write_index += count;
            return block;
        }

        BlockView<T const> read_block(size_t count)
        {
            validate_count(count);
            if (!can_read(count)) {
                throw std::logic_error("BlockRateBuffer does not have enough readable data");
            }

            auto block = make_ring_view(
                std::span<T const>(_storage),
                _read_index & (capacity() - 1),
                count
            );
            _read_index += count;
            return block;
        }
    };
}
