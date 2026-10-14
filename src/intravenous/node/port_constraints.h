#pragma once

#include <intravenous/continuous_value.h>
#include <intravenous/node/static_port_access.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {

enum class PortConstraintDirection : std::uint8_t {
    input,
    output,
};

struct ContinuousPortConstraintRef {
    PortConstraintDirection direction = PortConstraintDirection::input;
    std::size_t port_index = 0;

    constexpr bool operator==(ContinuousPortConstraintRef const&) const = default;
};

struct ContinuousValueTypeEqualityConstraint {
    ContinuousPortConstraintRef first {};
    ContinuousPortConstraintRef second {};

    constexpr bool operator==(
        ContinuousValueTypeEqualityConstraint const&) const = default;
};

struct ContinuousValueTypeAnchorConstraint {
    ContinuousPortConstraintRef port {};
    ContinuousValueTypeId value_type = continuous_value_type_id_v<Sample>;

    constexpr bool operator==(
        ContinuousValueTypeAnchorConstraint const&) const = default;
};

struct NodePortConstraints {
    std::vector<ContinuousValueTypeEqualityConstraint> value_type_equalities {};
    std::vector<ContinuousValueTypeAnchorConstraint> value_type_anchors {};

    constexpr bool operator==(NodePortConstraints const&) const = default;
};

// Module code contributes constraints through this narrow callback sink.  The
// owning graph implementation supplies the storage and callbacks, so authored
// node code does not allocate or depend on graph-builder containers.
struct ReflectedPortConstraintSink {
    void* state = nullptr;
    void (*equal_value_types)(
        void*, ContinuousPortConstraintRef, ContinuousPortConstraintRef) = nullptr;
    void (*anchor_value_type)(
        void*, ContinuousPortConstraintRef, ContinuousValueTypeId) = nullptr;

    void equal(
        ContinuousPortConstraintRef first,
        ContinuousPortConstraintRef second) const
    {
        if (!equal_value_types) {
            throw std::logic_error(
                "port-constraint sink has no equality callback");
        }
        equal_value_types(state, first, second);
    }

    void anchor(
        ContinuousPortConstraintRef port,
        ContinuousValueTypeId value_type) const
    {
        if (!anchor_value_type) {
            throw std::logic_error(
                "port-constraint sink has no anchor callback");
        }
        anchor_value_type(state, port, value_type);
    }
};

class ContinuousValueTypeConstraint {
    ReflectedPortConstraintSink const* sink_ = nullptr;
    ContinuousPortConstraintRef port_ {};

public:
    constexpr ContinuousValueTypeConstraint() = default;
    constexpr ContinuousValueTypeConstraint(
        ContinuousValueTypeConstraint const&) noexcept = default;
    constexpr ContinuousValueTypeConstraint(
        ReflectedPortConstraintSink const& sink,
        ContinuousPortConstraintRef port) noexcept
        : sink_(&sink)
        , port_(port)
    {}

    [[nodiscard]] constexpr ContinuousPortConstraintRef port() const noexcept
    {
        return port_;
    }

    [[nodiscard]] constexpr ReflectedPortConstraintSink const& sink() const
    {
        if (!sink_) throw std::logic_error("unbound value-type constraint");
        return *sink_;
    }

    ContinuousValueTypeConstraint& operator=(
        ContinuousValueTypeConstraint const& other)
    {
        if (&sink() != &other.sink()) {
            throw std::logic_error(
                "value-type constraints belong to different contexts");
        }
        sink().equal(port_, other.port_);
        return *this;
    }

    ContinuousValueTypeConstraint& operator=(ContinuousValueTypeId value_type)
    {
        sink().anchor(port_, value_type);
        return *this;
    }

    template<RegisteredContinuousValue T>
    ContinuousValueTypeConstraint& operator=(std::type_identity<T>)
    {
        return *this = continuous_value_type_id_v<T>;
    }
};

