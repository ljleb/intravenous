#pragma once

// Public, execution-independent background sample/event port and callback API.
// The executor owns coverage, requests, result storage, and retained records; these
// views only expose the storage selected for one node invocation.

#include <intravenous/coverage.h>
#include <intravenous/node/static_port_access.h>
#include <intravenous/node/traits.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace iv {

namespace background_port_details {
    inline IndexCoverage const& bound_coverage(
        IndexCoverage const* coverage) noexcept
    {
        IV_ASSERT(coverage != nullptr,
            "background port/context has no coverage binding");
        return *coverage;
    }

    template<typename State>
    std::add_lvalue_reference_t<State> state_from(
        std::span<std::byte> storage)
    requires (!std::is_void_v<State>)
    {
        void* pointer = storage.data();
        std::size_t space = storage.size();
        void* const aligned = std::align(
            alignof(State), sizeof(State), pointer, space);
        IV_ASSERT(aligned != nullptr,
            "background state storage does not contain the node TockState");
        return *static_cast<State*>(aligned);
    }
}

struct RandomAccessContinuousInputPort {
    ContinuousRegionStorageView const* regions = nullptr;
    std::size_t region_count = 0;
    IndexCoverage const* coverage_value = nullptr;
    ContinuousValueStorageGeometry geometry{};

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return background_port_details::bound_coverage(coverage_value);
    }

    [[nodiscard]] std::span<ContinuousRegionStorageView const>
    region_views() const noexcept
    {
        return {regions, region_count};
    }
};

struct TockContinuousOutputPort {
    ContinuousRegionStorageView const* regions = nullptr;
    std::size_t region_count = 0;
    IndexCoverage const* requested_coverage_value = nullptr;
    ContinuousValueStorageGeometry geometry{};

    [[nodiscard]] IndexCoverage const& requested_coverage() const noexcept
    {
        return background_port_details::bound_coverage(requested_coverage_value);
    }

    [[nodiscard]] std::span<ContinuousRegionStorageView const>
    region_views() const noexcept
    {
        return {regions, region_count};
    }
};

// Each entry is one logical Region. Entries may overlap or have identical
// ranges; their event storage remains independent. The provider invokes the
// visitor synchronously in timestamp order within the selected Region.
struct RandomAccessEventInputPort {
    using VisitEvent = void (*)(void*, TimedEvent const&);
    using ForEachEvent = void (*)(
        void const*, std::size_t, GlobalIndexRange, void*, VisitEvent);

    void const* data = nullptr;
    GlobalIndexRange const* region_ranges = nullptr;
    std::size_t region_count = 0;
    IndexCoverage const* coverage_value = nullptr;
    ForEachEvent for_each_event = nullptr;

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return background_port_details::bound_coverage(coverage_value);
    }

    [[nodiscard]] std::span<GlobalIndexRange const> regions() const noexcept
    {
        return {region_ranges, region_count};
    }

    template<typename Fn>
    void for_each(
        std::size_t region_index, GlobalIndexRange requested, Fn&& fn) const
    {
        IV_ASSERT(region_index < region_count,
            "background event input Region is out of bounds");
        IV_ASSERT(region_ranges[region_index].contains(requested),
            "background event input read lies outside its Region");
        if (requested.empty()) return;
        IV_ASSERT(for_each_event != nullptr,
            "background event input Region has no readable storage");
        using Visitor = std::remove_reference_t<Fn>;
        for_each_event(
            data,
            region_index,
            requested,
            std::addressof(fn),
            +[](void* opaque, TimedEvent const& event) {
                (*static_cast<Visitor*>(opaque))(event);
            });
    }

};

struct TockEventOutputPort {
    void* data = nullptr;
    GlobalIndexRange const* region_ranges = nullptr;
    std::size_t region_count = 0;
    IndexCoverage const* requested_coverage_value = nullptr;
    void (*write_event)(void*, std::size_t, TimedEvent const&) = nullptr;

    [[nodiscard]] IndexCoverage const& requested_coverage() const noexcept
    {
        return background_port_details::bound_coverage(requested_coverage_value);
    }

    [[nodiscard]] std::span<GlobalIndexRange const> regions() const noexcept
    {
        return {region_ranges, region_count};
    }

