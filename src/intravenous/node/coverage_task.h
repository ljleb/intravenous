#pragma once

// Background propagation only. These tasks never run on the audio thread.

#include <intravenous/coverage.h>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace iv {

using CoverageRequestId = std::uint64_t;

// An erased, package-owned coroutine frame. The candidate destroys the frame
// before its callback context, and both before releasing the package's code.
struct CoverageTaskHandle {
    void* frame = nullptr;
    void (*resume)(void*) = nullptr;
    bool (*done)(void const*) = nullptr;
    void (*rethrow)(void const*) = nullptr;
    std::span<CoverageRequestId const> (*waiting)(void const*) = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    void* context = nullptr;
    void (*destroy_context)(void*) noexcept = nullptr;

    void reset() noexcept {
        if (frame) destroy(frame);
        if (context) destroy_context(context);
        *this = {};
    }
};
static_assert(std::is_standard_layout_v<CoverageTaskHandle>);
static_assert(std::is_trivially_copyable_v<CoverageTaskHandle>);

class CoverageTask {
public:
    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;
    struct promise_type {
        std::exception_ptr failure{};
        std::vector<CoverageRequestId> prerequisites{};
        CoverageTask get_return_object() noexcept { return CoverageTask{Handle::from_promise(*this)}; }
        std::suspend_always initial_suspend() const noexcept { return {}; }
        std::suspend_always final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        void unhandled_exception() noexcept { failure = std::current_exception(); }
    };

    CoverageTask(CoverageTask const&) = delete;
    CoverageTask& operator=(CoverageTask const&) = delete;
    CoverageTask(CoverageTask&& other) noexcept : task_(std::exchange(other.task_, {})) {}
    CoverageTask& operator=(CoverageTask&& other) noexcept {
        if (this != &other) { task_.reset(); task_ = std::exchange(other.task_, {}); }
        return *this;
    }
    ~CoverageTask() { task_.reset(); }

    template<class Context>
    void own_context(Context* context) noexcept {
        task_.context = context;
        task_.destroy_context = +[](void* value) noexcept { delete static_cast<Context*>(value); };
    }

    [[nodiscard]] CoverageTaskHandle release() noexcept { return std::exchange(task_, {}); }

private:
    explicit CoverageTask(Handle handle) noexcept : task_{
        .frame = handle.address(),
        .resume = +[](void* frame) {
            auto task = Handle::from_address(frame);
            task.promise().prerequisites.clear();
            task.resume();
        },
        .done = +[](void const* frame) { return Handle::from_address(const_cast<void*>(frame)).done(); },
        .rethrow = +[](void const* frame) {
            auto const& failure = Handle::from_address(const_cast<void*>(frame)).promise().failure;
            if (failure) std::rethrow_exception(failure);
        },
        .waiting = +[](void const* frame) -> std::span<CoverageRequestId const> {
            return Handle::from_address(const_cast<void*>(frame)).promise().prerequisites;
        },
        .destroy = +[](void* frame) noexcept { Handle::from_address(frame).destroy(); },
    } {}
    CoverageTaskHandle task_{};
};

// Request creation does not block an operation. Only awaiting an unresolved
// request adds it to the coroutine's current scheduling prerequisites. Reading
// a successful result is separately recorded by the request's owning candidate.
struct CoverageRequestAccess {
    void* owner = nullptr;
    bool (*ready)(void*, CoverageRequestId) = nullptr;
    void const* (*read)(void*, CoverageRequestId) = nullptr;
};

enum class CoverageInformation { coverage, contents };

struct CoverageInputInfo {
    IndexCoverage coverage{};
    std::vector<GlobalIndexRange> regions{};
};

// One operation's access to its candidate. Port indices are local to the
// continuous/event input spans of that operation's reflected context.
struct CoveragePlanningContext {
    void* owner = nullptr;
    CoverageRequestAccess access{};
    CoverageRequestId (*request)(void*, bool event_input, std::size_t input,
        CoverageInformation, IndexCoverage const*) = nullptr;
    void (*start)(void*, CoverageTaskHandle) = nullptr;
    void (*completed)(void*) = nullptr;

    void finish() const { if (completed) completed(owner); }

    [[nodiscard]] CoverageRequestId request_input(bool event_input, std::size_t input,
        CoverageInformation information, IndexCoverage const* positions = nullptr) const {
        if (!request) throw std::logic_error("coverage input discovery requires a planning candidate");
        return request(owner, event_input, input, information, positions);
    }
    void submit(CoverageTask task) const {
        if (!start) throw std::logic_error("suspendable propagation requires a planning candidate");
        auto handle = task.release();
        try { start(owner, handle); }
        catch (...) { handle.reset(); throw; }
    }
};
static_assert(std::is_standard_layout_v<CoveragePlanningContext>);
static_assert(std::is_trivially_copyable_v<CoveragePlanningContext>);

template<class Value>
class CoverageRequest {
    CoverageRequestAccess access_{};
    CoverageRequestId id_ = 0;
public:
    CoverageRequest(CoverageRequestAccess access, CoverageRequestId id) : access_(access), id_(id) {}
    [[nodiscard]] CoverageRequestId id() const noexcept { return id_; }
    [[nodiscard]] bool ready() const { return access_.ready(access_.owner, id_); }
    [[nodiscard]] Value const& get() const {
        if (!ready()) throw std::logic_error("coverage request is not ready; await it before reading");
        return *static_cast<Value const*>(access_.read(access_.owner, id_));
    }
    [[nodiscard]] bool await_ready() const { return ready(); }
    void await_suspend(CoverageTask::Handle task) const { task.promise().prerequisites = {id_}; }
    [[nodiscard]] Value const& await_resume() const { return get(); }
};

template<class... Requests>
class CoverageRequestGroup {
    std::tuple<Requests...> requests_;
public:
    explicit CoverageRequestGroup(Requests... requests) : requests_(std::move(requests)...) {}
    [[nodiscard]] bool await_ready() const {
        return std::apply([](auto const&... requests) { return (requests.ready() && ...); }, requests_);
    }
    void await_suspend(CoverageTask::Handle task) const {
        auto& pending = task.promise().prerequisites;
        pending.clear();
        std::apply([&](auto const&... requests) {
            ([&] { if (!requests.ready()) pending.push_back(requests.id()); }(), ...);
        }, requests_);
    }
    void await_resume() const {}
};

template<class... Requests>
[[nodiscard]] auto when_all_coverage(Requests... requests) {
    return CoverageRequestGroup<Requests...>{std::move(requests)...};
}

} // namespace iv