class ContinuousPortConstraint {
    ContinuousValueTypeConstraint value_type_ {};

public:
    constexpr ContinuousPortConstraint() = default;
    constexpr ContinuousPortConstraint(
        ReflectedPortConstraintSink const& sink,
        ContinuousPortConstraintRef port) noexcept
        : value_type_(sink, port)
    {}

    [[nodiscard]] constexpr ContinuousValueTypeConstraint value_type() const
        noexcept
    {
        return value_type_;
    }
};

template<class Node>
class ConstrainPortsContext {
    ReflectedPortConstraintSink sink_ {};

    static void equal_pair(
        ContinuousValueTypeConstraint first,
        ContinuousValueTypeConstraint second)
    {
        if (&first.sink() != &second.sink()) {
            throw std::logic_error(
                "value-type constraints belong to different contexts");
        }
        first.sink().equal(first.port(), second.port());
    }

    static void equal_pair(
        ContinuousValueTypeConstraint port,
        ContinuousValueTypeId value_type)
    {
        port.sink().anchor(port.port(), value_type);
    }

    static void equal_pair(
        ContinuousValueTypeId value_type,
        ContinuousValueTypeConstraint port)
    {
        equal_pair(port, value_type);
    }

    static void equal_pair(
        ContinuousValueTypeId first,
        ContinuousValueTypeId second)
    {
        if (first != second) {
            throw std::logic_error("conflicting value-type constants");
        }
    }

    template<RegisteredContinuousValue T>
    static void equal_pair(
        std::type_identity<T>,
        ContinuousValueTypeId value_type)
    {
        equal_pair(continuous_value_type_id_v<T>, value_type);
    }

    template<RegisteredContinuousValue T>
    static void equal_pair(
        ContinuousValueTypeId value_type,
        std::type_identity<T>)
    {
        equal_pair(value_type, continuous_value_type_id_v<T>);
    }

    template<RegisteredContinuousValue First, RegisteredContinuousValue Second>
    static void equal_pair(
        std::type_identity<First>,
        std::type_identity<Second>)
    {
        equal_pair(
            continuous_value_type_id_v<First>,
            continuous_value_type_id_v<Second>);
    }

    template<RegisteredContinuousValue T>
    static void equal_pair(
        ContinuousValueTypeConstraint port,
        std::type_identity<T>)
    {
        equal_pair(port, continuous_value_type_id_v<T>);
    }

    template<RegisteredContinuousValue T>
    static void equal_pair(
        std::type_identity<T>,
        ContinuousValueTypeConstraint port)
    {
        equal_pair(port, continuous_value_type_id_v<T>);
    }

public:
    constexpr explicit ConstrainPortsContext(
        ReflectedPortConstraintSink sink) noexcept
        : sink_(sink)
    {}

    template<fixed_string Name>
    [[nodiscard]] constexpr ContinuousPortConstraint input() const
    {
        constexpr auto declaration =
            details::static_input_declaration_index<Node, Name>();
        static_assert(
            details::static_input_port_kind_at<Node, declaration>()
                == PortKind::sample,
            "port constraints for continuous values require a continuous input");
        return ContinuousPortConstraint(
            sink_,
            {
                .direction = PortConstraintDirection::input,
                .port_index = details::static_input_port_index<Node, Name>(),
            });
    }

    template<fixed_string Name>
    [[nodiscard]] constexpr ContinuousPortConstraint output() const
    {
        constexpr auto declaration =
            details::static_output_declaration_index<Node, Name>();
        static_assert(
            details::static_output_port_kind_at<Node, declaration>()
                == PortKind::sample,
            "port constraints for continuous values require a continuous output");
        return ContinuousPortConstraint(
            sink_,
            {
                .direction = PortConstraintDirection::output,
                .port_index = details::static_output_port_index<Node, Name>(),
            });
    }

    template<class First, class... Rest>
    void equal(First first, Rest... rest) const
    {
        static_assert(sizeof...(Rest) != 0,
            "equal() requires at least two constraint operands");
        (equal_pair(first, rest), ...);
    }
};

template<RegisteredContinuousValue T>
inline constexpr std::type_identity<T> continuous_value_type {};

} // namespace iv
