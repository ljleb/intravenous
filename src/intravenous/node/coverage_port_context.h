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

struct RandomAccessSampleInputPort {
    void const* data = nullptr;
    IndexCoverage const* coverage_value = nullptr;
    Sample (*read_sample)(void const*, GlobalIndex, std::size_t) = nullptr;

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return background_port_details::bound_coverage(coverage_value);
    }

    [[nodiscard]] Sample at(
        GlobalIndex index, std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(coverage().contains(index),
            "background sample input read lies outside published coverage");
        IV_ASSERT(read_sample != nullptr,
            "background sample input has no readable storage");
        return read_sample(data, index, channel);
    }
};

struct TockSampleOutputPort {
    void* data = nullptr;
    IndexCoverage const* requested_coverage_value = nullptr;
    void (*write_sample)(void*, GlobalIndex, std::size_t, Sample) = nullptr;

    [[nodiscard]] IndexCoverage const& requested_coverage() const noexcept
    {
        return background_port_details::bound_coverage(requested_coverage_value);
    }

    void write(
        GlobalIndex index, std::size_t channel, Sample value) const noexcept
    {
        IV_ASSERT(requested_coverage().contains(index),
            "background sample output write lies outside requested coverage");
        IV_ASSERT(write_sample != nullptr,
            "background sample output has no writable storage");
        write_sample(data, index, channel, value);
    }
};

// Event inputs may be physically split across any number of retained chunks. The
// provider invokes the visitor synchronously in global timestamp order without
// requiring a contiguous query-sized buffer.
struct RandomAccessEventInputPort {
    using VisitEvent = void (*)(void*, TimedEvent const&);
    using ForEachEvent = void (*)(
        void const*, GlobalIndexRange, void*, VisitEvent);

    void const* data = nullptr;
    IndexCoverage const* coverage_value = nullptr;
    ForEachEvent for_each_event = nullptr;

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return background_port_details::bound_coverage(coverage_value);
    }

    template<typename Fn>
    void for_each(GlobalIndexRange region, Fn&& fn) const
    {
        IV_ASSERT(coverage().contains(region),
            "background event input read lies outside published coverage");
        if (region.empty()) return;
        IV_ASSERT(for_each_event != nullptr,
            "background event input has no readable segmented storage");
        using Visitor = std::remove_reference_t<Fn>;
        for_each_event(
            data,
            region,
            std::addressof(fn),
            +[](void* opaque, TimedEvent const& event) {
                (*static_cast<Visitor*>(opaque))(event);
            });
    }

};

struct TockEventOutputPort {
    void* data = nullptr;
    IndexCoverage const* requested_coverage_value = nullptr;
    void (*write_event)(void*, TimedEvent const&) = nullptr;

    [[nodiscard]] IndexCoverage const& requested_coverage() const noexcept
    {
        return background_port_details::bound_coverage(requested_coverage_value);
    }

    void write(TimedEvent const& event) const noexcept
    {
        IV_ASSERT(requested_coverage().contains(event.time),
            "background event output write lies outside requested coverage");
        IV_ASSERT(write_event != nullptr,
            "background event output has no writable storage");
        // The callback contract requires nondecreasing timestamps for each
        // output invocation. The concrete sink may validate that in debug
        // builds without imposing a release-time sorting pass here.
        write_event(data, event);
    }
};

struct InputCoverageChange {
    IndexCoverage const* coverage_value = nullptr;
    IndexCoverage const* changed_value = nullptr;

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
    using PublishCoverage = void (*)(void*, IndexCoverage const&);
    using PublishChange = void (*)(void*, IndexCoverage const&);

    void* data = nullptr;
    IndexCoverage const* previous_coverage_value = nullptr;
    PublishCoverage publish_coverage_value = nullptr;
    PublishChange publish_changed_value = nullptr;

    [[nodiscard]] IndexCoverage const& previous_coverage() const noexcept
    {
        return background_port_details::bound_coverage(previous_coverage_value);
    }

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

template<ChannelTypeId Type>
class StaticBackgroundSampleInputAccess {
    RandomAccessSampleInputPort const& port_;

public:
    explicit StaticBackgroundSampleInputAccess(RandomAccessSampleInputPort const& port)
        : port_(port)
    {}

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return port_.coverage();
    }

