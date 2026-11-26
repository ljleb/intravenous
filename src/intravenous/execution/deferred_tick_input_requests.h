#pragma once

#include <intravenous/node/tick_input_request.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

namespace iv {

// One foreground writer and one background reader share this queue. Position
// runs name source coordinates. Execution ranges count rendered samples and
// continue across seeks. A result's ticket belongs to the generation that
// owns the queue.
class DeferredTickInputRequests {
public:
    using Ticket = std::uint64_t;

    struct Capacity {
        std::size_t callbacks = 0;
        std::size_t requests = 0;
        std::size_t runs = 0;
        std::size_t positions = 0;
        std::size_t ports = 0;
    };

    using Request = TickInputRequest;

    struct Batch {
        Ticket begin = 0;
        Ticket end = 0;

        [[nodiscard]] bool empty() const noexcept { return begin == end; }
    };

private:
    struct Frame {
        std::size_t request_begin = 0;
        std::size_t request_count = 0;
        std::size_t run_end = 0;
        std::size_t position_end = 0;
        GlobalIndex first_due{};
        GlobalIndex last_due_end{};
    };

    template<class Value>
    struct Storage {
        std::vector<Value> values;
        std::size_t begin = 0;
        std::size_t end = 0;

        // Don't split a borrowed span across the ring seam. A caller can
        // size the shared reserve with slack for the largest single request.
        [[nodiscard]] std::optional<std::size_t> reserve(std::size_t count) const noexcept
        {
            if (count == 0) return end;
            auto const available = values.size() - (end - begin);
            if (count > available) return std::nullopt;
            auto const until_wrap = values.size() - (end & (values.size() - 1));
            if (until_wrap >= count) return end;
            if (until_wrap > available - count) return std::nullopt;
            return end + until_wrap;
        }

        std::span<Value> store(std::size_t at, std::span<Value const> source) noexcept
        {
            if (source.empty()) return {};
            auto target = std::span{values}.subspan(at & (values.size() - 1), source.size());
            std::ranges::copy(source, target.begin());
            end = at + source.size();
            return target;
        }
    };

    std::uint64_t generation_ = 0;
    std::vector<Frame> frames_{};
    Storage<Request> requests_{};
    Storage<SamplePositionRun> runs_{};
    Storage<GlobalIndex> positions_{};
    struct PortRequests {
        Request* last = nullptr;
        Request const* next_due = nullptr;
    };
    std::vector<PortRequests> ports_{};
    std::atomic<Ticket> published_{0};
    std::atomic<Ticket> prepared_{0};
    std::atomic<GlobalIndex::storage_type> rendered_{0};
    Ticket retired_ = 0;
    Ticket adopted_ = 0;
    Ticket writing_ = 0;
    Frame* open_ = nullptr;
    GlobalIndexRange callback_execution_{};
    bool submission_failed_ = false;

    [[nodiscard]] Frame& frame(Ticket ticket) noexcept
    {
        return frames_[ticket % frames_.size()];
    }

    [[nodiscard]] Frame const& frame(Ticket ticket) const noexcept
    {
        return frames_[ticket % frames_.size()];
    }

public:
    DeferredTickInputRequests(std::uint64_t generation, Capacity capacity)
        : generation_(generation), ports_(capacity.ports)
    {
        if (!std::has_single_bit(capacity.callbacks) || !std::has_single_bit(capacity.requests)
            || (capacity.runs && !std::has_single_bit(capacity.runs))
            || (capacity.positions && !std::has_single_bit(capacity.positions))
            || (!capacity.runs && !capacity.positions)) {
            throw std::invalid_argument("invalid deferred Tick input request capacity");
        }
        frames_.resize(capacity.callbacks);
        requests_.values.resize(capacity.requests);
        runs_.values.resize(capacity.runs);
        positions_.values.resize(capacity.positions);
    }

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    // Foreground. Reserve before entering the root so a full queue can stall
    // the callback without executing any node or advancing the playhead.
    [[nodiscard]] bool begin_callback(GlobalIndexRange execution) noexcept
    {
        if (execution.empty() || execution.extent >= (Ticket{1} << 63)
            || open_ || writing_ - retired_ == frames_.size()) return false;
        callback_execution_ = execution;
        submission_failed_ = false;
        open_ = &frame(writing_);
        open_->request_begin = requests_.end;
        open_->request_count = 0;
        return true;
    }