    void write(std::size_t region_index, TimedEvent const& event) const noexcept
    {
        IV_ASSERT(region_index < region_count,
            "background event output Region is out of bounds");
        IV_ASSERT(region_ranges[region_index].contains(event.time),
            "background event output write lies outside its Region");
        IV_ASSERT(write_event != nullptr,
            "background event output Region has no writable storage");
        // The callback contract requires nondecreasing timestamps for each
        // output invocation. The concrete sink may validate that in debug
        // builds without imposing a release-time sorting pass here.
        write_event(data, region_index, event);
    }
};

struct InputCoverageChange {
    std::span<GlobalIndexRange const> const* region_ranges_view_value = nullptr;
    GlobalIndexRange const* region_ranges_value = nullptr;
    std::size_t region_range_count = 0;
    IndexCoverage const* coverage_value = nullptr;
    IndexCoverage const* changed_value = nullptr;

    [[nodiscard]] std::span<GlobalIndexRange const> regions() const noexcept
    {
        if (region_ranges_view_value != nullptr) {
            return *region_ranges_view_value;
        }
        return {region_ranges_value, region_range_count};
    }

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return background_port_details::bound_coverage(coverage_value);
    }

    [[nodiscard]] IndexCoverage const& changed() const noexcept
    {
        return background_port_details::bound_coverage(changed_value);
    }
};

struct OutputCoverageChange {
    using PublishRegions = void (*)(
        void*, GlobalIndexRange const*, std::size_t);
    using PublishCoverage = void (*)(void*, IndexCoverage const&);
    using PublishChange = void (*)(void*, IndexCoverage const&);

    void* data = nullptr;
    std::span<GlobalIndexRange const> const*
        previous_region_ranges_view_value = nullptr;
    GlobalIndexRange const* previous_region_ranges_value = nullptr;
    std::size_t previous_region_range_count = 0;
    IndexCoverage const* previous_coverage_value = nullptr;
    PublishRegions publish_regions_value = nullptr;
    PublishCoverage publish_coverage_value = nullptr;
    PublishChange publish_changed_value = nullptr;

    [[nodiscard]] IndexCoverage const& previous_coverage() const noexcept
    {
        return background_port_details::bound_coverage(previous_coverage_value);
    }

    [[nodiscard]] std::span<GlobalIndexRange const> previous_regions()
        const noexcept
    {
        if (previous_region_ranges_view_value != nullptr) {
            return *previous_region_ranges_view_value;
        }
        return {
            previous_region_ranges_value,
            previous_region_range_count,
        };
    }

    void publish_regions(
        std::span<GlobalIndexRange const> regions) const
    {
        IV_ASSERT(publish_regions_value != nullptr,
            "background forward context has no output-Region sink");
        publish_regions_value(data, regions.data(), regions.size());
    }

    void publish_region(GlobalIndexRange region) const
    {
        publish_regions(std::span{&region, std::size_t{1}});
    }

    // Internal positional publication for Tick-produced outputs. Authored
    // Region and Coverage outputs publish their Region list instead.
    void publish_coverage(IndexCoverage const& coverage) const
    {
        IV_ASSERT(publish_coverage_value != nullptr,
            "background forward context has no output-coverage sink");
        publish_coverage_value(data, coverage);
    }

    void change(IndexCoverage const& changed) const
    {
        if (changed.empty()) return;
        IV_ASSERT(publish_changed_value != nullptr,
            "background forward context has no output-change sink");
        publish_changed_value(data, changed);
    }

    void change(GlobalIndexRange changed) const
    {
        change(IndexCoverage{changed});
    }
};

struct OutputCoverageRequirement {
    IndexCoverage const* required_value = nullptr;

    [[nodiscard]] IndexCoverage const& required() const noexcept
    {
        return background_port_details::bound_coverage(required_value);
    }
};

struct InputCoverageRequirement {
    using PublishRequirement = void (*)(void*, IndexCoverage const&);

    void* data = nullptr;
    IndexCoverage const* coverage_value = nullptr;
    PublishRequirement publish_required_value = nullptr;

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return background_port_details::bound_coverage(coverage_value);
    }

    void require(IndexCoverage const& required) const
    {
        if (required.empty()) return;
        IV_ASSERT(publish_required_value != nullptr,
            "background reverse context has no input-requirement sink");
        publish_required_value(data, required);
    }

    void require(GlobalIndexRange required) const
    {
        require(IndexCoverage{required});
    }
};

