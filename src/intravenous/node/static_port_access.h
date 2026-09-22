#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/ports.h>

#include <cstddef>

namespace iv::details {
    template<typename Node, fixed_string Name>
    consteval InputConfig static_input_config()
    {
        static constexpr auto configs = Node::inputs();
        InputConfig const* found = nullptr;
        for (InputConfig const& config : configs) {
            if (config.name != Name.view()) continue;
            if (found != nullptr) throw "duplicate static input port name";
            found = &config;
        }
        if (found == nullptr) throw "unknown static input port name";
        return *found;
    }

    template<typename Node, fixed_string Name>
    consteval OutputConfig static_output_config()
    {
        static constexpr auto configs = Node::outputs();
        OutputConfig const* found = nullptr;
        for (OutputConfig const& config : configs) {
            if (config.name != Name.view()) continue;
            if (found != nullptr) throw "duplicate static output port name";
            found = &config;
        }
        if (found == nullptr) throw "unknown static output port name";
        return *found;
    }

    template<typename Node, fixed_string Name>
    consteval PortKind static_input_port_kind()
    {
        return is_sample(static_input_config<Node, Name>())
            ? PortKind::sample : PortKind::event;
    }

    template<typename Node, fixed_string Name>
    consteval PortKind static_output_port_kind()
    {
        return is_sample(static_output_config<Node, Name>())
            ? PortKind::sample : PortKind::event;
    }

    template<typename Node>
    consteval size_t static_output_count()
    {
        return count_sample_ports(Node::outputs());
    }

    template<typename Node>
    inline constexpr size_t static_output_count_v = static_output_count<Node>();

