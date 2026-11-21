#pragma once

#include <intravenous/node/coverage_task.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv {

// One selected transaction owns this scheduler. Request creation, successful
// reads and explicit waits are separate operations. Nothing here waits on a
// thread, a live producer queue, or a newer data version.
class BackgroundPlanningScheduler {
public:
    struct Operation;
    struct PendingRead { CoverageRequestId request; };

    struct Request {
        Operation* operation = nullptr;
        std::function<bool()> resolve{};
        std::function<void const*()> value{};
        std::string description{};
        bool ready = false;
        bool resolving = false;
        bool read = false;
    };

    struct Operation {
        BackgroundPlanningScheduler* scheduler = nullptr;
        void* group = nullptr;
        std::size_t node = 0;
        bool reverse = false;
        bool queued = false;
        bool executing = false;
        bool complete = false;
        bool restart = false;
        CoverageTaskHandle task{};
        std::vector<CoverageRequestId> waiting{};
        std::vector<CoverageRequestId> reads{};
        std::vector<CoverageRequestId> requests{};
        // A group dispatcher preserves the existing synchronous batch roots.
        std::function<void(std::span<Operation* const>)> dispatch{};
        std::function<void()> yielded{};
        ~Operation() { task.reset(); }
    };

private:
    std::deque<Request> requests_{};
    std::vector<std::unique_ptr<Operation>> operations_{};
    std::size_t progress_ = 0;

    [[nodiscard]] bool prerequisites_ready(Operation const& operation) const {
        return std::ranges::all_of(operation.waiting, [&](auto id) {
            return requests_.at(id).ready;
        });
    }

    void yielded(Operation& operation) {
        ++progress_;
        if (operation.restart) {
            operation.task.reset();
            operation.waiting.clear();
            operation.restart = false;
            return;
        }
        if (operation.task.frame) {
            try { operation.task.rethrow(operation.task.frame); }
            catch (PendingRead const&) {
                throw std::logic_error("suspendable coverage planner must await unresolved input information");
            }
            if (operation.task.done(operation.task.frame)) {
                operation.task.reset();
                operation.complete = true;
            } else {
                auto pending = operation.task.waiting(operation.task.frame);
                operation.waiting.assign(pending.begin(), pending.end());
                if (operation.waiting.empty()) {
                    throw std::logic_error("coverage task suspended without an information prerequisite");
                }
                for (auto id : operation.waiting) {
                    if (id >= requests_.size() || requests_[id].operation != &operation) {
                        throw std::logic_error("coverage task awaited a request from another operation");
                    }
                }
            }
        } else {
            operation.complete = true;
        }
        if (operation.yielded) operation.yielded();
    }

public:
    BackgroundPlanningScheduler() = default;
    BackgroundPlanningScheduler(BackgroundPlanningScheduler const&) = delete;
    BackgroundPlanningScheduler& operator=(BackgroundPlanningScheduler const&) = delete;
    ~BackgroundPlanningScheduler() { cancel(); }

    void cancel() noexcept {
        // A frame can retain request views and callback-context references.
        // Destroy every frame before releasing the request owners below.
        for (auto& operation : operations_) operation->task.reset();
        operations_.clear();
        requests_.clear();
    }

    Operation& add_operation(void* group, std::size_t node, bool reverse,
        std::function<void(std::span<Operation* const>)> dispatch,
        std::function<void()> on_yield = {}) {
        auto operation = std::make_unique<Operation>();
        operation->scheduler = this;
        operation->group = group;
        operation->node = node;
        operation->reverse = reverse;
        operation->dispatch = std::move(dispatch);
        operation->yielded = std::move(on_yield);
        operations_.push_back(std::move(operation));
        return *operations_.back();
    }

    void retire(void* group) noexcept {
        for (auto& operation : operations_) {
            if (operation->group != group) continue;
            operation->task.reset();
            operation->waiting.clear();
            operation->queued = false;
            operation->complete = true;
            operation->dispatch = {};
            operation->yielded = {};
        }
        // Request owners remain alive through all suspended consumers. The
        // caller's group must finish its own tasks before retiring its context.
    }

    void schedule(Operation& operation) {
        for (auto id : operation.requests) {
            requests_[id].ready = false;
            requests_[id].read = false;
        }
        operation.reads.clear();
        auto const was_complete = operation.complete;
        operation.queued = true;
        operation.complete = false;
        if (operation.executing && !was_complete && !operation.task.frame) return;
        if (operation.executing) { operation.restart = true; return; }
        operation.task.reset();
        operation.waiting.clear();
    }