namespace details {

class EventRegion {
    RandomAccessEventInputPort const* port_ = nullptr;
    std::size_t index_ = 0;

public:
    EventRegion() = default;
    EventRegion(
        RandomAccessEventInputPort const& port, std::size_t index) noexcept
        : port_(std::addressof(port)), index_(index)
    {
        IV_ASSERT(index < port.region_count,
            "event Region is out of bounds");
    }

    [[nodiscard]] GlobalIndexRange range() const noexcept
    {
        return port_->region_ranges[index_];
    }

    template<typename Fn>
    void for_each(Fn&& fn) const
    {
        port_->for_each(index_, range(), std::forward<Fn>(fn));
    }

    template<typename Fn>
    void for_each(GlobalIndexRange requested, Fn&& fn) const
    {
        port_->for_each(index_, requested, std::forward<Fn>(fn));
    }
};

class WritableEventRegion {
    TockEventOutputPort* port_ = nullptr;
    std::size_t index_ = 0;

public:
    WritableEventRegion() = default;
    WritableEventRegion(TockEventOutputPort& port, std::size_t index) noexcept
        : port_(std::addressof(port)), index_(index)
    {
        IV_ASSERT(index < port.region_count,
            "writable event Region is out of bounds");
    }

    [[nodiscard]] GlobalIndexRange range() const noexcept
    {
        return port_->region_ranges[index_];
    }

    void write(TimedEvent const& event) const noexcept
    {
        port_->write(index_, event);
    }
};

template<bool Mutable>
class EventCoverageView {
    using Port = std::conditional_t<
        Mutable, TockEventOutputPort, RandomAccessEventInputPort const>;
    Port* port_ = nullptr;

public:
    using region_type = std::conditional_t<
        Mutable, WritableEventRegion, EventRegion>;

    class iterator {
        Port* port_ = nullptr;
        std::size_t index_ = 0;

    public:
        using value_type = region_type;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;

        iterator() = default;
        iterator(Port* port, std::size_t index) noexcept
            : port_(port), index_(index)
        {}

        [[nodiscard]] region_type operator*() const noexcept
        {
            return region_type{*port_, index_};
        }

        iterator& operator++() noexcept
        {
            ++index_;
            return *this;
        }

        iterator operator++(int) noexcept
        {
            auto previous = *this;
            ++*this;
            return previous;
        }

        [[nodiscard]] friend bool operator==(
            iterator const&, iterator const&) = default;
    };

    EventCoverageView() = default;
    explicit EventCoverageView(Port& port) noexcept : port_(&port) {}

    [[nodiscard]] std::size_t size() const noexcept
    {
        return port_ ? port_->region_count : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] region_type operator[](std::size_t index) const noexcept
    {
        IV_ASSERT(index < size(), "event Coverage Region is out of bounds");
        return region_type{*port_, index};
    }

    [[nodiscard]] iterator begin() const noexcept
    {
        return iterator{port_, 0};
    }

    [[nodiscard]] iterator end() const noexcept
    {
        return iterator{port_, size()};
    }

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        IV_ASSERT(port_ != nullptr, "event Coverage has no positional union");
        if constexpr (Mutable) {
            return port_->requested_coverage();
        } else {
            return port_->coverage();
        }
    }
};

class StaticInputCoverageChange {
    InputCoverageChange const& change_;

public:
    explicit StaticInputCoverageChange(
        InputCoverageChange const& change) noexcept
        : change_(change)
    {}

    [[nodiscard]] std::span<GlobalIndexRange const> regions() const noexcept
    {
        return change_.regions();
    }

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return change_.coverage();
    }

    [[nodiscard]] IndexCoverage const& changed() const noexcept
    {
        return change_.changed();
    }
};

template<OutputProductionKind Production>
class StaticOutputCoverageChange {
    OutputCoverageChange const& change_;

public:
    explicit StaticOutputCoverageChange(
        OutputCoverageChange const& change) noexcept
        : change_(change)
    {}

    [[nodiscard]] IndexCoverage const& previous_coverage() const noexcept
    {
        return change_.previous_coverage();
    }

