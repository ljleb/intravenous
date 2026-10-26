#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/node/continuous_port.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace iv::details {
    template<size_t Limit, PortConfigTuple Tuple, class Predicate>
    consteval size_t count_static_configs_before(
        Tuple const& configs, Predicate predicate)
    {
        size_t position = 0;
        size_t result = 0;
        for_each_port_config(configs, [&](auto const& config) {
            if (position < Limit && predicate(config)) ++result;
            ++position;
        });
        return result;
    }

    // Port configs own std::strings. Long names allocate during constant
    // evaluation, so a config array cannot be retained in static constexpr
    // storage or exposed by reference. Keep each array local to its consteval
    // query and return only allocation-free facts derived from it.
    template<typename Node, fixed_string Name>
    consteval size_t static_input_declaration_index()
    {
        auto const configs = Node::inputs();
        size_t found = static_cast<size_t>(-1);
        size_t index = 0;
        for_each_port_config(configs, [&](auto const& config) {
            if (config.name == Name.view()) {
                if (found != static_cast<size_t>(-1)) {
                    throw "duplicate static input port name";
                }
                found = index;
            }
            ++index;
        });
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static input port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_output_declaration_index()
    {
        auto const configs = Node::outputs();
        size_t found = static_cast<size_t>(-1);
        size_t index = 0;
        for_each_port_config(configs, [&](auto const& config) {
            if (config.name == Name.view()) {
                if (found != static_cast<size_t>(-1)) {
                    throw "duplicate static output port name";
                }
                found = index;
            }
            ++index;
        });
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static output port name";
        }
        return found;
    }

    template<typename Node, size_t Index>
    consteval PortKind static_input_port_kind_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::inputs())>,
            "static input port index is out of bounds");
        auto const configs = Node::inputs();
        return is_continuous(port_config_at<Index>(configs))
            ? PortKind::sample : PortKind::event;
    }

    template<typename Node, size_t Index>
    consteval PortKind static_output_port_kind_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        auto const configs = Node::outputs();
        return is_continuous(port_config_at<Index>(configs))
            ? PortKind::sample : PortKind::event;
    }

    template<typename Node, size_t Index>
    consteval size_t static_sample_input_port_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::inputs())>,
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::sample,
            "static input port is not a sample port");
        auto const configs = Node::inputs();
        return count_static_configs_before<Index>(
            configs, [](auto const& config) { return is_continuous(config); });
    }

    template<typename Node, size_t Index>
    consteval size_t static_event_input_port_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::inputs())>,
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::event,
            "static input port is not an event port");
        auto const configs = Node::inputs();
        return count_static_configs_before<Index>(
            configs, [](auto const& config) { return !is_continuous(config); });
    }

    template<typename Node, size_t Index>
    consteval size_t static_realtime_sample_output_port_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::sample,
            "static output port is not a sample port");
        static_assert(!is_tock(port_config_at<Index>(Node::outputs())),
            "static output port is not produced by Tick execution");
        auto const configs = Node::outputs();
        return count_static_configs_before<Index>(configs, [](auto const& config) {
            return is_continuous(config) && is_tick(config);
        });
    }

    template<typename Node, size_t Index>
    consteval size_t static_sample_output_port_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::sample,
            "static output port is not a continuous port");
        auto const configs = Node::outputs();
        return count_static_configs_before<Index>(
            configs, [](auto const& config) { return is_continuous(config); });
    }

    template<typename Node, size_t Index>
    consteval size_t static_realtime_event_output_port_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::event,
            "static output port is not an event port");
        static_assert(!is_tock(port_config_at<Index>(Node::outputs())),
            "static output port is not produced by Tick execution");
        auto const configs = Node::outputs();
        return count_static_configs_before<Index>(configs, [](auto const& config) {
            return !is_continuous(config) && is_tick(config);
        });
    }

    template<typename Node, size_t Index>
    consteval ChannelLayout static_input_port_layout_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::inputs())>,
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::sample,
            "static input port is not a sample port");
        auto const configs = Node::inputs();
        return effective_channel_layout(port_config_at<Index>(configs));
    }

    template<typename Node, size_t Index>
    consteval ChannelLayout static_declared_output_port_layout_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::sample,
            "static output port is not a sample port");
        auto const configs = Node::outputs();
        return effective_channel_layout(port_config_at<Index>(configs));
    }

    template<typename Node, size_t Index>
    consteval InputAccessKind static_input_port_access_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::inputs())>,
            "static input port index is out of bounds");
        auto const configs = Node::inputs();
        using Config = std::remove_cvref_t<
            decltype(port_config_at<Index>(configs))>;
        return Config::access;
    }

    template<typename Node, size_t Index>
    consteval OutputProductionKind static_output_port_production_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        auto const configs = Node::outputs();
        using Config = std::remove_cvref_t<
            decltype(port_config_at<Index>(configs))>;
        return Config::production;
    }

    template<typename Node, size_t Index>
    consteval bool static_input_port_is_random_access_at()
    {
        return static_input_port_access_at<Node, Index>()
            != InputAccessKind::sequential;
    }

    template<typename Node, size_t Index>
    consteval bool static_output_port_is_tock_at()
    {
        return static_output_port_production_at<Node, Index>()
            != OutputProductionKind::tick;
    }

    template<typename Node, size_t Index>
    consteval bool static_output_port_is_recording_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        auto const configs = Node::outputs();
        auto const& config = port_config_at<Index>(configs);
        return is_tick(config) && is_persisted(config);
    }

    template<typename Node>
    consteval size_t static_recording_sample_output_count()
    {
        if constexpr (requires { Node::outputs(); }) {
            return count_port_configs(Node::outputs(), [](auto const& config) {
                return is_continuous(config) && is_tick(config)
                    && is_persisted(config);
            });
        } else {
            return 0;
        }
    }

    template<typename Node>
    consteval size_t static_recording_event_output_count()
    {
        if constexpr (requires { Node::outputs(); }) {
            return count_port_configs(Node::outputs(), [](auto const& config) {
                return !is_continuous(config) && is_tick(config)
                    && is_persisted(config);
            });
        } else {
            return 0;
        }
    }

    template<typename Node>
    inline constexpr size_t static_recording_sample_output_count_v =
        static_recording_sample_output_count<Node>();

    template<typename Node>
    inline constexpr size_t static_recording_event_output_count_v =
        static_recording_event_output_count<Node>();

    template<typename Node, size_t Index>
    consteval size_t static_recording_sample_output_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::sample
                && static_output_port_is_recording_at<Node, Index>(),
            "static output port is not a Tick/persisted sample port");
        auto const configs = Node::outputs();
        return count_static_configs_before<Index>(configs, [](auto const& config) {
            return is_continuous(config) && is_tick(config) && is_persisted(config);
        });
    }

    template<typename Node, size_t Index>
    consteval size_t static_recording_event_output_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::outputs())>,
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::event
                && static_output_port_is_recording_at<Node, Index>(),
            "static output port is not a Tick/persisted event port");
        auto const configs = Node::outputs();
        return count_static_configs_before<Index>(configs, [](auto const& config) {
            return !is_continuous(config) && is_tick(config) && is_persisted(config);
        });
    }

    template<typename Node, size_t Index>
    consteval size_t static_random_access_input_port_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::inputs())>,
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::sample
                && static_input_port_is_random_access_at<Node, Index>(),
            "static input port is not a Random Access sample port");
        auto const configs = Node::inputs();
        return count_static_configs_before<Index>(configs, [](auto const& config) {
            return is_continuous(config) && is_random_access(config);
        });
    }

    template<typename Node, size_t Index>
    consteval size_t static_random_access_event_input_port_index_at()
    {
        static_assert(Index < port_config_count_v<decltype(Node::inputs())>,
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::event
                && static_input_port_is_random_access_at<Node, Index>(),
            "static input port is not a Random Access event port");
        auto const configs = Node::inputs();
        return count_static_configs_before<Index>(configs, [](auto const& config) {
            return !is_continuous(config) && is_random_access(config);
        });
    }

    template<typename Node, fixed_string Name>
    consteval PortKind static_input_port_kind()
    {
        return static_input_port_kind_at<
            Node, static_input_declaration_index<Node, Name>()>();
    }

    template<typename Node, fixed_string Name>
    consteval PortKind static_output_port_kind()
    {
        return static_output_port_kind_at<
            Node, static_output_declaration_index<Node, Name>()>();
    }

    template<typename Node>
    consteval size_t static_output_count()
    {
        return count_continuous_ports(Node::outputs());
    }

    template<typename Node>
    inline constexpr size_t static_output_count_v = static_output_count<Node>();

    template<typename Node, fixed_string Name>
    consteval size_t static_input_port_index()
    {
        auto const configs = Node::inputs();
        size_t found = static_cast<size_t>(-1);
        size_t sample_index = 0;
        for_each_port_config(configs, [&](auto const& config) {
            if (is_continuous(config)) {
                if (config.name == Name.view()) {
                    if (found != static_cast<size_t>(-1)) {
                        throw "duplicate static input port name";
                    }
                    found = sample_index;
                }
                ++sample_index;
            }
        });
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static sample input port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval ContinuousValueTypeSet static_input_port_value_types()
    {
        auto const configs = Node::inputs();
        std::optional<ContinuousValueTypeSet> result;
        for_each_port_config(configs, [&](auto const& config) {
            if constexpr (AuthoredContinuousInputConfig<decltype(config)>) {
                if (config.name == Name.view()) {
                    result = authored_continuous_value_types_v<decltype(config)>;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "unknown static sample input port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_output_port_index()
    {
        auto const configs = Node::outputs();
        size_t found = static_cast<size_t>(-1);
        size_t sample_index = 0;
        for_each_port_config(configs, [&](auto const& config) {
            if (is_continuous(config)) {
                if (config.name == Name.view()) {
                    if (found != static_cast<size_t>(-1)) {
                        throw "duplicate static output port name";
                    }
                    found = sample_index;
                }
                ++sample_index;
            }
        });
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static sample output port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval ContinuousValueTypeSet static_output_port_value_types()
    {
        auto const configs = Node::outputs();
        std::optional<ContinuousValueTypeSet> result;
        for_each_port_config(configs, [&](auto const& config) {
            if constexpr (AuthoredContinuousOutputConfig<decltype(config)>) {
                if (config.name == Name.view()) {
                    result = authored_continuous_value_types_v<decltype(config)>;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "unknown static sample output port name";
    }

    template<typename Node, fixed_string Name>
    consteval ChannelLayout static_input_port_layout()
    {
        constexpr auto index = static_input_declaration_index<Node, Name>();
        auto const configs = Node::inputs();
        return effective_channel_layout(port_config_at<index>(configs));
    }

    template<typename Node, fixed_string Name>
    consteval bool static_input_port_is_random_access()
    {
        return static_input_port_is_random_access_at<
            Node, static_input_declaration_index<Node, Name>()>();
    }

    template<typename Node, fixed_string Name>
    consteval InputAccessKind static_input_port_access()
    {
        return static_input_port_access_at<
            Node, static_input_declaration_index<Node, Name>()>();
    }

    // Background callback contexts contain only background ports. Convert a declaration's
    // storage sample-port index to its compact background-port index so
    // access callbacks never receive fake realtime placeholders.
    template<typename Node, fixed_string Name>
    consteval size_t static_random_access_input_port_index()
    {
        auto const configs = Node::inputs();
        constexpr size_t port_index = static_input_port_index<Node, Name>();
        static_assert(static_input_port_is_random_access<Node, Name>(),
            "requested static input is not declared background");
        size_t background_index = 0;
        size_t sample_index = 0;
        std::optional<size_t> result;
        for_each_port_config(configs, [&](auto const& config) {
            if (is_continuous(config) && !result) {
                if (sample_index == port_index) result = background_index;
                else {
                    if (is_random_access(config)) ++background_index;
                    ++sample_index;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "unknown static sample input port name";
    }

    template<typename Node, fixed_string Name>
    consteval ChannelLayout static_output_port_layout()
    {
        constexpr auto index = static_output_declaration_index<Node, Name>();
        auto const configs = Node::outputs();
        return effective_channel_layout(port_config_at<index>(configs));
    }

    template<typename Node, fixed_string Name>
    consteval bool static_output_port_is_tock()
    {
        return static_output_port_is_tock_at<
            Node, static_output_declaration_index<Node, Name>()>();
    }

    template<typename Node, fixed_string Name>
    consteval OutputProductionKind static_output_port_production()
    {
        return static_output_port_production_at<
            Node, static_output_declaration_index<Node, Name>()>();
    }

    template<typename Node, fixed_string Name>
    consteval bool static_output_port_is_recording()
    {
        return static_output_port_is_recording_at<
            Node, static_output_declaration_index<Node, Name>()>();
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_realtime_output_port_index()
    {
        auto const configs = Node::outputs();
        constexpr size_t port_index = static_output_port_index<Node, Name>();
        static_assert(!static_output_port_is_tock<Node, Name>(),
            "requested static sample output is not produced by tick execution");
        size_t realtime_index = 0;
        size_t sample_index = 0;
        std::optional<size_t> result;
        for_each_port_config(configs, [&](auto const& config) {
            if (is_continuous(config) && !result) {
                if (sample_index == port_index) result = realtime_index;
                else {
                    if (is_tick(config)) ++realtime_index;
                    ++sample_index;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "unknown static sample output port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_tock_output_port_index()
    {
        auto const configs = Node::outputs();
        constexpr size_t port_index = static_output_port_index<Node, Name>();
        static_assert(static_output_port_is_tock<Node, Name>(),
            "requested static output is not produced by tock_coverage");
        size_t tock_index = 0;
        size_t sample_index = 0;
        std::optional<size_t> result;
        for_each_port_config(configs, [&](auto const& config) {
            if (is_continuous(config) && !result) {
                if (sample_index == port_index) result = tock_index;
                else {
                    if (is_tock(config)) ++tock_index;
                    ++sample_index;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "unknown static sample output port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_event_input_port_index()
    {
        auto const configs = Node::inputs();
        size_t found = static_cast<size_t>(-1);
        size_t event_index = 0;
        for_each_port_config(configs, [&](auto const& config) {
            if (!is_continuous(config)) {
                if (config.name == Name.view()) {
                    if (found != static_cast<size_t>(-1)) {
                        throw "duplicate static input port name";
                    }
                    found = event_index;
                }
                ++event_index;
            }
        });
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static event input port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_event_output_port_index()
    {
        auto const configs = Node::outputs();
        size_t found = static_cast<size_t>(-1);
        size_t event_index = 0;
        for_each_port_config(configs, [&](auto const& config) {
            if (!is_continuous(config)) {
                if (config.name == Name.view()) {
                    if (found != static_cast<size_t>(-1)) {
                        throw "duplicate static output port name";
                    }
                    found = event_index;
                }
                ++event_index;
            }
        });
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static event output port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval bool static_event_input_port_is_random_access()
    {
        constexpr auto declaration_index =
            static_input_declaration_index<Node, Name>();
        static_assert(
            static_input_port_kind_at<Node, declaration_index>() == PortKind::event,
            "static input port is not an event port");
        return static_input_port_is_random_access_at<Node, declaration_index>();
    }

    template<typename Node, fixed_string Name>
    consteval bool static_event_output_port_is_tock()
    {
        constexpr auto declaration_index =
            static_output_declaration_index<Node, Name>();
        static_assert(
            static_output_port_kind_at<Node, declaration_index>() == PortKind::event,
            "static output port is not an event port");
        return static_output_port_is_tock_at<Node, declaration_index>();
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_realtime_event_output_port_index()
    {
        auto const configs = Node::outputs();
        constexpr size_t port_index = static_event_output_port_index<Node, Name>();
        static_assert(!static_event_output_port_is_tock<Node, Name>(),
            "requested static event output is not produced by tick execution");
        size_t realtime_index = 0;
        size_t event_index = 0;
        std::optional<size_t> result;
        for_each_port_config(configs, [&](auto const& config) {
            if (!is_continuous(config) && !result) {
                if (event_index == port_index) result = realtime_index;
                else {
                    if (is_tick(config)) ++realtime_index;
                    ++event_index;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "unknown static event output port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_random_access_event_input_port_index()
    {
        auto const configs = Node::inputs();
        constexpr size_t port_index = static_event_input_port_index<Node, Name>();
        static_assert(static_event_input_port_is_random_access<Node, Name>(),
            "requested static event input is not declared background");
        size_t background_index = 0;
        size_t event_index = 0;
        std::optional<size_t> result;
        for_each_port_config(configs, [&](auto const& config) {
            if (!is_continuous(config) && !result) {
                if (event_index == port_index) result = background_index;
                else {
                    if (is_random_access(config)) ++background_index;
                    ++event_index;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "unknown static event input port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_tock_event_output_port_index()
    {
        auto const configs = Node::outputs();
        constexpr size_t port_index = static_event_output_port_index<Node, Name>();
        static_assert(static_event_output_port_is_tock<Node, Name>(),
            "requested static event output is not produced by tock_coverage");
        size_t tock_index = 0;
        size_t event_index = 0;
        std::optional<size_t> result;
        for_each_port_config(configs, [&](auto const& config) {
            if (!is_continuous(config) && !result) {
                if (event_index == port_index) result = tock_index;
                else {
                    if (is_tock(config)) ++tock_index;
                    ++event_index;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "unknown static event output port name";
    }

    template<typename Node, size_t Index>
    consteval ChannelLayout static_output_port_layout_at()
    {
        auto const configs = Node::outputs();
        size_t sample_index = 0;
        std::optional<ChannelLayout> result;
        for_each_port_config(configs, [&](auto const& config) {
            if (is_continuous(config) && !result) {
                if (sample_index == Index) {
                    result = effective_channel_layout(config);
                } else {
                    ++sample_index;
                }
            }
        });
        if (result) {
            return *result;
        }
        throw "static output port index is out of bounds";
    }

/*
 * The access wrappers below operate on the graph's storage sample-port
 * indices. Event entries live in the same authored config array, but never
 * acquire a sample-buffer accessor.
 */
    template<class Channel>
    constexpr size_t channel_index(Channel)
    {
        using ChannelT = std::remove_cvref_t<Channel>;
        return ChannelT::channel_index;
    }

    template<ChannelTypeId Type, class Channel>
    consteval size_t static_channel_index()
    {
        using ChannelT = std::remove_cvref_t<Channel>;
        static_assert(
            std::same_as<typename ChannelT::channel_type, typename RuntimeChannelTypeTraits<Type>::type>,
            "named channel does not belong to the static port channel type"
        );
        return ChannelT::channel_index;
    }

    class StaticInputPortAccess {
    protected:
        InputPort const& _port;

    public:
        constexpr explicit StaticInputPortAccess(InputPort const& port)
            : _port(port)
        {}

        constexpr Sample get(size_t history = 0, size_t channel = 0) const
        {
            return _port.get(history, channel);
        }

        constexpr Sample get_frame(
            size_t sample_offset, size_t channel = 0) const
        {
            return _port.get_frame(sample_offset, channel);
        }

        constexpr BlockView<Sample> get_block(
            size_t block_size, size_t sample_offset = 0) const
        {
            return _port.get_block(block_size, sample_offset);
        }

        constexpr size_t latency() const { return _port.latency(); }
        constexpr size_t buffer_size() const { return _port.buffer_size(); }
        constexpr ChannelLayout channel_layout() const
        {
            return _port.channel_layout();
        }
    };

    template<bool Recording>
    class StaticOutputDispositionObserver;

    template<>
    class StaticOutputDispositionObserver<false> {
    protected:
        constexpr StaticOutputDispositionObserver() noexcept = default;

    };

    template<>
    class StaticOutputDispositionObserver<true> {
        OutputDisposition* _disposition = nullptr;

    protected:
        constexpr explicit StaticOutputDispositionObserver(
            OutputDisposition& disposition) noexcept
            : _disposition(&disposition)
        {}

        constexpr void observe_write() const
        {
            IV_ASSERT(*_disposition != OutputDisposition::voided,
                "an output cannot be written after write_void() in the same invocation");
            *_disposition = OutputDisposition::written;
        }

        constexpr void observe_void() const
        {
            IV_ASSERT(*_disposition != OutputDisposition::written,
                "write_void() cannot follow an ordinary output write in the same invocation");
            *_disposition = OutputDisposition::voided;
        }
    };

    template<bool Recording>
    class StaticOutputPortAccess
        : private StaticOutputDispositionObserver<Recording> {
    protected:
        OutputPort& _port;

        IV_FORCEINLINE constexpr void author_write() const
        {
            if constexpr (Recording) {
                this->observe_write();
            }
        }

    public:
        constexpr explicit StaticOutputPortAccess(OutputPort& port)
        requires (!Recording)
            : StaticOutputDispositionObserver<Recording>()
            , _port(port)
        {}

        constexpr StaticOutputPortAccess(
            OutputPort& port, OutputDisposition& disposition)
        requires Recording
            : StaticOutputDispositionObserver<Recording>(disposition)
            , _port(port)
        {}

        constexpr Sample get(size_t offset = 0, size_t channel = 0) const
        {
            return _port.get(offset, channel);
        }

        constexpr BlockView<Sample> get_block(
            size_t block_size, size_t sample_offset = 0) const
        {
            return _port.get_block(block_size, sample_offset);
        }

        constexpr void write_frame(
            size_t frame_offset, size_t channel, Sample value) const
        {
            author_write();
            _port.write_frame(frame_offset, channel, value);
        }

        constexpr void write_block(
            size_t frame_offset,
            size_t channel,
            BlockView<Sample const> const& source) const
        {
            author_write();
            _port.write_block(frame_offset, channel, source);
        }

        constexpr void push(Sample value) const
        {
            author_write();
            _port.push(value);
        }

        constexpr void push_frame(std::span<Sample const> source) const
        {
            author_write();
            _port.push_frame(source);
        }

        constexpr void push_block(std::span<Sample const> samples) const
        {
            author_write();
            _port.push_block(samples);
        }

        constexpr void push_block(BlockView<Sample const> samples) const
        {
            author_write();
            _port.push_block(samples);
        }

        constexpr void accumulate_block(std::span<Sample const> samples) const
        {
            author_write();
            _port.accumulate_block(samples);
        }

        constexpr void accumulate_block(BlockView<Sample const> samples) const
        {
            author_write();
            _port.accumulate_block(samples);
        }

        constexpr void push_silence(size_t block_size) const
        {
            author_write();
            _port.push_silence(block_size);
        }

        constexpr void update(Sample value, size_t offset = 0) const
        {
            author_write();
            _port.update(value, offset);
        }

        constexpr void update_frame(
            std::span<Sample const> source, size_t offset = 0) const
        {
            author_write();
            _port.update_frame(source, offset);
        }

        constexpr void write_void() const requires Recording
        {
            this->observe_void();
        }

        constexpr size_t position() const { return _port.position(); }
        constexpr size_t buffer_size() const { return _port.buffer_size(); }
        constexpr ChannelLayout channel_layout() const
        {
            return _port.channel_layout();
        }
    };

    template<ChannelTypeId Type>
    class StaticInputSamplePortAccess : public StaticInputPortAccess {

    public:
        constexpr explicit StaticInputSamplePortAccess(InputPort const& port)
            : StaticInputPortAccess(port)
        {}

        constexpr Sample operator()(size_t history = 0) const
        requires (Type == ChannelTypeId::mono)
        {
            return _port.get(history);
        }

        template<class Channel>
        constexpr Sample operator()(Channel, size_t history = 0) const
        requires (Type != ChannelTypeId::mono)
        {
            return _port.get(history, static_channel_index<Type, Channel>());
        }
    };

    template<ChannelTypeId Type, bool Recording>
    class StaticOutputSamplePortAccess
        : public StaticOutputPortAccess<Recording> {

    public:
        class Cell {
            StaticOutputPortAccess<Recording> const& _output;
            size_t _channel;

        public:
            constexpr Cell(
                StaticOutputPortAccess<Recording> const& output,
                size_t channel)
                : _output(output), _channel(channel)
            {}

            constexpr Cell const& operator=(Sample value) const
            {
                _output.write_frame(0, _channel, value);
                return *this;
            }
        };

        constexpr StaticOutputSamplePortAccess(
            OutputPort& port)
        requires (!Recording)
            : StaticOutputPortAccess<Recording>(port)
        {}

        constexpr StaticOutputSamplePortAccess(
            OutputPort& port, OutputDisposition& disposition)
        requires Recording
            : StaticOutputPortAccess<Recording>(port, disposition)
        {}

        constexpr Cell operator()() const
        requires (Type == ChannelTypeId::mono)
        {
            return Cell(*this, 0);
        }

        template<class Channel>
        constexpr Cell operator()(Channel) const
        requires (Type != ChannelTypeId::mono)
        {
            return Cell(*this, static_channel_index<Type, Channel>());
        }
    };

    template<ChannelTypeId Type, SampleStreamLayout Layout>
    class StaticInputBlockPortAccess : public StaticInputPortAccess {
        size_t _block_size;

        class Axis {
            InputPort const& _port;
            size_t _outer;
            size_t _block_size;
        public:
            constexpr Axis(InputPort const& port, size_t outer, size_t block_size) :
                _port(port), _outer(outer), _block_size(block_size) {}
            template<class Channel>
            constexpr Sample operator[](Channel) const
            requires (Layout == SampleStreamLayout::interleaved)
            {
                IV_ASSERT(_outer < _block_size, "sample frame index out of bounds");
                return _port.get_frame(_outer, static_channel_index<Type, Channel>());
            }
            constexpr Sample operator[](size_t frame) const
            requires (Layout == SampleStreamLayout::planar)
            {
                IV_ASSERT(frame < _block_size, "sample frame index out of bounds");
                return _port.get_frame(frame, _outer);
            }
        };

    public:
        constexpr StaticInputBlockPortAccess(InputPort const& port, size_t block_size)
            : StaticInputPortAccess(port), _block_size(block_size)
        {}
        constexpr Sample operator[](size_t frame) const requires (Type == ChannelTypeId::mono)
        {
            IV_ASSERT(frame < _block_size, "sample frame index out of bounds");
            return this->_port.get_frame(frame);
        }
        template<class Channel>
        constexpr Axis operator[](Channel) const requires (Type != ChannelTypeId::mono && Layout == SampleStreamLayout::planar)
        {
            auto const index = static_channel_index<Type, Channel>();
            return Axis(this->_port, index, _block_size);
        }
        constexpr Axis operator[](size_t frame) const requires (Type != ChannelTypeId::mono && Layout == SampleStreamLayout::interleaved)
        {
            return Axis(this->_port, frame, _block_size);
        }
    };

    template<ChannelTypeId Type, SampleStreamLayout Layout, bool Recording>
    class StaticOutputBlockPortAccess
        : public StaticOutputPortAccess<Recording> {
        size_t _block_size;

        class Cell {
            StaticOutputPortAccess<Recording> const& _output;
            size_t _frame;
            size_t _channel;
        public:
            constexpr Cell(
                StaticOutputPortAccess<Recording> const& output,
                size_t frame,
                size_t channel)
                : _output(output), _frame(frame), _channel(channel)
            {}
            constexpr Cell const& operator=(Sample value) const
            {
                _output.write_frame(_frame, _channel, value);
                return *this;
            }
        };
        class Axis {
            StaticOutputPortAccess<Recording> const& _output;
            size_t _outer;
            size_t _block_size;
        public:
            constexpr Axis(
                StaticOutputPortAccess<Recording> const& output,
                size_t outer,
                size_t block_size)
                : _output(output), _outer(outer), _block_size(block_size)
            {}
            constexpr void write_block(BlockView<Sample const> const& source) const
            requires (Layout == SampleStreamLayout::planar)
            {
                IV_ASSERT(source.size() == _block_size, "source block size does not match output block size");
                _output.write_block(0, _outer, source);
            }
            template<class Channel>
            constexpr Cell operator[](Channel) const requires (Layout == SampleStreamLayout::interleaved)
            {
                IV_ASSERT(_outer < _block_size, "sample frame index out of bounds");
                return Cell(
                    _output,
                    _outer,
                    static_channel_index<Type, Channel>());
            }
            constexpr Cell operator[](size_t frame) const requires (Layout == SampleStreamLayout::planar)
            {
                IV_ASSERT(frame < _block_size, "sample frame index out of bounds");
                return Cell(_output, frame, _outer);
            }
        };

    public:
        constexpr StaticOutputBlockPortAccess(
            OutputPort& port,
            size_t block_size)
        requires (!Recording)
            : StaticOutputPortAccess<Recording>(port)
            , _block_size(block_size)
        {}

        constexpr StaticOutputBlockPortAccess(
            OutputPort& port,
            size_t block_size,
            OutputDisposition& disposition)
        requires Recording
            : StaticOutputPortAccess<Recording>(port, disposition)
            , _block_size(block_size)
        {}
        constexpr Cell operator[](size_t frame) const requires (Type == ChannelTypeId::mono)
        {
            IV_ASSERT(frame < _block_size, "sample frame index out of bounds");
            return Cell(*this, frame, 0);
        }
        template<class Channel>
        constexpr Axis operator[](Channel) const requires (Type != ChannelTypeId::mono && Layout == SampleStreamLayout::planar)
        {
            auto const index = static_channel_index<Type, Channel>();
            return Axis(*this, index, _block_size);
        }
        constexpr Axis operator[](size_t frame) const requires (Type != ChannelTypeId::mono && Layout == SampleStreamLayout::interleaved)
        {
            return Axis(*this, frame, _block_size);
        }
    };

    enum class TickPortAccessKind : std::uint8_t {
        point,
        block,
    };

    // Type-owned continuous access used by GraphJit. The selected type is one
    // immutable realization fact. A single-type declaration therefore exposes
    // this facade directly; a multi-type declaration exposes it only through
    // is<T>() / as<T>() below.
    template<RegisteredContinuousValue Value,
             ChannelTypeId ChannelType,
             SampleStreamLayout Layout,
             TickPortAccessKind Access>
    class StaticContinuousInputAccess {
        ContinuousInputPort<Value> port_{};

        class Axis {
            ContinuousInputPort<Value> const* port_ = nullptr;
            std::size_t outer_ = 0;

        public:
            constexpr Axis(
                ContinuousInputPort<Value> const& port,
                std::size_t outer) noexcept
                : port_(&port), outer_(outer)
            {}

            template<class Channel>
            [[nodiscard]] constexpr auto operator[](Channel) const noexcept
            requires (Layout == SampleStreamLayout::interleaved)
            {
                return port_->get_value(
                    outer_, static_channel_index<ChannelType, Channel>());
            }

            [[nodiscard]] constexpr auto operator[](
                std::size_t value) const noexcept
            requires (Layout == SampleStreamLayout::planar)
            {
                return port_->get_value(value, outer_);
            }
        };

        template<class Channel>
        static consteval std::size_t selected_channel()
        {
            return static_channel_index<ChannelType, Channel>();
        }

    public:
        constexpr explicit StaticContinuousInputAccess(
            ContinuousInputPort<Value> port) noexcept
            : port_(port)
        {}

        [[nodiscard]] constexpr auto get(
            std::size_t history = 0,
            std::size_t channel = 0) const noexcept
        {
            return port_.get(history, channel);
        }

        [[nodiscard]] constexpr auto get_value(
            std::size_t value,
            std::size_t channel = 0) const noexcept
        requires (Access == TickPortAccessKind::block)
        {
            return port_.get_value(value, channel);
        }

        [[nodiscard]] constexpr auto operator()(
            std::size_t history = 0) const noexcept
        requires (ChannelType == ChannelTypeId::mono)
        {
            return port_.get(history);
        }

        template<class Channel>
        [[nodiscard]] constexpr auto operator()(
            Channel, std::size_t history = 0) const noexcept
        requires (ChannelType != ChannelTypeId::mono)
        {
            return port_.get(history, selected_channel<Channel>());
        }

        [[nodiscard]] constexpr auto operator[](
            std::size_t value) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType == ChannelTypeId::mono)
        {
            return port_.get_value(value);
        }

        template<class Channel>
        [[nodiscard]] constexpr Axis operator[](Channel) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType != ChannelTypeId::mono
            && Layout == SampleStreamLayout::planar)
        {
            return Axis{port_, selected_channel<Channel>()};
        }

        [[nodiscard]] constexpr Axis operator[](
            std::size_t value) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType != ChannelTypeId::mono
            && Layout == SampleStreamLayout::interleaved)
        {
            IV_ASSERT(value < port_.block_size(),
                "continuous input value index is out of bounds");
            return Axis{port_, value};
        }

        template<class Channel>
        [[nodiscard]] constexpr auto block(Channel) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType != ChannelTypeId::mono)
        {
            return port_.block(selected_channel<Channel>());
        }

        [[nodiscard]] constexpr auto block() const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType == ChannelTypeId::mono)
        {
            return port_.block();
        }

        [[nodiscard]] constexpr auto block(std::size_t channel) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType != ChannelTypeId::mono)
        {
            return port_.block(channel);
        }

        template<class Channel>
        [[nodiscard]] constexpr auto block_extended(Channel) const noexcept
        requires (ChannelType != ChannelTypeId::mono)
        {
            return port_.block_extended(selected_channel<Channel>());
        }

        [[nodiscard]] constexpr auto block_extended() const noexcept
        requires (ChannelType == ChannelTypeId::mono)
        {
            return port_.block_extended();
        }

        [[nodiscard]] constexpr FractionalGlobalIndex begin() const noexcept
        {
            return port_.begin();
        }

        [[nodiscard]] constexpr std::size_t block_size() const noexcept
        requires (Access == TickPortAccessKind::block)
        {
            return port_.block_size();
        }

        [[nodiscard]] constexpr std::uint64_t value_index() const noexcept
        {
            return port_.value_index();
        }

        [[nodiscard]] constexpr std::size_t history() const noexcept
        {
            return port_.history();
        }

        [[nodiscard]] constexpr std::size_t latency() const noexcept
        {
            return port_.latency();
        }

        [[nodiscard]] constexpr ChannelLayout channel_layout() const noexcept
        {
            return port_.channel_layout();
        }
    };

    template<RegisteredContinuousValue Value,
             ChannelTypeId ChannelType,
             SampleStreamLayout Layout,
             TickPortAccessKind Access,
             bool Recording>
    class StaticContinuousOutputAccess
        : private StaticOutputDispositionObserver<Recording> {
        ContinuousOutputPort<Value> port_{};

        class Axis {
            StaticContinuousOutputAccess const* output_ = nullptr;
            std::size_t outer_ = 0;

        public:
            constexpr Axis(
                StaticContinuousOutputAccess const& output,
                std::size_t outer) noexcept
                : output_(&output), outer_(outer)
            {}

            template<class Channel>
            [[nodiscard]] constexpr decltype(auto) operator[](
                Channel) const noexcept
            requires (Layout == SampleStreamLayout::interleaved)
            {
                output_->author_write();
                return output_->port_.value(
                    outer_, static_channel_index<ChannelType, Channel>());
            }

            [[nodiscard]] constexpr decltype(auto) operator[](
                std::size_t value) const noexcept
            requires (Layout == SampleStreamLayout::planar)
            {
                output_->author_write();
                return output_->port_.value(value, outer_);
            }
        };

        template<class Channel>
        static consteval std::size_t selected_channel()
        {
            return static_channel_index<ChannelType, Channel>();
        }

        constexpr void author_write() const noexcept
        {
            if constexpr (Recording) this->observe_write();
        }

    public:
        constexpr explicit StaticContinuousOutputAccess(
            ContinuousOutputPort<Value> port) noexcept
        requires (!Recording)
            : StaticOutputDispositionObserver<Recording>()
            , port_(port)
        {}

        constexpr StaticContinuousOutputAccess(
            ContinuousOutputPort<Value> port,
            OutputDisposition& disposition) noexcept
        requires Recording
            : StaticOutputDispositionObserver<Recording>(disposition)
            , port_(port)
        {}

        [[nodiscard]] constexpr auto get(
            std::size_t offset = 0,
            std::size_t channel = 0) const noexcept
        {
            return port_.get(offset, channel);
        }

        [[nodiscard]] constexpr decltype(auto) value(
            std::size_t offset = 0,
            std::size_t channel = 0) const noexcept
        requires (Access == TickPortAccessKind::block)
        {
            author_write();
            return port_.value(offset, channel);
        }

        [[nodiscard]] constexpr decltype(auto) operator()() const noexcept
        requires (ChannelType == ChannelTypeId::mono)
        {
            author_write();
            return port_.value(0);
        }

        template<class Channel>
        [[nodiscard]] constexpr decltype(auto) operator()(Channel) const noexcept
        requires (ChannelType != ChannelTypeId::mono)
        {
            author_write();
            return port_.value(0, selected_channel<Channel>());
        }

        [[nodiscard]] constexpr decltype(auto) operator()(
            std::size_t channel) const noexcept
        {
            author_write();
            return port_.value(0, channel);
        }

        [[nodiscard]] constexpr decltype(auto) operator[](
            std::size_t value) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType == ChannelTypeId::mono)
        {
            author_write();
            return port_.value(value);
        }

        template<class Channel>
        [[nodiscard]] constexpr Axis operator[](Channel) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType != ChannelTypeId::mono
            && Layout == SampleStreamLayout::planar)
        {
            return Axis{*this, selected_channel<Channel>()};
        }

        [[nodiscard]] constexpr Axis operator[](
            std::size_t value) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType != ChannelTypeId::mono
            && Layout == SampleStreamLayout::interleaved)
        {
            IV_ASSERT(value < port_.block_size(),
                "continuous output value index is out of bounds");
            return Axis{*this, value};
        }

        template<class Channel>
        [[nodiscard]] constexpr auto block(Channel) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType != ChannelTypeId::mono)
        {
            author_write();
            return port_.block(selected_channel<Channel>());
        }

        [[nodiscard]] constexpr auto block() const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType == ChannelTypeId::mono)
        {
            author_write();
            return port_.block();
        }

        [[nodiscard]] constexpr auto block(std::size_t channel) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType != ChannelTypeId::mono)
        {
            author_write();
            return port_.block(channel);
        }

        constexpr void push(Value const& value) const noexcept
        requires (ContinuousValueType<Value>::rank == 0)
        {
            author_write();
            port_.push(value);
        }

        constexpr void push(FFTBlock value) const noexcept
        requires std::same_as<Value, FFTBlock>
        {
            author_write();
            port_.push(value);
        }

        template<class Range>
            requires requires(Range const& values) {
                values.size();
                values[std::size_t{}];
            }
        constexpr void push_block(Range const& values) const noexcept
        requires (Access == TickPortAccessKind::block
            && ChannelType == ChannelTypeId::mono)
        {
            author_write();
            for (std::size_t value = 0; value < values.size(); ++value) {
                port_.push(values[value]);
            }
        }

        constexpr void update(
            Value const& value,
            std::size_t offset = 0,
            std::size_t channel = 0) const noexcept
        requires (ContinuousValueType<Value>::rank == 0)
        {
            author_write();
            port_.update(value, offset, channel);
        }

        constexpr void update(
            FFTBlock value,
            std::size_t offset = 0,
            std::size_t channel = 0) const noexcept
        requires std::same_as<Value, FFTBlock>
        {
            author_write();
            port_.update(value, offset, channel);
        }

        constexpr void clear_block() const noexcept
        requires (Access == TickPortAccessKind::block)
        {
            author_write();
            auto const channels = channel_count(port_.channel_layout());
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto values = port_.block(channel);
                for (std::size_t index = 0; index < values.size(); ++index) {
                    ContinuousValueType<Value>::clear_value(values[index]);
                }
            }
        }

        constexpr void write_void() const noexcept requires Recording
        {
            this->observe_void();
        }

        [[nodiscard]] constexpr FractionalGlobalIndex begin() const noexcept
        {
            return port_.begin();
        }

        [[nodiscard]] constexpr std::size_t block_size() const noexcept
        requires (Access == TickPortAccessKind::block)
        {
            return port_.block_size();
        }

        [[nodiscard]] constexpr std::uint64_t value_index() const noexcept
        {
            return port_.value_index();
        }

        [[nodiscard]] constexpr std::size_t history() const noexcept
        {
            return port_.history();
        }

        [[nodiscard]] constexpr std::size_t latency() const noexcept
        {
            return port_.latency();
        }

        [[nodiscard]] constexpr ChannelLayout channel_layout() const noexcept
        {
            return port_.channel_layout();
        }
    };

    template<class Tuple, class Binding,
             ChannelTypeId ChannelType,
             SampleStreamLayout Layout,
             TickPortAccessKind Access>
    class StaticVariantContinuousInputAccess;

    template<class... Values, class Binding,
             ChannelTypeId ChannelType,
             SampleStreamLayout Layout,
             TickPortAccessKind Access>
    class StaticVariantContinuousInputAccess<
        std::tuple<Values...>, Binding, ChannelType, Layout, Access> {
        Binding const* binding_ = nullptr;
        FractionalGlobalIndex begin_{};
        std::uint64_t begin_ordinal_ = 0;
        std::uint64_t sequential_begin_ordinal_ = 0;
        std::size_t block_size_ = 0;

    public:
        constexpr StaticVariantContinuousInputAccess(
            Binding const& binding,
            FractionalGlobalIndex begin,
            std::uint64_t begin_ordinal,
            std::uint64_t sequential_begin_ordinal,
            std::size_t block_size) noexcept
            : binding_(&binding)
            , begin_(begin)
            , begin_ordinal_(begin_ordinal)
            , sequential_begin_ordinal_(sequential_begin_ordinal)
            , block_size_(block_size)
        {}

        template<RegisteredContinuousValue Value>
        [[nodiscard]] constexpr bool is() const noexcept
        requires ((std::same_as<Value, Values> || ...))
        {
            return binding_->storage.geometry.value_type
                == continuous_value_type_id_v<Value>;
        }

        template<RegisteredContinuousValue Value>
        [[nodiscard]] constexpr auto as() const noexcept
        requires ((std::same_as<Value, Values> || ...))
        {
            IV_ASSERT(is<Value>(),
                "continuous input alternative does not match its resolved type");
            return StaticContinuousInputAccess<
                Value, ChannelType, Layout, Access>{
                ContinuousInputPort<Value>{
                    *binding_,
                    begin_,
                    begin_ordinal_,
                    sequential_begin_ordinal_,
                    block_size_}};
        }
    };

    template<class Tuple, class Binding,
             ChannelTypeId ChannelType,
             SampleStreamLayout Layout,
             TickPortAccessKind Access,
             bool Recording>
    class StaticVariantContinuousOutputAccess;

    template<class... Values, class Binding,
             ChannelTypeId ChannelType,
             SampleStreamLayout Layout,
             TickPortAccessKind Access,
             bool Recording>
    class StaticVariantContinuousOutputAccess<
        std::tuple<Values...>, Binding, ChannelType, Layout, Access, Recording> {
        Binding const* binding_ = nullptr;
        FractionalGlobalIndex begin_{};
        std::uint64_t begin_ordinal_ = 0;
        std::uint64_t sequential_begin_ordinal_ = 0;
        std::size_t block_size_ = 0;
        OutputDisposition* disposition_ = nullptr;

    public:
        constexpr StaticVariantContinuousOutputAccess(
            Binding const& binding,
            FractionalGlobalIndex begin,
            std::uint64_t begin_ordinal,
            std::uint64_t sequential_begin_ordinal,
            std::size_t block_size) noexcept
        requires (!Recording)
            : binding_(&binding)
            , begin_(begin)
            , begin_ordinal_(begin_ordinal)
            , sequential_begin_ordinal_(sequential_begin_ordinal)
            , block_size_(block_size)
        {}

        constexpr StaticVariantContinuousOutputAccess(
            Binding const& binding,
            FractionalGlobalIndex begin,
            std::uint64_t begin_ordinal,
            std::uint64_t sequential_begin_ordinal,
            std::size_t block_size,
            OutputDisposition& disposition) noexcept
        requires Recording
            : binding_(&binding)
            , begin_(begin)
            , begin_ordinal_(begin_ordinal)
            , sequential_begin_ordinal_(sequential_begin_ordinal)
            , block_size_(block_size)
            , disposition_(&disposition)
        {}

        template<RegisteredContinuousValue Value>
        [[nodiscard]] constexpr bool is() const noexcept
        requires ((std::same_as<Value, Values> || ...))
        {
            return binding_->storage.geometry.value_type
                == continuous_value_type_id_v<Value>;
        }

        template<RegisteredContinuousValue Value>
        [[nodiscard]] constexpr auto as() const noexcept
        requires ((std::same_as<Value, Values> || ...))
        {
            IV_ASSERT(is<Value>(),
                "continuous output alternative does not match its resolved type");
            auto port = ContinuousOutputPort<Value>{
                *binding_,
                begin_,
                begin_ordinal_,
                sequential_begin_ordinal_,
                block_size_};
            if constexpr (Recording) {
                return StaticContinuousOutputAccess<
                    Value, ChannelType, Layout, Access, true>{
                        port, *disposition_};
            } else {
                return StaticContinuousOutputAccess<
                    Value, ChannelType, Layout, Access, false>{port};
            }
        }
    };
}
