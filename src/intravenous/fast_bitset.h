#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstddef>

namespace iv {
    template <size_t num_bits>
    class FastBitset {
        static_assert(num_bits > 0, "Need at least one bit");

        static constexpr size_t bits_per_word = 64;
        static constexpr size_t word_count = (num_bits + bits_per_word - 1) / bits_per_word;

        std::array<std::uint64_t, word_count> data_ {};

        static constexpr std::uint64_t last_word_mask()
        {
            constexpr size_t tail_bits = num_bits % bits_per_word;
            if constexpr (tail_bits == 0) {
                return ~std::uint64_t { 0 };
            } else {
                return (std::uint64_t { 1 } << tail_bits) - 1;
            }
        }

        constexpr void normalize()
        {
            data_.back() &= last_word_mask();
        }

    public:
        static constexpr size_t size() { return num_bits; }
        static constexpr size_t words() { return word_count; }

        constexpr void set(size_t pos)
        {
            auto& w = data_[pos >> 6];
            w |= std::uint64_t { 1 } << (pos & 63);
        }

        constexpr void reset(size_t pos)
        {
            auto& w = data_[pos >> 6];
            w &= ~(std::uint64_t { 1 } << (pos & 63));
        }

        constexpr bool test(size_t pos) const
        {
            return (data_[pos >> 6] >> (pos & 63)) & 1;
        }

        constexpr void clear()
        {
            data_.fill(0);
        }

        constexpr bool any() const
        {
            return begin() != end();
        }

        constexpr size_t first_set() const
        {
            auto it = begin();
            return it != end() ? *it : num_bits;
        }

        constexpr FastBitset operator~() const
        {
            FastBitset copy = *this;
            for (auto& word : copy.data_) {
                word = ~word;
            }
            copy.normalize();
            return copy;
        }

        constexpr FastBitset& operator&=(FastBitset const& other)
        {
            for (size_t i = 0; i < word_count; ++i) {
                data_[i] &= other.data_[i];
            }
            normalize();
            return *this;
        }

        constexpr FastBitset& operator|=(FastBitset const& other)
        {
            for (size_t i = 0; i < word_count; ++i) {
                data_[i] |= other.data_[i];
            }
            normalize();
            return *this;
        }

        friend constexpr FastBitset operator&(FastBitset lhs, FastBitset const& rhs)
        {
            lhs &= rhs;
            return lhs;
        }

        friend constexpr FastBitset operator|(FastBitset lhs, FastBitset const& rhs)
        {
            lhs |= rhs;
            return lhs;
        }

        class const_iterator {
            FastBitset const* bs_ = nullptr;
            size_t word_idx_ = 0;
            std::uint64_t word_ = 0;
            size_t idx_ = 0;

            constexpr void advance_to_next()
            {
                while (word_ == 0 && ++word_idx_ < word_count) {
                    word_ = bs_->data_[word_idx_];
                }
                if (word_ != 0) {
                    idx_ = word_idx_ * bits_per_word + std::countr_zero(word_);
                    word_ &= word_ - 1;
                } else {
                    bs_ = nullptr;
                }
            }

        public:
            constexpr const_iterator() = default;

            constexpr explicit const_iterator(FastBitset const* bs)
                : bs_(bs)
                , word_idx_(0)
                , word_(bs->data_[0])
                , idx_(0)
            {
                if (word_ == 0) {
                    advance_to_next();
                } else {
                    idx_ = std::countr_zero(word_);
                    word_ &= word_ - 1;
                }
            }

            constexpr size_t operator*() const { return idx_; }

            constexpr const_iterator& operator++()
            {
                if (bs_) {
                    advance_to_next();
                }
                return *this;
            }

            constexpr bool operator!=(const const_iterator& other) const
            {
                return bs_ != other.bs_;
            }
        };

        constexpr const_iterator begin() const { return const_iterator(this); }
        constexpr const_iterator end() const { return const_iterator(); }
    };
}