    [[nodiscard]] std::span<GlobalIndexRange const> previous_regions()
        const noexcept
    {
        return change_.previous_regions();
    }

    void publish(std::span<GlobalIndexRange const> regions) const
    requires (Production == OutputProductionKind::coverage)
    {
        change_.publish_regions(regions);
    }

    void publish(GlobalIndexRange region) const
    requires (Production == OutputProductionKind::region)
    {
        change_.publish_region(region);
    }

    void change(IndexCoverage const& changed) const
    {
        change_.change(changed);
    }

    void change(GlobalIndexRange changed) const
    {
        change_.change(changed);
    }
};

template<RegisteredContinuousValue Value, InputAccessKind Access>
class StaticBackgroundContinuousInputAccess {
    RandomAccessContinuousInputPort const* port_ = nullptr;

public:
    explicit constexpr StaticBackgroundContinuousInputAccess(
        RandomAccessContinuousInputPort const& port) noexcept
        : port_(&port)
    {}

    [[nodiscard]] constexpr IndexCoverage const& coverage() const noexcept
    {
        return port_->coverage();
    }

    [[nodiscard]] constexpr auto region() const noexcept
    requires (Access == InputAccessKind::region)
    {
        IV_ASSERT(port_->region_count == 1,
            "Region input does not contain exactly one Region");
        return ContinuousRegion<Value>{port_->regions[0]};
    }

    [[nodiscard]] constexpr auto regions() const noexcept
    requires (Access == InputAccessKind::coverage)
    {
        return ContinuousCoverage<Value>{
            port_->region_views(), port_->coverage()};
    }
};

template<class Tuple, InputAccessKind Access>
class StaticVariantBackgroundContinuousInputAccess;

template<class... Values, InputAccessKind Access>
class StaticVariantBackgroundContinuousInputAccess<
    std::tuple<Values...>, Access> {
    RandomAccessContinuousInputPort const* port_ = nullptr;

public:
    explicit constexpr StaticVariantBackgroundContinuousInputAccess(
        RandomAccessContinuousInputPort const& port) noexcept
        : port_(&port)
    {}

    template<RegisteredContinuousValue Value>
    [[nodiscard]] constexpr bool is() const noexcept
    requires ((std::same_as<Value, Values> || ...))
    {
        return port_->geometry.value_type == continuous_value_type_id_v<Value>;
    }

    template<RegisteredContinuousValue Value>
    [[nodiscard]] constexpr auto as() const noexcept
    requires ((std::same_as<Value, Values> || ...))
    {
        IV_ASSERT(is<Value>(),
            "continuous input alternative does not match its resolved type");
        return StaticBackgroundContinuousInputAccess<Value, Access>{*port_};
    }
};

template<RegisteredContinuousValue Value, OutputProductionKind Production>
class StaticBackgroundContinuousOutputAccess {
    TockContinuousOutputPort* port_ = nullptr;

public:
    explicit constexpr StaticBackgroundContinuousOutputAccess(
        TockContinuousOutputPort& port) noexcept
        : port_(&port)
    {}

    [[nodiscard]] constexpr IndexCoverage const& requested_coverage()
        const noexcept
    {
        return port_->requested_coverage();
    }

    [[nodiscard]] constexpr auto region() const noexcept
    requires (Production == OutputProductionKind::region)
    {
        IV_ASSERT(port_->region_count == 1,
            "Region output does not contain exactly one Region");
        return WritableContinuousRegion<Value>{port_->regions[0]};
    }

    [[nodiscard]] constexpr auto regions() const noexcept
    requires (Production == OutputProductionKind::coverage)
    {
        return WritableContinuousCoverage<Value>{
            port_->region_views(), port_->requested_coverage()};
    }
};

template<class Tuple, OutputProductionKind Production>
class StaticVariantBackgroundContinuousOutputAccess;

