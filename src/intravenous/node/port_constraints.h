#pragma once

#include <intravenous/continuous_value.h>
#include <intravenous/node/static_port_access.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {

enum class PortConstraintDirection : std::uint8_t { input, output };
enum class CoverageOrdering : std::uint8_t { unspecified, sorted };

// Power-of-two transported-value factor relative to one logical node step.
// Authored construction names the factor itself; graph solving uses the named
// exponent factory so negative resolved domains cannot be confused with factors.
class PortPace {
public:
    using exponent_type = std::int32_t;

private:
    struct ExponentTag {};
    exponent_type exponent_ = 0;

    constexpr PortPace(exponent_type exponent, ExponentTag) noexcept
        : exponent_(exponent)
    {}

public:
    constexpr PortPace() noexcept = default;
    constexpr PortPace(std::uint64_t factor)
    {
        if (!std::has_single_bit(factor))
            throw std::invalid_argument("port pace factor must be a nonzero power of two");
        exponent_ = static_cast<exponent_type>(std::countr_zero(factor));
    }

    [[nodiscard]] static constexpr PortPace from_exponent(
        exponent_type exponent) noexcept
    {
        return PortPace(exponent, ExponentTag{});
    }

    [[nodiscard]] constexpr exponent_type exponent() const noexcept
    {
        return exponent_;
    }

    constexpr bool operator==(PortPace const&) const = default;
};

struct PortConstraintRef {
    PortConstraintDirection direction = PortConstraintDirection::input;
    PortKind kind = PortKind::sample;
    std::size_t port_index = 0;
    constexpr bool operator==(PortConstraintRef const&) const = default;
};

struct ContinuousValueTypeProperty { using value_type = ContinuousValueTypeId; };
struct ContinuousValueSizeProperty { using value_type = std::size_t; };
struct PortPaceProperty { using value_type = PortPace; };
struct PortHistoryProperty { using value_type = std::size_t; };
struct PortLatencyProperty { using value_type = std::size_t; };
struct CoverageOrderingProperty { using value_type = CoverageOrdering; };

template<class Property>
struct PortPropertyEqualityConstraint {
    PortConstraintRef first {};
    PortConstraintRef second {};
    constexpr bool operator==(PortPropertyEqualityConstraint const&) const = default;
};

template<class Property>
struct PortPropertyAnchorConstraint {
    PortConstraintRef port {};
    typename Property::value_type value {};
    constexpr bool operator==(PortPropertyAnchorConstraint const&) const = default;
};

using ContinuousValueTypeEqualityConstraint = PortPropertyEqualityConstraint<ContinuousValueTypeProperty>;
using ContinuousValueTypeAnchorConstraint = PortPropertyAnchorConstraint<ContinuousValueTypeProperty>;
using ContinuousValueSizeEqualityConstraint = PortPropertyEqualityConstraint<ContinuousValueSizeProperty>;
using ContinuousValueSizeAnchorConstraint = PortPropertyAnchorConstraint<ContinuousValueSizeProperty>;
using PortPaceEqualityConstraint = PortPropertyEqualityConstraint<PortPaceProperty>;
using PortPaceAnchorConstraint = PortPropertyAnchorConstraint<PortPaceProperty>;
using AbsolutePortPaceAnchorConstraint =
    PortPropertyAnchorConstraint<PortPaceProperty>;
using PortHistoryEqualityConstraint = PortPropertyEqualityConstraint<PortHistoryProperty>;
using PortHistoryAnchorConstraint = PortPropertyAnchorConstraint<PortHistoryProperty>;
using PortLatencyEqualityConstraint = PortPropertyEqualityConstraint<PortLatencyProperty>;
using PortLatencyAnchorConstraint = PortPropertyAnchorConstraint<PortLatencyProperty>;
using CoverageOrderingEqualityConstraint = PortPropertyEqualityConstraint<CoverageOrderingProperty>;
using CoverageOrderingAnchorConstraint = PortPropertyAnchorConstraint<CoverageOrderingProperty>;

