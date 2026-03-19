#pragma once
#include <array>
#include <cstdint>
#include <bit>


namespace iv {
    template <size_t quad_words>
    class FastBitset {
        static_assert(quad_words > 0, "Need at least one word");
        std::array<uint64_t, quad_words> data_{};

    public:
        void set(size_t pos) {
            auto& w = data_[pos >> 6];
            w |= uint64_t(1) << (pos & 63);
        }
        void reset(size_t pos) {
            auto& w = data_[pos >> 6];
            w &= ~(uint64_t(1) << (pos & 63));
        }
        bool test(size_t pos) const {
            return (data_[pos >> 6] >> (pos & 63)) & 1;
        }
        void clear() {
            data_.fill(0);
        }

        // forward‑only const iterator
        class const_iterator {
            const FastBitset* bs_;
            size_t word_idx_;
            uint64_t word_;
            size_t idx_;

            void advance_to_next() {
                // fill word_ with next nonzero or mark end
                while (word_ == 0 && ++word_idx_ < quad_words) {
                    word_ = bs_->data_[word_idx_];
                }
                if (word_ != 0) {
                    unsigned tz = std::countr_zero(word_);
                    idx_ = word_idx_ * 64 + tz;
                    word_ &= word_ - 1;
                }
                else {
                    // mark end
                    bs_ = nullptr;
                }
            }

        public:
            // end iterator
            const_iterator() : bs_(nullptr), word_idx_(0), word_(0), idx_(0) {}
            // begin iterator
            explicit const_iterator(const FastBitset* bs)
                : bs_(bs), word_idx_(0), word_(bs->data_[0]), idx_(0)
            {
                if (word_ == 0) advance_to_next();
                else {
                    unsigned tz = std::countr_zero(word_);
                    idx_ = tz;
                    word_ &= word_ - 1;
                }
            }

            size_t operator*() const { return idx_; }

            const_iterator& operator++() {
                if (!bs_) return *this;
                advance_to_next();
                return *this;
            }

            bool operator!=(const const_iterator& o) const {
                return bs_ != o.bs_;
            }
        };

        const_iterator begin() const { return const_iterator(this); }
        const_iterator end()   const { return const_iterator(); }
    };
}