template<class... Values, OutputProductionKind Production>
class StaticVariantBackgroundContinuousOutputAccess<
    std::tuple<Values...>, Production> {
    TockContinuousOutputPort* port_ = nullptr;

public:
    explicit constexpr StaticVariantBackgroundContinuousOutputAccess(
        TockContinuousOutputPort& port) noexcept
        : port_(&port)
    {}

    template<RegisteredContinuousValue Value>
    [[nodiscard]] constexpr bool is() const noexcept
    requires ((std::same_as<Value, Values> || ...))
    {
        return port_->geometry.value_type == continuous_value_type_id_v<Value>;
    }

    template<RegisteredContinuousValue Value>
    [[nodiscard]] constexpr auto as() const noexcept
    requires ((std::same_as<Value, Values> || ...))
    {
        IV_ASSERT(is<Value>(),
            "continuous output alternative does not match its resolved type");
        return StaticBackgroundContinuousOutputAccess<Value, Production>{*port_};
    }
};

template<InputAccessKind Access>
class StaticBackgroundEventInputAccess {
    RandomAccessEventInputPort const* port_ = nullptr;

public:
    explicit StaticBackgroundEventInputAccess(RandomAccessEventInputPort const& port)
        : port_(std::addressof(port))
    {}

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return port_->coverage();
    }

    [[nodiscard]] EventRegion region() const noexcept
    requires (Access == InputAccessKind::region)
    {
        IV_ASSERT(port_->region_count == 1,
            "event Region input does not contain exactly one Region");
        return EventRegion{*port_, 0};
    }

    [[nodiscard]] EventCoverageView<false> regions() const noexcept
    requires (Access == InputAccessKind::coverage)
    {
        return EventCoverageView<false>{*port_};
    }
};

template<OutputProductionKind Production>
class StaticBackgroundEventOutputAccess {
    TockEventOutputPort* port_ = nullptr;

public:
    explicit StaticBackgroundEventOutputAccess(TockEventOutputPort& port)
        : port_(std::addressof(port))
    {}

    [[nodiscard]] IndexCoverage const& requested_coverage() const noexcept
    {
        return port_->requested_coverage();
    }

    [[nodiscard]] WritableEventRegion region() const noexcept
    requires (Production == OutputProductionKind::region)
    {
        IV_ASSERT(port_->region_count == 1,
            "event Region output does not contain exactly one Region");
        return WritableEventRegion{*port_, 0};
    }

    [[nodiscard]] EventCoverageView<true> regions() const noexcept
    requires (Production == OutputProductionKind::coverage)
    {
        return EventCoverageView<true>{*port_};
    }
};

template<typename Node, fixed_string Name>
constexpr std::size_t background_input_index()
{
    if constexpr (static_input_port_kind<Node, Name>() == PortKind::sample) {
        return static_random_access_input_port_index<Node, Name>();
    } else {
        return static_random_access_event_input_port_index<Node, Name>();
    }
}

} // namespace details

using EventRegion = details::EventRegion;
using WritableEventRegion = details::WritableEventRegion;
using EventCoverage = details::EventCoverageView<false>;
using WritableEventCoverage = details::EventCoverageView<true>;

template<typename Node>
struct TockCoverageContext {
    std::span<RandomAccessContinuousInputPort const> inputs {};
    std::span<TockContinuousOutputPort> outputs {};
    std::span<RandomAccessEventInputPort const> event_inputs {};
    std::span<TockEventOutputPort> event_outputs {};
    std::span<std::byte> background_state_storage {};
    std::size_t sample_rate = 48000;

    using TockState = typename NodeBackgroundState<Node>::Type;