    template<typename Node, fixed_string Name>
    consteval size_t static_input_port_index()
    {
        static constexpr auto configs = Node::inputs();
        size_t found = static_cast<size_t>(-1);
        size_t sample_index = 0;
        for (size_t i = 0; i < configs.size(); ++i) {
            if (is_sample(configs[i])) {
                if (configs[i].name == Name.view()) {
                    if (found != static_cast<size_t>(-1)) {
                        throw "duplicate static input port name";
                    }
                    found = sample_index;
                }
                ++sample_index;
            }
        }
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static sample input port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval SampleInputProperties static_input_port_properties()
    {
        static constexpr auto configs = Node::inputs();
        for (InputConfig const& config : configs) {
            if (auto const* properties =
                    std::get_if<SampleInputProperties>(&config.kind);
                properties != nullptr && config.name == Name.view()) {
                return *properties;
            }
        }
        throw "unknown static sample input port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_output_port_index()
    {
        static constexpr auto configs = Node::outputs();
        size_t found = static_cast<size_t>(-1);
        size_t sample_index = 0;
        for (size_t i = 0; i < configs.size(); ++i) {
            if (is_sample(configs[i])) {
                if (configs[i].name == Name.view()) {
                    if (found != static_cast<size_t>(-1)) {
                        throw "duplicate static output port name";
                    }
                    found = sample_index;
                }
                ++sample_index;
            }
        }
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static sample output port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval SampleOutputProperties static_output_port_properties()
    {
        static constexpr auto configs = Node::outputs();
        for (OutputConfig const& config : configs) {
            if (auto const* properties =
                    std::get_if<SampleOutputProperties>(&config.kind);
                properties != nullptr && config.name == Name.view()) {
                return *properties;
            }
        }
        throw "unknown static sample output port name";
    }

    template<typename Node, fixed_string Name>
    consteval ChannelLayout static_input_port_layout()
    {
        return effective_channel_layout(static_input_port_properties<Node, Name>());
    }

    template<typename Node, fixed_string Name>
    consteval bool static_input_port_is_indexed()
    {
        InputConfig const config = static_input_config<Node, Name>();
        if (!is_sample(config)) throw "static input port is not a sample port";
        return is_indexed(config);
    }

    // Indexed callback contexts contain only indexed ports. Convert a declaration's
    // physical sample-port ordinal to its compact indexed-port ordinal so
    // access callbacks never receive fake realtime placeholders.
    template<typename Node, fixed_string Name>
    consteval size_t static_indexed_input_port_index()
    {
        static constexpr auto configs = Node::inputs();
        constexpr size_t port_index = static_input_port_index<Node, Name>();
        static_assert(static_input_port_is_indexed<Node, Name>(),
            "requested static input is not declared indexed");
        size_t indexed_index = 0;
        size_t sample_index = 0;
        for (InputConfig const& config : configs) {
            if (!is_sample(config)) continue;
            if (sample_index == port_index) return indexed_index;
            if (is_indexed(config)) ++indexed_index;
            ++sample_index;
        }
        throw "unknown static sample input port name";
    }

    template<typename Node, fixed_string Name>
    consteval ChannelLayout static_output_port_layout()
    {
        return effective_channel_layout(static_output_port_properties<Node, Name>());
    }

    template<typename Node, fixed_string Name>
    consteval bool static_output_port_is_indexed()
    {
        OutputConfig const config = static_output_config<Node, Name>();
        if (!is_sample(config)) throw "static output port is not a sample port";
        return is_indexed(config);
    }

    template<typename Node, fixed_string Name>
    consteval bool static_output_port_is_tick_record()
    {
        OutputConfig const config = static_output_config<Node, Name>();
        return is_tick_record(config.access);
    }

    template<typename Node, fixed_string Name>
    consteval bool static_output_port_is_tock_produced()
    {
        OutputConfig const config = static_output_config<Node, Name>();
        return is_tock_produced(config.access);
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_tock_output_port_index()
    {
        static constexpr auto configs = Node::outputs();
        constexpr size_t port_index = static_output_port_index<Node, Name>();
        static_assert(static_output_port_is_tock_produced<Node, Name>(),
            "requested static output is not produced by tock_coverage");
        size_t tock_index = 0;
        size_t sample_index = 0;
        for (OutputConfig const& config : configs) {
            if (!is_sample(config)) continue;
            if (sample_index == port_index) return tock_index;
            if (is_tock_produced(config.access)) ++tock_index;
            ++sample_index;
        }
        throw "unknown static sample output port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_tick_record_output_port_index()
    {
        static constexpr auto configs = Node::outputs();
        constexpr size_t port_index = static_output_port_index<Node, Name>();
        static_assert(static_output_port_is_tick_record<Node, Name>(),
            "requested static output is not produced by tick_record");
        size_t record_index = 0;
        size_t sample_index = 0;
        for (OutputConfig const& config : configs) {
            if (!is_sample(config)) continue;
            if (sample_index == port_index) return record_index;
            if (is_tick_record(config.access)) ++record_index;
            ++sample_index;
        }
        throw "unknown static sample output port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_event_input_port_index()
    {
        static constexpr auto configs = Node::inputs();
        size_t found = static_cast<size_t>(-1);
        size_t event_index = 0;
        for (InputConfig const& config : configs) {
            if (is_sample(config)) continue;
            if (config.name == Name.view()) {
                if (found != static_cast<size_t>(-1)) {
                    throw "duplicate static input port name";
                }
                found = event_index;
            }
            ++event_index;
        }
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static event input port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_event_output_port_index()
    {
        static constexpr auto configs = Node::outputs();
        size_t found = static_cast<size_t>(-1);
        size_t event_index = 0;
        for (OutputConfig const& config : configs) {
            if (is_sample(config)) continue;
            if (config.name == Name.view()) {
                if (found != static_cast<size_t>(-1)) {
                    throw "duplicate static output port name";
                }
                found = event_index;
            }
            ++event_index;
        }
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static event output port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval bool static_event_input_port_is_indexed()
    {
        InputConfig const config = static_input_config<Node, Name>();
        if (is_sample(config)) throw "static input port is not an event port";
        return is_indexed(config);
    }

    template<typename Node, fixed_string Name>
    consteval bool static_event_output_port_is_indexed()
    {
        OutputConfig const config = static_output_config<Node, Name>();
        if (is_sample(config)) throw "static output port is not an event port";
        return is_indexed(config);
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_indexed_event_input_port_index()
    {
        static constexpr auto configs = Node::inputs();
        constexpr size_t port_index = static_event_input_port_index<Node, Name>();
        static_assert(static_event_input_port_is_indexed<Node, Name>(),
            "requested static event input is not declared indexed");
        size_t indexed_index = 0;
        size_t event_index = 0;
        for (InputConfig const& config : configs) {
            if (is_sample(config)) continue;
            if (event_index == port_index) return indexed_index;
            if (is_indexed(config)) ++indexed_index;
            ++event_index;
        }
        throw "unknown static event input port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_tock_event_output_port_index()
    {
        static constexpr auto configs = Node::outputs();
        constexpr size_t port_index = static_event_output_port_index<Node, Name>();
        static_assert(static_output_port_is_tock_produced<Node, Name>(),
            "requested static event output is not produced by tock_coverage");
        size_t tock_index = 0;
        size_t event_index = 0;
        for (OutputConfig const& config : configs) {
            if (is_sample(config)) continue;
            if (event_index == port_index) return tock_index;
            if (is_tock_produced(config.access)) ++tock_index;
            ++event_index;
        }
        throw "unknown static event output port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_tick_record_event_output_port_index()
    {
        static constexpr auto configs = Node::outputs();
        constexpr size_t port_index = static_event_output_port_index<Node, Name>();
        static_assert(static_output_port_is_tick_record<Node, Name>(),
            "requested static event output is not produced by tick_record");
        size_t record_index = 0;
        size_t event_index = 0;
        for (OutputConfig const& config : configs) {
            if (is_sample(config)) continue;
            if (event_index == port_index) return record_index;
            if (is_tick_record(config.access)) ++record_index;
            ++event_index;
        }
        throw "unknown static event output port name";
    }

    template<typename Node, size_t Index>
    consteval ChannelLayout static_output_port_layout_at()
    {
        static constexpr auto configs = Node::outputs();
        size_t sample_index = 0;
        for (OutputConfig const& config : configs) {
            if (auto const* properties =
                    std::get_if<SampleOutputProperties>(&config.kind);
                properties != nullptr) {
                if (sample_index == Index) {
                    return effective_channel_layout(*properties);
                }
                ++sample_index;
            }
        }
        throw "static output port index is out of bounds";
    }

/*
 * The access wrappers below operate on the graph's physical sample-port
 * ordinals. Event entries live in the same authored config array, but never
 * acquire a sample-buffer accessor.
 */
    template<class Channel>
    constexpr size_t channel_ordinal(Channel)
    {
        using ChannelT = std::remove_cvref_t<Channel>;
        return ChannelT::channel_ordinal;
    }

    template<ChannelTypeId Type, class Channel>
    consteval size_t static_channel_ordinal()
    {
        using ChannelT = std::remove_cvref_t<Channel>;
        static_assert(
            std::same_as<typename ChannelT::channel_type, typename RuntimeChannelTypeTraits<Type>::type>,
            "named channel does not belong to the static port channel type"
        );
        return ChannelT::channel_ordinal;
    }

    template<ChannelTypeId Type>
    class StaticInputSamplePortAccess {
        InputPort const& _port;

    public:
        constexpr explicit StaticInputSamplePortAccess(InputPort const& port) : _port(port) {}

        constexpr Sample operator()(size_t history = 0) const
        requires (Type == ChannelTypeId::mono)
        {
            return _port.get(history);
        }

        template<class Channel>
        constexpr Sample operator()(Channel, size_t history = 0) const
        requires (Type != ChannelTypeId::mono)
        {
            return _port.get(history, static_channel_ordinal<Type, Channel>());
        }
    };

    template<ChannelTypeId Type>
    class StaticOutputSamplePortAccess {
        OutputPort& _port;

    public:
        class Cell {
            OutputPort& _port;
            size_t _channel;

        public:
            constexpr Cell(OutputPort& port, size_t channel) : _port(port), _channel(channel) {}

            constexpr Cell const& operator=(Sample value) const
            {
                _port.write_frame(0, _channel, value);
                return *this;
            }
        };

        constexpr explicit StaticOutputSamplePortAccess(OutputPort& port) : _port(port) {}

        constexpr Cell operator()() const
        requires (Type == ChannelTypeId::mono)
        {
            return Cell(_port, 0);
        }

        template<class Channel>
        constexpr Cell operator()(Channel) const
        requires (Type != ChannelTypeId::mono)
        {
            return Cell(_port, static_channel_ordinal<Type, Channel>());
        }
    };

    template<ChannelTypeId Type, SampleStreamLayout Layout>
    class StaticInputBlockPortAccess {
        InputPort const& _port;
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
                return _port.get_frame(_outer, static_channel_ordinal<Type, Channel>());
            }
            constexpr Sample operator[](size_t frame) const
            requires (Layout == SampleStreamLayout::planar)
            {
                IV_ASSERT(frame < _block_size, "sample frame index out of bounds");
                return _port.get_frame(frame, _outer);
            }
        };

    public:
        constexpr StaticInputBlockPortAccess(InputPort const& port, size_t block_size) : _port(port), _block_size(block_size) {}
        constexpr Sample operator[](size_t frame) const requires (Type == ChannelTypeId::mono)
        {
            IV_ASSERT(frame < _block_size, "sample frame index out of bounds");
            return _port.get_frame(frame);
        }
        template<class Channel>
        constexpr Axis operator[](Channel) const requires (Type != ChannelTypeId::mono && Layout == SampleStreamLayout::planar)
        {
            auto const ordinal = static_channel_ordinal<Type, Channel>();
            return Axis(_port, ordinal, _block_size);
        }
        constexpr Axis operator[](size_t frame) const requires (Type != ChannelTypeId::mono && Layout == SampleStreamLayout::interleaved)
        {
            return Axis(_port, frame, _block_size);
        }
        operator InputPort const&() const
        {
            return _port;
        }
    };

    template<ChannelTypeId Type, SampleStreamLayout Layout>
    class StaticOutputBlockPortAccess {
        OutputPort& _port;
        size_t _block_size;

        class Cell {
            OutputPort& _port;
            size_t _frame;
            size_t _channel;
        public:
            constexpr Cell(OutputPort& port, size_t frame, size_t channel) : _port(port), _frame(frame), _channel(channel) {}
            constexpr Cell const& operator=(Sample value) const { _port.write_frame(_frame, _channel, value); return *this; }
        };
        class Axis {
            OutputPort& _port;
            size_t _outer;
            size_t _block_size;
        public:
            constexpr Axis(OutputPort& port, size_t outer, size_t block_size) : _port(port), _outer(outer), _block_size(block_size) {}
            constexpr void write_block(BlockView<Sample const> const& source) const
            requires (Layout == SampleStreamLayout::planar)
            {
                IV_ASSERT(source.size() == _block_size, "source block size does not match output block size");
                _port.write_block(0, _outer, source);
            }
            template<class Channel>
            constexpr Cell operator[](Channel) const requires (Layout == SampleStreamLayout::interleaved)
            {
                IV_ASSERT(_outer < _block_size, "sample frame index out of bounds");
                return Cell(_port, _outer, static_channel_ordinal<Type, Channel>());
            }
            constexpr Cell operator[](size_t frame) const requires (Layout == SampleStreamLayout::planar)
            {
                IV_ASSERT(frame < _block_size, "sample frame index out of bounds");
                return Cell(_port, frame, _outer);
            }
            operator OutputPort&()
            {
                return _port;
            }
        };

    public:
        constexpr StaticOutputBlockPortAccess(OutputPort& port, size_t block_size) : _port(port), _block_size(block_size) {}
        constexpr Cell operator[](size_t frame) const requires (Type == ChannelTypeId::mono)
        {
            IV_ASSERT(frame < _block_size, "sample frame index out of bounds");
            return Cell(_port, frame, 0);
        }
        template<class Channel>
        constexpr Axis operator[](Channel) const requires (Type != ChannelTypeId::mono && Layout == SampleStreamLayout::planar)
        {
            auto const ordinal = static_channel_ordinal<Type, Channel>();
            return Axis(_port, ordinal, _block_size);
        }
        constexpr Axis operator[](size_t frame) const requires (Type != ChannelTypeId::mono && Layout == SampleStreamLayout::interleaved)
        {
            return Axis(_port, frame, _block_size);
        }
    };
}