    [[nodiscard]] Sample at(GlobalIndex index) const noexcept
    requires (Type == ChannelTypeId::mono)
    {
        return port_.at(index);
    }

    template<class Channel>
    [[nodiscard]] Sample at(Channel, GlobalIndex index) const noexcept
    requires (Type != ChannelTypeId::mono)
    {
        return port_.at(index, static_channel_index<Type, Channel>());
    }
};

template<ChannelTypeId Type>
class StaticBackgroundSampleOutputAccess {
    TockSampleOutputPort& port_;

public:
    explicit StaticBackgroundSampleOutputAccess(TockSampleOutputPort& port)
        : port_(port)
    {}

    [[nodiscard]] IndexCoverage const& requested_coverage() const noexcept
    {
        return port_.requested_coverage();
    }

    void write(GlobalIndex index, Sample value) const noexcept
    requires (Type == ChannelTypeId::mono)
    {
        port_.write(index, 0, value);
    }

    template<class Channel>
    void write(Channel, GlobalIndex index, Sample value) const noexcept
    requires (Type != ChannelTypeId::mono)
    {
        port_.write(index, static_channel_index<Type, Channel>(), value);
    }
};

class StaticBackgroundEventInputAccess {
    RandomAccessEventInputPort const& port_;

public:
    explicit StaticBackgroundEventInputAccess(RandomAccessEventInputPort const& port)
        : port_(port)
    {}

    [[nodiscard]] IndexCoverage const& coverage() const noexcept
    {
        return port_.coverage();
    }

    template<typename Fn>
    void for_each(GlobalIndexRange region, Fn&& fn) const
    {
        port_.for_each(region, std::forward<Fn>(fn));
    }

};

class StaticBackgroundEventOutputAccess {
    TockEventOutputPort& port_;

public:
    explicit StaticBackgroundEventOutputAccess(TockEventOutputPort& port)
        : port_(port)
    {}

    [[nodiscard]] IndexCoverage const& requested_coverage() const noexcept
    {
        return port_.requested_coverage();
    }

    void write(TimedEvent const& event) const noexcept { port_.write(event); }
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

template<typename Node>
struct TockCoverageContext {
    std::span<RandomAccessSampleInputPort const> inputs {};
    std::span<TockSampleOutputPort> outputs {};
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
            constexpr auto layout = details::static_input_port_layout<Node, Name>();
            constexpr auto port_index =
                details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(port_index < inputs.size(),
                "background sample input is absent from tock context");
            return details::StaticBackgroundSampleInputAccess<layout.channel_type>(
                inputs[port_index]);
        } else {
            constexpr auto port_index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(port_index < event_inputs.size(),
                "background event input is absent from tock context");
            return details::StaticBackgroundEventInputAccess(event_inputs[port_index]);
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
            constexpr auto layout = details::static_output_port_layout<Node, Name>();
            constexpr auto port_index =
                details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(port_index < outputs.size(),
                "background sample output is absent from tock context");
            return details::StaticBackgroundSampleOutputAccess<layout.channel_type>(
                outputs[port_index]);
        } else {
            constexpr auto port_index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(port_index < event_outputs.size(),
                "background event output is absent from tock context");
            return details::StaticBackgroundEventOutputAccess(event_outputs[port_index]);
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
    [[nodiscard]] InputCoverageChange const& input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(details::static_input_port_is_random_access<Node, Name>(),
            "forward coverage can only inspect background inputs");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(index < inputs.size(),
                "background sample input is absent from forward context");
            return inputs[index];
        } else {
            constexpr auto index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(index < event_inputs.size(),
                "background event input is absent from forward context");
            return event_inputs[index];
        }
    }

    template<fixed_string Name>
    [[nodiscard]] OutputCoverageChange const& output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "forward coverage can only publish background outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(index < outputs.size(),
                "background sample output is absent from forward context");
            return outputs[index];
        } else {
            constexpr auto index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(index < event_outputs.size(),
                "background event output is absent from forward context");
            return event_outputs[index];
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