    template<fixed_string Name>
    [[nodiscard]] auto input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(details::static_input_port_is_random_access<Node, Name>(),
            "TockCoverageContext can only access inputs declared background");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto access =
                details::static_input_port_access<Node, Name>();
            constexpr auto declaration =
                details::static_input_declaration_index<Node, Name>();
            using Config = std::remove_cvref_t<decltype(port_config_at<
                declaration>(std::declval<decltype(Node::inputs()) const&>()))>;
            using Values = typename Config::value_types;
            constexpr auto port_index =
                details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(port_index < inputs.size(),
                "background continuous input is absent from tock context");
            if constexpr (std::tuple_size_v<Values> == 1) {
                using Value = std::tuple_element_t<0, Values>;
                return details::StaticBackgroundContinuousInputAccess<
                    Value, access>{inputs[port_index]};
            } else {
                return details::StaticVariantBackgroundContinuousInputAccess<
                    Values, access>{inputs[port_index]};
            }
        } else {
            constexpr auto access =
                details::static_input_port_access<Node, Name>();
            constexpr auto port_index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(port_index < event_inputs.size(),
                "background event input is absent from tock context");
            return details::StaticBackgroundEventInputAccess<access>{
                event_inputs[port_index]};
        }
    }

    template<fixed_string Name>
    [[nodiscard]] auto output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "TockCoverageContext can only write background outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto production =
                details::static_output_port_production<Node, Name>();
            constexpr auto declaration =
                details::static_output_declaration_index<Node, Name>();
            using Config = std::remove_cvref_t<decltype(port_config_at<
                declaration>(std::declval<decltype(Node::outputs()) const&>()))>;
            using Values = typename Config::value_types;
            constexpr auto port_index =
                details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(port_index < outputs.size(),
                "background continuous output is absent from tock context");
            if constexpr (std::tuple_size_v<Values> == 1) {
                using Value = std::tuple_element_t<0, Values>;
                return details::StaticBackgroundContinuousOutputAccess<
                    Value, production>{outputs[port_index]};
            } else {
                return details::StaticVariantBackgroundContinuousOutputAccess<
                    Values, production>{outputs[port_index]};
            }
        } else {
            constexpr auto production =
                details::static_output_port_production<Node, Name>();
            constexpr auto port_index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(port_index < event_outputs.size(),
                "background event output is absent from tock context");
            return details::StaticBackgroundEventOutputAccess<production>{
                event_outputs[port_index]};
        }
    }

    [[nodiscard]] std::add_lvalue_reference_t<TockState> tock_state() const
    requires (!std::is_void_v<TockState>)
    {
        return background_port_details::state_from<TockState>(background_state_storage);
    }
};

template<typename Node>
struct PropagateForwardCoverageContext {
    std::span<InputCoverageChange const> inputs {};
    std::span<OutputCoverageChange> outputs {};
    std::span<InputCoverageChange const> event_inputs {};
    std::span<OutputCoverageChange> event_outputs {};
    bool local_state_changed = false;
    std::size_t sample_rate = 48000;

    template<fixed_string Name>
    [[nodiscard]] auto input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(details::static_input_port_is_random_access<Node, Name>(),
            "forward coverage can only inspect background inputs");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(index < inputs.size(),
                "background sample input is absent from forward context");
            return details::StaticInputCoverageChange{inputs[index]};
        } else {
            constexpr auto index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(index < event_inputs.size(),
                "background event input is absent from forward context");
            return details::StaticInputCoverageChange{event_inputs[index]};
        }
    }

    template<fixed_string Name>
    [[nodiscard]] auto output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "forward coverage can only publish background outputs");
        constexpr auto production =
            details::static_output_port_production<Node, Name>();
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(index < outputs.size(),
                "background sample output is absent from forward context");
            return details::StaticOutputCoverageChange<production>{
                outputs[index]};
        } else {
            constexpr auto index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(index < event_outputs.size(),
                "background event output is absent from forward context");
            return details::StaticOutputCoverageChange<production>{
                event_outputs[index]};
        }
    }

};

template<typename Node>
struct PropagateReverseCoverageContext {
    std::span<InputCoverageRequirement> inputs {};
    std::span<OutputCoverageRequirement const> outputs {};
    std::span<InputCoverageRequirement> event_inputs {};
    std::span<OutputCoverageRequirement const> event_outputs {};
    std::size_t sample_rate = 48000;

    template<fixed_string Name>
    [[nodiscard]] OutputCoverageRequirement const& output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "reverse coverage can only inspect background outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(index < outputs.size(),
                "background sample output is absent from reverse context");
            return outputs[index];
        } else {
            constexpr auto index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(index < event_outputs.size(),
                "background event output is absent from reverse context");
            return event_outputs[index];
        }
    }

    template<fixed_string Name>
    [[nodiscard]] InputCoverageRequirement const& input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(details::static_input_port_is_random_access<Node, Name>(),
            "reverse coverage can only require background inputs");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(index < inputs.size(),
                "background sample input is absent from reverse context");
            return inputs[index];
        } else {
            constexpr auto index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(index < event_inputs.size(),
                "background event input is absent from reverse context");
            return event_inputs[index];
        }
    }

};