    [[nodiscard]] bool submit(Request request) noexcept
    {
        auto const positions = request.positions;
        if (!open_ || request.port >= ports_.size() || request.execution.empty()
            || !callback_execution_.contains(request.execution)
            || callback_execution_.end().forward_distance_to(request.first_due) >= (Ticket{1} << 63)
            || !std::ranges::all_of(positions.runs, &SamplePositionRun::valid)) {
            submission_failed_ = true;
            return false;
        }
        auto& port = ports_[request.port];
        if (port.last && port.last->due_range() != request.due_range()
            && port.last->due_range().end().signed_distance_to(request.first_due) < 0) {
            submission_failed_ = true;
            return false;
        }
        auto const request_at = requests_.reserve(1);
        auto const runs_at = runs_.reserve(positions.runs.size());
        auto const points_at = positions_.reserve(positions.individual_positions.size());
        if (!request_at || !runs_at || !points_at) {
            submission_failed_ = true;
            return false;
        }
        // An empty request still has a deadline. It records that the callback
        // asked for no values, which differs from a request lost in transit.
        request.positions = {.runs = runs_.store(*runs_at, positions.runs),
            .individual_positions = positions_.store(*points_at, positions.individual_positions)};
        auto const begin = callback_execution_.begin;
        auto const due_end = request.first_due.advanced_by(request.execution.extent);
        if (open_->request_count == 0
            || begin.forward_distance_to(request.first_due) < begin.forward_distance_to(open_->first_due)) {
            open_->first_due = request.first_due;
        }
        if (open_->request_count == 0
            || begin.forward_distance_to(due_end) > begin.forward_distance_to(open_->last_due_end)) {
            open_->last_due_end = due_end;
        }
        request.next_for_port = nullptr;
        auto& stored = requests_.store(*request_at, {&request, 1}).front();
        if (port.last) port.last->next_for_port = &stored;
        if (!port.next_due) port.next_due = &stored;
        port.last = &stored;
        ++open_->request_count;
        return true;
    }

    // Foreground only, after admitting the callback. The selected snapshot
    // covers every due request before the callback begins.
    [[nodiscard]] TickInputDueRequests due(std::size_t input, GlobalIndexRange execution) noexcept
    {
        if (input >= ports_.size() || execution.empty()) return {};
        auto& next = ports_[input].next_due;
        while (next && next->due_range().end().signed_distance_to(execution.begin) >= 0)
            next = next->next_for_port;
        std::size_t count = 0;
        for (auto const* request = next;
             request && request->first_due.signed_distance_to(execution.end()) > 0;
             request = request->next_for_port)
            ++count;
        return {next, count};
    }

    [[nodiscard]] bool submission_failed() const noexcept { return submission_failed_; }

    // Foreground. Only a complete callback becomes visible to the worker.
    [[nodiscard]] bool publish_callback() noexcept
    {
        if (!open_) return false;
        auto const nonempty = open_->request_count != 0;
        open_->run_end = runs_.end;
        open_->position_end = positions_.end;
        open_ = nullptr;
        if (nonempty) published_.store(++writing_, std::memory_order_release);
        return nonempty;
    }

    // Background. A failed transaction can retry this exact prefix. Producer
    // writes after selection never enter the selected transaction.
    [[nodiscard]] Batch select() const noexcept
    {
        return {prepared_.load(std::memory_order_relaxed), published_.load(std::memory_order_acquire)};
    }

    [[nodiscard]] auto requests(Ticket ticket) const noexcept
    {
        auto const& selected = frame(ticket);
        return std::views::iota(std::size_t{0}, selected.request_count)
            | std::views::transform([this, begin = selected.request_begin](std::size_t offset) -> Request const& {
                  return requests_.values[(begin + offset) & (requests_.values.size() - 1)];
              });
    }

    // Background. Publish a snapshot carrying batch.end after its sparse
    // materialization commits. Slot reclamation belongs to the foreground.
    [[nodiscard]] bool complete(Batch batch) noexcept
    {
        if (batch.begin != prepared_.load(std::memory_order_relaxed)
            || batch.end - batch.begin > published_.load(std::memory_order_acquire) - batch.begin) {
            return false;
        }
        prepared_.store(batch.end, std::memory_order_release);
        return true;
    }

    // Foreground. A newer worker result cannot make an older pinned view ready.
    [[nodiscard]] bool adopt(Ticket ticket) noexcept
    {
        if (ticket - adopted_ > prepared_.load(std::memory_order_acquire) - adopted_) return false;
        adopted_ = ticket;
        return true;
    }

    // Foreground. A result can have been adopted well before its final Tick
    // callback. Keep the request until the last sample using it renders.
    void release_rendered(GlobalIndex execution_end) noexcept
    {
        rendered_.store(execution_end.bits(), std::memory_order_release);
        while (retired_ != adopted_) {
            auto const& selected = frame(retired_);
            if (selected.last_due_end.forward_distance_to(execution_end) >= (Ticket{1} << 63)) break;
            for (auto const& request : requests(retired_)) {
                auto& port = ports_[request.port];
                if (port.next_due == &request) port.next_due = request.next_for_port;
                if (port.last == &request) port.last = nullptr;
            }
            requests_.begin = selected.request_begin + selected.request_count;
            runs_.begin = selected.run_end;
            positions_.begin = selected.position_end;
            ++retired_;
        }
    }

    [[nodiscard]] GlobalIndex rendered_through() const noexcept
    {
        return GlobalIndex::from_bits(rendered_.load(std::memory_order_acquire));
    }

    [[nodiscard]] bool ready_until(GlobalIndex execution_end) const noexcept
    {
        for (auto ticket = adopted_; ticket != writing_; ++ticket) {
            auto const distance = frame(ticket).first_due.forward_distance_to(execution_end);
            if (distance != 0 && distance < (Ticket{1} << 63)) return false;
        }
        return true;
    }
};

} // namespace iv
