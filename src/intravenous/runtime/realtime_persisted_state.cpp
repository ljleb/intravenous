#include <intravenous/runtime/realtime_persisted_state.h>

#include <cassert>
#include <stdexcept>
#include <utility>

namespace iv {

RealtimePersistedState::RealtimePersistedState(
    std::uint64_t generation,
    PersistedPageStore::ReaderSlot page_slot,
    TickMaterializationStore::ReaderSlot materialization_slot) noexcept
    : generation_(generation)
    , page_slot_(std::move(page_slot))
    , materialization_slot_(std::move(materialization_slot))
{}

RealtimePersistedState::RealtimePersistedState(
    std::uint64_t generation,
    PersistedPageStore::ReaderSlot page_slot) noexcept
    : generation_(generation)
    , page_slot_(std::move(page_slot))
{}

std::unique_ptr<RealtimePersistedState> RealtimePersistedState::prepare(
    std::uint64_t generation,
    PersistedPageStore& pages,
    TickMaterializationStore& materializations)
{
    return std::unique_ptr<RealtimePersistedState>{
        new RealtimePersistedState{
            generation,
            pages.register_reader(),
            materializations.register_reader(),
        }};
}

std::unique_ptr<RealtimePersistedState>
RealtimePersistedState::prepare_initial(
    std::uint64_t generation,
    PersistedPageStore& pages)
{
    auto state = std::unique_ptr<RealtimePersistedState>{
        new RealtimePersistedState{generation, pages.register_reader()}};
    state->owned_materialization_ =
        std::make_unique<TickMaterializationSnapshot>();
    return state;
}

bool RealtimePersistedState::capture_initial() noexcept
{
    if (captured_ || !owned_materialization_) return false;
    pages_ = page_slot_.pin();
    auto const version = pages_.snapshot().version();
    owned_materialization_->generation_ = generation_;
    owned_materialization_->semantic_version_ = version.semantic;
    owned_materialization_->pages_ = version;
    captured_ = true;
    return true;
}

std::expected<void, std::string>
RealtimePersistedState::capture_current()
{
    if (captured_) {
        return std::unexpected(
            "realtime persisted state has already captured its roots");
    }
    pages_ = page_slot_.pin();
    materialization_ = materialization_slot_.pin();
    captured_ = true;
    if (materialization().generation() != generation_) {
        return std::unexpected(
            "realtime persisted state has an incompatible materialization generation");
    }
    if (materialization().pages() != pages().version()) {
        return std::unexpected(
            "realtime persisted state page and materialization roots disagree");
    }
    return {};
}

RealtimePersistedStateMailbox::RealtimePersistedStateMailbox()
{
    static_assert(
        std::atomic<RealtimePersistedState*>::is_always_lock_free);
}

RealtimePersistedStateMailbox::~RealtimePersistedStateMailbox()
{
    delete pending_.exchange(nullptr, std::memory_order_relaxed);
    delete std::exchange(active_, nullptr);
    auto* returned = returned_.exchange(nullptr, std::memory_order_relaxed);
    while (returned) {
        auto* next = returned->returned_next_;
        delete returned;
        returned = next;
    }
}

void RealtimePersistedStateMailbox::publish(
    std::unique_ptr<RealtimePersistedState> state)
{
    if (!state) {
        throw std::invalid_argument(
            "cannot publish an empty realtime persisted state");
    }
    if (!state->captured_) {
        throw std::logic_error(
            "cannot publish an uncaptured realtime persisted state");
    }
    auto* superseded = pending_.exchange(
        state.release(), std::memory_order_acq_rel);
    if (superseded) return_for_reclamation(*superseded);
}

void RealtimePersistedStateMailbox::activate_generation(
    std::unique_ptr<RealtimePersistedState> initial_state)
{
    if (!initial_state) {
        throw std::invalid_argument(
            "cannot activate an empty realtime persisted state");
    }
    if (!initial_state->captured_) {
        throw std::logic_error(
            "cannot activate an uncaptured realtime persisted state");
    }
    auto* superseded_pending = pending_.exchange(
        nullptr, std::memory_order_acquire);
    if (superseded_pending) {
        return_for_reclamation(*superseded_pending);
    }
    if (active_) return_for_reclamation(*active_);
    active_ = initial_state.release();
}

void RealtimePersistedStateMailbox::return_for_reclamation(
    RealtimePersistedState& state) noexcept
{
    auto* head = returned_.load(std::memory_order_relaxed);
    do {
        state.returned_next_ = head;
    } while (!returned_.compare_exchange_weak(
        head,
        &state,
        std::memory_order_release,
        std::memory_order_relaxed));
}

RealtimePersistedState const* RealtimePersistedStateMailbox::adopt(
    std::uint64_t generation) noexcept
{
    auto* selected = pending_.exchange(nullptr, std::memory_order_acquire);
    if (selected && selected->generation() != generation) {
        return_for_reclamation(*selected);
        selected = nullptr;
    }
    if (selected) {
        if (active_) return_for_reclamation(*active_);
        active_ = selected;
    }
    return active_ && active_->generation() == generation ? active_ : nullptr;
}

std::size_t RealtimePersistedStateMailbox::reclaim_returned() noexcept
{
    auto* returned = returned_.exchange(nullptr, std::memory_order_acquire);
    std::size_t reclaimed = 0;
    while (returned) {
        auto* next = returned->returned_next_;
        delete returned;
        returned = next;
        ++reclaimed;
    }
    return reclaimed;
}

} // namespace iv