namespace details {

template<typename Node, typename Context>
struct BackgroundBatchLaneBinding {
    Node const* node = nullptr;
    Context* context = nullptr;
};

template<typename Node, typename Context>
class BackgroundBatchLane {
    BackgroundBatchLaneBinding<Node, Context> const* binding_ = nullptr;

public:
    BackgroundBatchLane() = default;
    explicit BackgroundBatchLane(
        BackgroundBatchLaneBinding<Node, Context> const& binding) noexcept
        : binding_(std::addressof(binding))
    {}

    [[nodiscard]] Node const& node() const noexcept
    {
        IV_ASSERT(binding_ != nullptr && binding_->node != nullptr,
            "background batch lane has no configured node");
        return *binding_->node;
    }

    [[nodiscard]] Context& context() const noexcept
    {
        IV_ASSERT(binding_ != nullptr && binding_->context != nullptr,
            "background batch lane has no scalar context");
        return *binding_->context;
    }

    template<fixed_string Name>
    [[nodiscard]] decltype(auto) input() const
    requires requires(Context& context) { context.template input<Name>(); }
    {
        return context().template input<Name>();
    }

    template<fixed_string Name>
    [[nodiscard]] decltype(auto) output() const
    requires requires(Context& context) { context.template output<Name>(); }
    {
        return context().template output<Name>();
    }

    [[nodiscard]] decltype(auto) tock_state() const
    requires requires(Context& context) { context.tock_state(); }
    {
        return context().tock_state();
    }
};

template<typename Node, typename Context>
class BackgroundBatchIterator {
    BackgroundBatchLaneBinding<Node, Context> const* current_ = nullptr;

public:
    using value_type = BackgroundBatchLane<Node, Context>;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    BackgroundBatchIterator() = default;
    explicit BackgroundBatchIterator(
        BackgroundBatchLaneBinding<Node, Context> const* current) noexcept
        : current_(current)
    {}

    [[nodiscard]] value_type operator*() const noexcept
    {
        return value_type{*current_};
    }

    BackgroundBatchIterator& operator++() noexcept
    {
        ++current_;
        return *this;
    }

    BackgroundBatchIterator operator++(int) noexcept
    {
        auto result = *this;
        ++*this;
        return result;
    }

    friend bool operator==(
        BackgroundBatchIterator const&,
        BackgroundBatchIterator const&) = default;
};

template<typename Node, typename Context>
class BackgroundBatchContextBase {
    std::span<BackgroundBatchLaneBinding<Node, Context> const> lanes_ {};

protected:
    explicit BackgroundBatchContextBase(
        std::span<BackgroundBatchLaneBinding<Node, Context> const> lanes) noexcept
        : lanes_(lanes)
    {}

public:
    using Lane = BackgroundBatchLane<Node, Context>;
    using iterator = BackgroundBatchIterator<Node, Context>;

    [[nodiscard]] std::size_t size() const noexcept { return lanes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return lanes_.empty(); }

    [[nodiscard]] Lane operator[](std::size_t index) const noexcept
    {
        IV_ASSERT(index < lanes_.size(),
            "background batch lane is out of range");
        return Lane{lanes_[index]};
    }

    [[nodiscard]] iterator begin() const noexcept
    {
        return iterator{lanes_.data()};
    }

    [[nodiscard]] iterator end() const noexcept
    {
        return iterator{lanes_.empty()
            ? lanes_.data()
            : lanes_.data() + lanes_.size()};
    }
};

} // namespace details

template<typename Node>
class TockCoverageBatchContext
    : public details::BackgroundBatchContextBase<
        Node, TockCoverageContext<Node>> {
    using Base = details::BackgroundBatchContextBase<
        Node, TockCoverageContext<Node>>;

public:
    explicit TockCoverageBatchContext(
        std::span<details::BackgroundBatchLaneBinding<
            Node, TockCoverageContext<Node>> const> lanes) noexcept
        : Base(lanes)
    {}
};

template<typename Node>
class PropagateForwardCoverageBatchContext
    : public details::BackgroundBatchContextBase<
        Node, PropagateForwardCoverageContext<Node>> {
    using Base = details::BackgroundBatchContextBase<
        Node, PropagateForwardCoverageContext<Node>>;

public:
    explicit PropagateForwardCoverageBatchContext(
        std::span<details::BackgroundBatchLaneBinding<
            Node, PropagateForwardCoverageContext<Node>> const> lanes) noexcept
        : Base(lanes)
    {}
};

