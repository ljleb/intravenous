#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/ports.h>

#include <cstddef>

namespace iv::details {
    template<typename Node, fixed_string Name>
    consteval size_t static_input_port_index()
    {
        static constexpr auto configs = Node::inputs();
        size_t found = configs.size();
        for (size_t i = 0; i < configs.size(); ++i) {
            if (configs[i].name == Name.view()) {
                if (found != configs.size()) {
                    throw "duplicate static input port name";
                }
                found = i;
            }
        }
        if (found == configs.size()) {
            throw "unknown static input port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_output_port_index()
    {
        static constexpr auto configs = Node::outputs();
        size_t found = configs.size();
        for (size_t i = 0; i < configs.size(); ++i) {
            if (configs[i].name == Name.view()) {
                if (found != configs.size()) {
                    throw "duplicate static output port name";
                }
                found = i;
            }
        }
        if (found == configs.size()) {
            throw "unknown static output port name";
        }
        return found;
    }

    template<typename Node, fixed_string Name>
    consteval ChannelLayout static_input_port_layout()
    {
        static constexpr auto configs = Node::inputs();
        return effective_channel_layout(configs[static_input_port_index<Node, Name>()]);
    }

    template<typename Node, fixed_string Name>
    consteval ChannelLayout static_output_port_layout()
    {
        static constexpr auto configs = Node::outputs();
        return effective_channel_layout(configs[static_output_port_index<Node, Name>()]);
    }
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