struct NodePortConstraints {
    std::vector<ContinuousValueTypeEqualityConstraint> value_type_equalities {};
    std::vector<ContinuousValueTypeAnchorConstraint> value_type_anchors {};
    std::vector<ContinuousValueSizeEqualityConstraint> size_equalities {};
    std::vector<ContinuousValueSizeAnchorConstraint> size_anchors {};
    std::vector<PortPaceEqualityConstraint> pace_equalities {};
    std::vector<PortPaceAnchorConstraint> pace_anchors {};
    std::vector<AbsolutePortPaceAnchorConstraint> absolute_pace_anchors {};
    std::vector<PortHistoryEqualityConstraint> history_equalities {};
    std::vector<PortHistoryAnchorConstraint> history_anchors {};
    std::vector<PortLatencyEqualityConstraint> latency_equalities {};
    std::vector<PortLatencyAnchorConstraint> latency_anchors {};
    std::vector<CoverageOrderingEqualityConstraint> ordering_equalities {};
    std::vector<CoverageOrderingAnchorConstraint> ordering_anchors {};
    constexpr bool operator==(NodePortConstraints const&) const = default;
};

struct ReflectedPortConstraintSink {
    void* state = nullptr;
#define IV_PORT_CONSTRAINT_CALLBACKS(name, type)                         \
    void (*equal_##name)(void*, PortConstraintRef, PortConstraintRef) = nullptr; \
    void (*anchor_##name)(void*, PortConstraintRef, type) = nullptr
    IV_PORT_CONSTRAINT_CALLBACKS(value_types, ContinuousValueTypeId);
    IV_PORT_CONSTRAINT_CALLBACKS(sizes, std::size_t);
    IV_PORT_CONSTRAINT_CALLBACKS(paces, PortPace);
    IV_PORT_CONSTRAINT_CALLBACKS(histories, std::size_t);
    IV_PORT_CONSTRAINT_CALLBACKS(latencies, std::size_t);
    IV_PORT_CONSTRAINT_CALLBACKS(orderings, CoverageOrdering);
#undef IV_PORT_CONSTRAINT_CALLBACKS
    void (*anchor_absolute_pace)(
        void*, PortConstraintRef, PortPace) = nullptr;

    template<class Property>
    void equal(PortConstraintRef first, PortConstraintRef second) const
    {
        auto callback = [&] {
            if constexpr (std::same_as<Property, ContinuousValueTypeProperty>) return equal_value_types;
            else if constexpr (std::same_as<Property, ContinuousValueSizeProperty>) return equal_sizes;
            else if constexpr (std::same_as<Property, PortPaceProperty>) return equal_paces;
            else if constexpr (std::same_as<Property, PortHistoryProperty>) return equal_histories;
            else if constexpr (std::same_as<Property, PortLatencyProperty>) return equal_latencies;
            else { static_assert(std::same_as<Property, CoverageOrderingProperty>); return equal_orderings; }
        }();
        if (!callback) throw std::logic_error("port-constraint sink has no equality callback");
        callback(state, first, second);
    }

    template<class Property>
    void anchor(PortConstraintRef port, typename Property::value_type value) const
    {
        auto callback = [&] {
            if constexpr (std::same_as<Property, ContinuousValueTypeProperty>) return anchor_value_types;
            else if constexpr (std::same_as<Property, ContinuousValueSizeProperty>) return anchor_sizes;
            else if constexpr (std::same_as<Property, PortPaceProperty>) return anchor_paces;
            else if constexpr (std::same_as<Property, PortHistoryProperty>) return anchor_histories;
            else if constexpr (std::same_as<Property, PortLatencyProperty>) return anchor_latencies;
            else { static_assert(std::same_as<Property, CoverageOrderingProperty>); return anchor_orderings; }
        }();
        if (!callback) throw std::logic_error("port-constraint sink has no anchor callback");
        callback(state, port, value);
    }
};

class AbsolutePortPaceConstraint {
    ReflectedPortConstraintSink const* sink_ = nullptr;
    PortConstraintRef port_ {};

public:
    constexpr AbsolutePortPaceConstraint() = default;
    constexpr AbsolutePortPaceConstraint(
        ReflectedPortConstraintSink const& sink,
        PortConstraintRef port) noexcept
        : sink_(&sink), port_(port)
    {}

    AbsolutePortPaceConstraint& operator=(PortPace value)
    {
        if (!sink_ || !sink_->anchor_absolute_pace)
            throw std::logic_error(
                "port-constraint sink has no absolute-pace callback");
        sink_->anchor_absolute_pace(sink_->state, port_, value);
        return *this;
    }
};

template<class Property>
class PortPropertyConstraint {
    ReflectedPortConstraintSink const* sink_ = nullptr;
    PortConstraintRef port_ {};
public:
    using value_type = typename Property::value_type;
    constexpr PortPropertyConstraint() = default;
    constexpr PortPropertyConstraint(PortPropertyConstraint const&) noexcept = default;
    constexpr PortPropertyConstraint(ReflectedPortConstraintSink const& sink, PortConstraintRef port) noexcept
        : sink_(&sink), port_(port) {}
    [[nodiscard]] constexpr PortConstraintRef port() const noexcept { return port_; }
    [[nodiscard]] constexpr ReflectedPortConstraintSink const& sink() const
    {
        if (!sink_) throw std::logic_error("unbound port constraint");
        return *sink_;
    }
    PortPropertyConstraint& operator=(PortPropertyConstraint const& other)
    {
        if (&sink() != &other.sink()) throw std::logic_error("port constraints belong to different contexts");
        sink().template equal<Property>(port_, other.port_);
        return *this;
    }
    PortPropertyConstraint& operator=(value_type value)
    {
        sink().template anchor<Property>(port_, value);
        return *this;
    }
    template<RegisteredContinuousValue T>
        requires std::same_as<Property, ContinuousValueTypeProperty>
    PortPropertyConstraint& operator=(std::type_identity<T>)
    {
        return *this = continuous_value_type_id_v<T>;
    }

    [[nodiscard]] constexpr AbsolutePortPaceConstraint absolute() const
        requires std::same_as<Property, PortPaceProperty>
    {
        return {sink(), port_};
    }
};

using ContinuousValueTypeConstraint = PortPropertyConstraint<ContinuousValueTypeProperty>;
using ContinuousValueSizeConstraint = PortPropertyConstraint<ContinuousValueSizeProperty>;
using PortPaceConstraint = PortPropertyConstraint<PortPaceProperty>;
using PortHistoryConstraint = PortPropertyConstraint<PortHistoryProperty>;
using PortLatencyConstraint = PortPropertyConstraint<PortLatencyProperty>;
using CoverageOrderingConstraint = PortPropertyConstraint<CoverageOrderingProperty>;

template<bool Continuous, bool Dimensioned, bool HasHistory, bool HasLatency, bool HasOrdering>
class PortConstraint {
    ReflectedPortConstraintSink const* sink_ = nullptr;
    PortConstraintRef port_ {};
public:
    constexpr PortConstraint() = default;
    constexpr PortConstraint(ReflectedPortConstraintSink const& sink, PortConstraintRef port) noexcept
        : sink_(&sink), port_(port) {}
    [[nodiscard]] constexpr ContinuousValueTypeConstraint value_type() const noexcept requires Continuous { return {*sink_, port_}; }
    [[nodiscard]] constexpr ContinuousValueSizeConstraint size() const noexcept requires (Continuous && Dimensioned) { return {*sink_, port_}; }
    [[nodiscard]] constexpr PortPaceConstraint pace() const noexcept requires Continuous { return {*sink_, port_}; }
    [[nodiscard]] constexpr PortHistoryConstraint history() const noexcept requires HasHistory { return {*sink_, port_}; }
    [[nodiscard]] constexpr PortLatencyConstraint latency() const noexcept requires HasLatency { return {*sink_, port_}; }
    [[nodiscard]] constexpr CoverageOrderingConstraint ordering() const noexcept requires HasOrdering { return {*sink_, port_}; }
};

[[nodiscard]] constexpr bool domain_has_dynamic_dimension(ContinuousValueTypeSet domain) noexcept
{
    for (auto const& type : continuous_value_type_registry)
        if (type.rank != 0 && domain.contains(type.id)) return true;
    return false;
}

template<class Node>
class ConstrainPortsContext {
    ReflectedPortConstraintSink sink_ {};

    template<class Property>
    static void equal_pair(PortPropertyConstraint<Property> first, PortPropertyConstraint<Property> second)
    {
        if (&first.sink() != &second.sink()) throw std::logic_error("port constraints belong to different contexts");
        first.sink().template equal<Property>(first.port(), second.port());
    }
    template<class Property>
    static void equal_pair(PortPropertyConstraint<Property> port, typename Property::value_type value)
    {
        port.sink().template anchor<Property>(port.port(), value);
    }
    template<class Property>
    static void equal_pair(typename Property::value_type value, PortPropertyConstraint<Property> port) { equal_pair(port, value); }
    template<class Property>
    static void equal_pair(typename Property::value_type first, typename Property::value_type second)
    {
        if (first != second) throw std::logic_error("conflicting port-constraint constants");
    }
    template<RegisteredContinuousValue T>
    static void equal_pair(ContinuousValueTypeConstraint port, std::type_identity<T>) { equal_pair(port, continuous_value_type_id_v<T>); }
    template<RegisteredContinuousValue T>
    static void equal_pair(std::type_identity<T>, ContinuousValueTypeConstraint port) { equal_pair(port, continuous_value_type_id_v<T>); }
    template<RegisteredContinuousValue First, RegisteredContinuousValue Second>
    static void equal_pair(std::type_identity<First>, std::type_identity<Second>)
    {
        equal_pair<ContinuousValueTypeProperty>(continuous_value_type_id_v<First>, continuous_value_type_id_v<Second>);
    }

public:
    constexpr explicit ConstrainPortsContext(ReflectedPortConstraintSink sink) noexcept : sink_(sink) {}

    template<fixed_string Name>
    [[nodiscard]] constexpr auto input() const
    {
        constexpr auto declaration = details::static_input_declaration_index<Node, Name>();
        constexpr auto kind = details::static_input_port_kind_at<Node, declaration>();
        constexpr bool continuous = kind == PortKind::sample;
        constexpr auto access =
            details::static_input_port_access_at<Node, declaration>();
        constexpr bool sequential = access == InputAccessKind::sequential;
        constexpr bool coverage = access == InputAccessKind::coverage;
        constexpr bool dimensioned = [&] {
            if constexpr (!continuous) return false;
            else return domain_has_dynamic_dimension(
                details::static_input_port_value_types<Node, Name>());
        }();
        constexpr auto port = [&] {
            if constexpr (continuous) return details::static_input_port_index<Node, Name>();
            else return details::static_event_input_port_index<Node, Name>();
        }();
        return PortConstraint<continuous, dimensioned, sequential, false, coverage>(
            sink_, {PortConstraintDirection::input, kind, port});
    }

    template<fixed_string Name>
    [[nodiscard]] constexpr auto output() const
    {
        constexpr auto declaration = details::static_output_declaration_index<Node, Name>();
        constexpr auto kind = details::static_output_port_kind_at<Node, declaration>();
        constexpr bool continuous = kind == PortKind::sample;
        constexpr auto production =
            details::static_output_port_production_at<Node, declaration>();
        constexpr bool tick = production == OutputProductionKind::tick;
        constexpr bool coverage =
            production == OutputProductionKind::coverage;
        constexpr bool dimensioned = [&] {
            if constexpr (!continuous) return false;
            else return domain_has_dynamic_dimension(
                details::static_output_port_value_types<Node, Name>());
        }();
        constexpr auto port = [&] {
            if constexpr (continuous) return details::static_output_port_index<Node, Name>();
            else return details::static_event_output_port_index<Node, Name>();
        }();
        return PortConstraint<continuous, dimensioned, tick, tick, coverage>(
            sink_, {PortConstraintDirection::output, kind, port});
    }

    template<class First, class... Rest>
    void equal(First first, Rest... rest) const
    {
        static_assert(sizeof...(Rest) != 0, "equal() requires at least two constraint operands");
        (equal_pair(first, rest), ...);
    }
};

template<RegisteredContinuousValue T>
inline constexpr std::type_identity<T> continuous_value_type {};

} // namespace iv