template<typename Node>
class PropagateReverseCoverageBatchContext
    : public details::BackgroundBatchContextBase<
        Node, PropagateReverseCoverageContext<Node>> {
    using Base = details::BackgroundBatchContextBase<
        Node, PropagateReverseCoverageContext<Node>>;

public:
    explicit PropagateReverseCoverageBatchContext(
        std::span<details::BackgroundBatchLaneBinding<
            Node, PropagateReverseCoverageContext<Node>> const> lanes) noexcept
        : Base(lanes)
    {}
};

template<typename Node>
void do_tock_coverage(Node const& node, TockCoverageContext<Node>& context)
{
    if constexpr (details::has_tock_coverage_batch<Node>) {
        details::BackgroundBatchLaneBinding<
            Node, TockCoverageContext<Node>> lane{
                .node = std::addressof(node),
                .context = std::addressof(context),
            };
        TockCoverageBatchContext<Node> batch{{std::addressof(lane), 1}};
        Node::tock_coverage_batch(batch);
    } else {
        node.tock_coverage(context);
    }
}

template<typename Node>
void do_tock_coverage_batch(TockCoverageBatchContext<Node>& context)
{
    if (context.empty()) return;
    if constexpr (details::has_tock_coverage_batch<Node>) {
        Node::tock_coverage_batch(context);
    } else {
        for (auto lane : context) {
            lane.node().tock_coverage(lane.context());
        }
    }
}

template<typename Node>
void do_propagate_forward_coverage(
    Node const& node, PropagateForwardCoverageContext<Node>& context)
{
    if constexpr (details::has_propagate_forward_coverage_batch<Node>) {
        details::BackgroundBatchLaneBinding<
            Node, PropagateForwardCoverageContext<Node>> lane{
                .node = std::addressof(node),
                .context = std::addressof(context),
            };
        PropagateForwardCoverageBatchContext<Node> batch{
            {std::addressof(lane), 1}};
        Node::propagate_forward_coverage_batch(batch);
    } else {
        node.propagate_forward_coverage(context);
    }
}

template<typename Node>
void do_propagate_forward_coverage_batch(
    PropagateForwardCoverageBatchContext<Node>& context)
{
    if (context.empty()) return;
    if constexpr (details::has_propagate_forward_coverage_batch<Node>) {
        Node::propagate_forward_coverage_batch(context);
    } else {
        for (auto lane : context) {
            lane.node().propagate_forward_coverage(lane.context());
        }
    }
}

template<typename Node>
void do_propagate_reverse_coverage(
    Node const& node, PropagateReverseCoverageContext<Node>& context)
{
    if constexpr (details::has_propagate_reverse_coverage_batch<Node>) {
        details::BackgroundBatchLaneBinding<
            Node, PropagateReverseCoverageContext<Node>> lane{
                .node = std::addressof(node),
                .context = std::addressof(context),
            };
        PropagateReverseCoverageBatchContext<Node> batch{
            {std::addressof(lane), 1}};
        Node::propagate_reverse_coverage_batch(batch);
    } else if constexpr (details::has_propagate_reverse_coverage<Node>) {
        node.propagate_reverse_coverage(context);
    } else {
        bool required = false;
        for (auto const& output : context.outputs) {
            required = required || !output.required().empty();
        }
        for (auto const& output : context.event_outputs) {
            required = required || !output.required().empty();
        }
        if (!required) return;
        for (auto const& input : context.inputs) input.require(input.coverage());
        for (auto const& input : context.event_inputs) input.require(input.coverage());
    }
}

template<typename Node>
void do_propagate_reverse_coverage_batch(
    PropagateReverseCoverageBatchContext<Node>& context)
{
    if (context.empty()) return;
    if constexpr (details::has_propagate_reverse_coverage_batch<Node>) {
        Node::propagate_reverse_coverage_batch(context);
    } else {
        for (auto lane : context) {
            do_propagate_reverse_coverage(lane.node(), lane.context());
        }
    }
}

} // namespace iv
