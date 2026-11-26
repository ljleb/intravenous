#pragma once

#include <intravenous/sample_positions.h>

#include <cstddef>
#include <iterator>

namespace iv {

struct TickInputRequest {
    std::size_t port = 0;
    GlobalIndexRange execution{};
    GlobalIndex first_due{};
    SamplePositionsView positions{};
    TickInputRequest const* next_for_port = nullptr;

    [[nodiscard]] GlobalIndexRange due_range() const noexcept
    {
        return {first_due, execution.extent};
    }
};

// Requests are borrowed for the duration of the callback. An empty range
// differs from a due request that explicitly selected no source positions.
struct TickInputDueRequests {
    struct Iterator {
        using value_type = TickInputRequest;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;

        TickInputRequest const* request = nullptr;
        std::size_t remaining = 0;

        TickInputRequest const& operator*() const noexcept { return *request; }
        TickInputRequest const* operator->() const noexcept { return request; }
        Iterator& operator++() noexcept
        {
            request = --remaining ? request->next_for_port : nullptr;
            return *this;
        }
        Iterator operator++(int) noexcept
        {
            auto before = *this;
            ++*this;
            return before;
        }
        bool operator==(Iterator const&) const noexcept = default;
    };

    TickInputRequest const* first = nullptr;
    std::size_t count = 0;

    [[nodiscard]] Iterator begin() const noexcept { return {count ? first : nullptr, count}; }
    [[nodiscard]] Iterator end() const noexcept { return {}; }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

struct TickInputRequestOutcome {
    bool submitted = false;
    TickInputDueRequests due{};
};

template<class Input>
struct TickInputRequestResult {
    // The new request and the results due now are independent. The input
    // view owns neither the due request records nor their prepared values.
    bool submitted;
    TickInputDueRequests due;
    Input input;
};

} // namespace iv
