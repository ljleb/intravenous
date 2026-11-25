#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace iv::graph_jit {

enum class SampleFeedbackDelivery : std::uint64_t { fresh, delivered, absent };

// A ring of finalized Sample deliveries is indexed in the consumer's time.
// The tags distinguish producer values, authored gaps, and positions where
// the detach's initial value applies. Callers own the initialized backing
// storage and the queue ends.
class SampleFeedbackQueue {
    static constexpr auto mixed = static_cast<SampleFeedbackDelivery>(3);

    std::span<std::uint64_t> contents_;
    std::size_t capacity_;

    static constexpr std::size_t nodes_per_word = 32;

    [[nodiscard]] SampleFeedbackDelivery contents(std::size_t node) const noexcept
    {
        auto const shift = 2 * (node % nodes_per_word);
        return static_cast<SampleFeedbackDelivery>((contents_[node / nodes_per_word] >> shift) & 3);
    }

    void set_contents(std::size_t node, SampleFeedbackDelivery value) noexcept
    {
        auto const shift = 2 * (node % nodes_per_word);
        auto& word = contents_[node / nodes_per_word];
        word = (word & ~(std::uint64_t{3} << shift))
            | (static_cast<std::uint64_t>(value) << shift);
    }

    void set_range(std::size_t node, std::size_t left, std::size_t right,
        std::size_t begin, std::size_t end, SampleFeedbackDelivery value) noexcept
    {
        auto const previous = contents(node);
        if (previous == value) return;
        if (begin <= left && right <= end) {
            set_contents(node, value);
            return;
        }

        auto const middle = left + (right - left) / 2;
        auto const first_child = 2 * node;
        auto const second_child = first_child + 1;
        // Uniform intervals hide their descendants. Materialize only the two
        // child tags when splitting one, so a large fresh gap costs O(log N).
        if (previous != mixed) {
            set_contents(first_child, previous);
            set_contents(second_child, previous);
        }
        if (begin < middle) set_range(first_child, left, middle, begin, end, value);
        if (end > middle) set_range(second_child, middle, right, begin, end, value);
        auto const first = contents(first_child);
        auto const second = contents(second_child);
        set_contents(node, first == second ? first : mixed);
    }

    void mark(std::uint64_t first, std::size_t frames, SampleFeedbackDelivery value) noexcept
    {
        assert(frames <= capacity_);
        if (frames == 0) return;
        auto const begin = static_cast<std::size_t>(first & (capacity_ - 1));
        auto const before_wrap = std::min(frames, capacity_ - begin);
        set_range(1, 0, capacity_, begin, begin + before_wrap, value);
        if (before_wrap < frames)
            set_range(1, 0, capacity_, 0, frames - before_wrap, value);
    }

    template<class Visitor>
    void visit(std::size_t node, std::size_t left, std::size_t right,
        std::size_t begin, std::size_t end, std::uint64_t first, Visitor& visitor) const
    {
        auto const value = contents(node);
        if (value != mixed) {
            auto const start = std::max(left, begin);
            auto const stop = std::min(right, end);
            visitor(first + (start - begin), stop - start, value);
            return;
        }

        auto const middle = left + (right - left) / 2;
        if (begin < middle) visit(2 * node, left, middle, begin, end, first, visitor);
        if (end > middle) visit(2 * node + 1, middle, right, begin, end, first, visitor);
    }

public:
    // A zero result means the capacity is invalid. The Sample ring requires
    // the same power-of-two capacity. Each frame adds four bits of metadata.
    [[nodiscard]] static constexpr std::size_t required_words(std::size_t capacity) noexcept
    {
        if (!std::has_single_bit(capacity)
            || capacity > std::numeric_limits<std::size_t>::max() / 2) return 0;
        return (2 * capacity - 1) / nodes_per_word + 1;
    }

    SampleFeedbackQueue(std::span<std::uint64_t> contents, std::size_t capacity) noexcept
        : contents_(contents), capacity_(capacity)
    {
        assert(required_words(capacity) != 0 && required_words(capacity) <= contents.size());
    }

    void clear() noexcept { set_contents(1, SampleFeedbackDelivery::fresh); }

    // The surviving prefix stays at its original destination positions.
    // Growing makes the new tail fresh. Shrinking drops the old tail, so the
    // next producer write can start earlier. A later grow cannot resurrect it.
    void retime(std::uint64_t next_input, std::size_t previous_delay,
        std::size_t next_delay) noexcept
    {
        assert(previous_delay < capacity_ && next_delay < capacity_);
        auto const retained = std::min(previous_delay, next_delay);
        auto const affected = std::max(previous_delay, next_delay) - retained;
        mark(next_input + retained, affected, SampleFeedbackDelivery::fresh);
    }

    void mark_delivered(std::uint64_t first, std::size_t frames) noexcept
    {
        mark(first, frames, SampleFeedbackDelivery::delivered);
    }

    void mark_absent(std::uint64_t first, std::size_t frames) noexcept
    {
        mark(first, frames, SampleFeedbackDelivery::absent);
    }

    // Visit uniform ranges in playback order. The visitor receives the
    // absolute destination position, count and delivery kind.
    template<class Visitor>
    void visit_deliveries(std::uint64_t first, std::size_t frames, Visitor&& visitor) const
    {
        assert(frames <= capacity_);
        if (frames == 0) return;
        auto const begin = static_cast<std::size_t>(first & (capacity_ - 1));
        auto const before_wrap = std::min(frames, capacity_ - begin);
        visit(1, 0, capacity_, begin, begin + before_wrap, first, visitor);
        if (before_wrap < frames)
            visit(1, 0, capacity_, 0, frames - before_wrap, first + before_wrap, visitor);
    }
};

} // namespace iv::graph_jit