    [[nodiscard]] CoverageRequestId request(Operation& operation,
        std::function<bool()> resolve, std::function<void const*()> value,
        std::string description = {}) {
        auto id = static_cast<CoverageRequestId>(requests_.size());
        requests_.push_back({.operation = &operation,
            .resolve = std::move(resolve), .value = std::move(value),
            .description = std::move(description)});
        operation.requests.push_back(id);
        return id;
    }

    void resolve_metadata(CoverageRequestId id) {
        auto& request = requests_.at(id);
        if (!request.ready) request.ready = request.resolve();
    }

    [[nodiscard]] bool ready(CoverageRequestId id) const { return requests_.at(id).ready; }

    [[nodiscard]] void const* read(CoverageRequestId id) {
        auto& request = requests_.at(id);
        if (!request.ready) throw PendingRead{id};
        if (!request.read) {
            request.operation->reads.push_back(id);
            request.read = true;
        }
        return request.value();
    }

    [[nodiscard]] CoverageRequestAccess access() noexcept {
        return {.owner = this,
            .ready = +[](void* owner, CoverageRequestId id) {
                return static_cast<BackgroundPlanningScheduler*>(owner)->ready(id);
            },
            .read = +[](void* owner, CoverageRequestId id) {
                return static_cast<BackgroundPlanningScheduler*>(owner)->read(id);
            }};
    }

    void finish(Operation& operation) {
        if (!operation.complete && !operation.task.frame) yielded(operation);
    }

    void submit(Operation& operation, CoverageTaskHandle task) {
        if (operation.task.frame) throw std::logic_error("coverage operation submitted two tasks");
        operation.task = task;
    }

    // Reentrant driving lets a discovery evaluation plan its own reverse
    // requirements while other suspended forward planners continue to progress.
    // Executing operations and resolving requests are never entered twice.
    void run_until(std::function<bool()> const& finished) {
        while (!finished()) {
            auto const before = progress_;
            auto const request_count = requests_.size();
            for (std::size_t index = 0; index < request_count; ++index) {
                auto& request = requests_[index];
                if (request.ready || request.resolving
                    || std::ranges::find(request.operation->waiting, index)
                        == request.operation->waiting.end()) continue;
                request.resolving = true;
                try { request.ready = request.resolve(); }
                catch (...) { request.resolving = false; throw; }
                request.resolving = false;
                if (request.ready) ++progress_;
            }
            // Dispatch ready synchronous operations together. Lazy coroutine
            // adapters submit frames here; their first resume is below.
            for (std::size_t index = 0; index < operations_.size(); ++index) {
                auto* first = operations_[index].get();
                if (!first->queued || first->complete || first->executing
                    || first->task.frame || !prerequisites_ready(*first)) continue;
                std::vector<Operation*> group;
                for (auto const& entry : operations_) {
                    auto* operation = entry.get();
                    if (operation->group == first->group && operation->reverse == first->reverse
                        && operation->queued && !operation->complete && !operation->executing
                        && !operation->task.frame && prerequisites_ready(*operation)) {
                        operation->executing = true;
                        operation->waiting.clear();
                        group.push_back(operation);
                    }
                }
                try {
                    first->dispatch(group);
                    for (auto* operation : group) {
                        operation->executing = false;
                        if (!operation->task.frame && !operation->complete) yielded(*operation);
                    }
                } catch (PendingRead const& pending) {
                    auto& request = requests_.at(pending.request);
                    request.operation->waiting = {pending.request};
                    for (auto* operation : group) operation->executing = false;
                    ++progress_;
                } catch (...) {
                    for (auto* operation : group) operation->executing = false;
                    throw;
                }
            }
            for (std::size_t index = 0; index < operations_.size(); ++index) {
                auto& operation = *operations_[index];
                if (!operation.queued || operation.complete || operation.executing
                    || !operation.task.frame || !prerequisites_ready(operation)) continue;
                operation.executing = true;
                operation.waiting.clear();
                try { operation.task.resume(operation.task.frame); }
                catch (...) { operation.executing = false; throw; }
                operation.executing = false;
                yielded(operation);
            }
            if (finished()) return;
            if (progress_ == before) {
                std::ostringstream error;
                error << "unresolved background information dependency cycle:";
                for (auto const& operation : operations_) {
                    if (!operation->queued || operation->complete) continue;
                    error << " node " << operation->node
                        << (operation->reverse ? " reverse" : " forward") << " waits";
                    for (auto id : operation->waiting) {
                        error << ' ' << id << '[' << requests_.at(id).description << ']';
                    }
                }
                throw std::runtime_error(error.str());
            }
        }
    }
};

} // namespace iv
