#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/ports.h>

#include <cstddef>

namespace iv::details {
    // Port configs own std::strings. Long names allocate during constant
    // evaluation, so a config array cannot be retained in static constexpr
    // storage or exposed by reference. Keep each array local to its consteval
    // query and return only allocation-free facts derived from it.
    template<typename Node, fixed_string Name>
    consteval size_t static_input_declaration_index()
    {
        auto const configs = Node::inputs();
        size_t found = static_cast<size_t>(-1);
        for (size_t index = 0; index < configs.size(); ++index) {
            if (configs[index].name != Name.view()) continue;
            if (found != static_cast<size_t>(-1)) {
                throw "duplicate static input port name";
            }
            found = index;
        }
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
        for (size_t index = 0; index < configs.size(); ++index) {
            if (configs[index].name != Name.view()) continue;
            if (found != static_cast<size_t>(-1)) {
                throw "duplicate static output port name";
            }
            found = index;
        }
        if (found == static_cast<size_t>(-1)) {
            throw "unknown static output port name";
        }
        return found;
    }

    template<typename Node, size_t Index>
    consteval PortKind static_input_port_kind_at()
    {
        static_assert(Index < Node::inputs().size(),
            "static input port index is out of bounds");
        auto const configs = Node::inputs();
        return is_sample(configs[Index])
            ? PortKind::sample : PortKind::event;
    }

    template<typename Node, size_t Index>
    consteval PortKind static_output_port_kind_at()
    {
        static_assert(Index < Node::outputs().size(),
            "static output port index is out of bounds");
        auto const configs = Node::outputs();
        return is_sample(configs[Index])
            ? PortKind::sample : PortKind::event;
    }

    template<typename Node, size_t Index>
    consteval size_t static_sample_input_port_index_at()
    {
        static_assert(Index < Node::inputs().size(),
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::sample,
            "static input port is not a sample port");
        auto const configs = Node::inputs();
        size_t result = 0;
        for (size_t index = 0; index < Index; ++index) {
            if (is_sample(configs[index])) ++result;
        }
        return result;
    }

    template<typename Node, size_t Index>
    consteval size_t static_event_input_port_index_at()
    {
        static_assert(Index < Node::inputs().size(),
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::event,
            "static input port is not an event port");
        auto const configs = Node::inputs();
        size_t result = 0;
        for (size_t index = 0; index < Index; ++index) {
            if (!is_sample(configs[index])) ++result;
        }
        return result;
    }

    template<typename Node, size_t Index>
    consteval size_t static_realtime_sample_output_port_index_at()
    {
        static_assert(Index < Node::outputs().size(),
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::sample,
            "static output port is not a sample port");
        static_assert(!is_tock(Node::outputs()[Index]),
            "static output port is not produced by Tick execution");
        auto const configs = Node::outputs();
        size_t result = 0;
        for (size_t index = 0; index < Index; ++index) {
            if (is_sample(configs[index]) && is_tick(configs[index].production)) {
                ++result;
            }
        }
        return result;
    }

    template<typename Node, size_t Index>
    consteval size_t static_realtime_event_output_port_index_at()
    {
        static_assert(Index < Node::outputs().size(),
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::event,
            "static output port is not an event port");
        static_assert(!is_tock(Node::outputs()[Index]),
            "static output port is not produced by Tick execution");
        auto const configs = Node::outputs();
        size_t result = 0;
        for (size_t index = 0; index < Index; ++index) {
            if (!is_sample(configs[index]) && is_tick(configs[index].production)) {
                ++result;
            }
        }
        return result;
    }

    template<typename Node, size_t Index>
    consteval ChannelLayout static_input_port_layout_at()
    {
        static_assert(Index < Node::inputs().size(),
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::sample,
            "static input port is not a sample port");
        auto const configs = Node::inputs();
        return effective_channel_layout(sample_properties(configs[Index]));
    }

    template<typename Node, size_t Index>
    consteval ChannelLayout static_declared_output_port_layout_at()
    {
        static_assert(Index < Node::outputs().size(),
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::sample,
            "static output port is not a sample port");
        auto const configs = Node::outputs();
        return effective_channel_layout(sample_properties(configs[Index]));
    }

    template<typename Node, size_t Index>
    consteval bool static_input_port_is_random_access_at()
    {
        static_assert(Index < Node::inputs().size(),
            "static input port index is out of bounds");
        auto const configs = Node::inputs();
        return is_random_access(configs[Index]);
    }

    template<typename Node, size_t Index>
    consteval bool static_output_port_is_tock_at()
    {
        static_assert(Index < Node::outputs().size(),
            "static output port index is out of bounds");
        auto const configs = Node::outputs();
        return is_tock(configs[Index]);
    }

    template<typename Node, size_t Index>
    consteval bool static_output_port_is_recording_at()
    {
        static_assert(Index < Node::outputs().size(),
            "static output port index is out of bounds");
        auto const configs = Node::outputs();
        auto const& config = configs[Index];
        return is_tick(config.production) && is_persisted(config.retention);
    }

    template<typename Node>
    consteval size_t static_recording_sample_output_count()
    {
        if constexpr (requires { Node::outputs(); }) {
            size_t result = 0;
            for (auto const& config : Node::outputs()) {
                if (is_sample(config) && is_tick(config.production)
                    && is_persisted(config.retention)) {
                    ++result;
                }
            }
            return result;
        } else {
            return 0;
        }
    }

    template<typename Node>
    consteval size_t static_recording_event_output_count()
    {
        if constexpr (requires { Node::outputs(); }) {
            size_t result = 0;
            for (auto const& config : Node::outputs()) {
                if (!is_sample(config) && is_tick(config.production)
                    && is_persisted(config.retention)) {
                    ++result;
                }
            }
            return result;
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
        static_assert(Index < Node::outputs().size(),
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::sample
                && static_output_port_is_recording_at<Node, Index>(),
            "static output port is not a Tick/persisted sample port");
        auto const configs = Node::outputs();
        size_t result = 0;
        for (size_t index = 0; index < Index; ++index) {
            auto const& config = configs[index];
            if (is_sample(config) && is_tick(config.production)
                && is_persisted(config.retention)) {
                ++result;
            }
        }
        return result;
    }

    template<typename Node, size_t Index>
    consteval size_t static_recording_event_output_index_at()
    {
        static_assert(Index < Node::outputs().size(),
            "static output port index is out of bounds");
        static_assert(static_output_port_kind_at<Node, Index>() == PortKind::event
                && static_output_port_is_recording_at<Node, Index>(),
            "static output port is not a Tick/persisted event port");
        auto const configs = Node::outputs();
        size_t result = 0;
        for (size_t index = 0; index < Index; ++index) {
            auto const& config = configs[index];
            if (!is_sample(config) && is_tick(config.production)
                && is_persisted(config.retention)) {
                ++result;
            }
        }
        return result;
    }

    template<typename Node, size_t Index>
    consteval size_t static_random_access_input_port_index_at()
    {
        static_assert(Index < Node::inputs().size(),
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::sample
                && static_input_port_is_random_access_at<Node, Index>(),
            "static input port is not a Random Access sample port");
        auto const configs = Node::inputs();
        size_t result = 0;
        for (size_t index = 0; index < Index; ++index) {
            if (is_sample(configs[index]) && is_random_access(configs[index])) {
                ++result;
            }
        }
        return result;
    }

    template<typename Node, size_t Index>
    consteval size_t static_random_access_event_input_port_index_at()
    {
        static_assert(Index < Node::inputs().size(),
            "static input port index is out of bounds");
        static_assert(static_input_port_kind_at<Node, Index>() == PortKind::event
                && static_input_port_is_random_access_at<Node, Index>(),
            "static input port is not a Random Access event port");
        auto const configs = Node::inputs();
        size_t result = 0;
        for (size_t index = 0; index < Index; ++index) {
            if (!is_sample(configs[index]) && is_random_access(configs[index])) {
                ++result;
            }
        }
        return result;
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
        return count_sample_ports(Node::outputs());
    }

    template<typename Node>
    inline constexpr size_t static_output_count_v = static_output_count<Node>();

    template<typename Node, fixed_string Name>
    consteval size_t static_input_port_index()
    {
        auto const configs = Node::inputs();
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
        auto const configs = Node::inputs();
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
        auto const configs = Node::outputs();
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
        auto const configs = Node::outputs();
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
    consteval bool static_input_port_is_random_access()
    {
        return static_input_port_is_random_access_at<
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
        for (InputConfig const& config : configs) {
            if (!is_sample(config)) continue;
            if (sample_index == port_index) return background_index;
            if (is_random_access(config)) ++background_index;
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
    consteval bool static_output_port_is_tock()
    {
        return static_output_port_is_tock_at<
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
        for (OutputConfig const& config : configs) {
            if (!is_sample(config)) continue;
            if (sample_index == port_index) return realtime_index;
            if (is_tick(config.production)) ++realtime_index;
            ++sample_index;
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
        for (OutputConfig const& config : configs) {
            if (!is_sample(config)) continue;
            if (sample_index == port_index) return tock_index;
            if (is_tock(config)) ++tock_index;
            ++sample_index;
        }
        throw "unknown static sample output port name";
    }

    template<typename Node, fixed_string Name>
    consteval size_t static_event_input_port_index()
    {
        auto const configs = Node::inputs();
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
        auto const configs = Node::outputs();
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
        for (OutputConfig const& config : configs) {
            if (is_sample(config)) continue;
            if (event_index == port_index) return realtime_index;
            if (is_tick(config.production)) ++realtime_index;
            ++event_index;
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
        for (InputConfig const& config : configs) {
            if (is_sample(config)) continue;
            if (event_index == port_index) return background_index;
            if (is_random_access(config)) ++background_index;
            ++event_index;
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
        for (OutputConfig const& config : configs) {
            if (is_sample(config)) continue;
            if (event_index == port_index) return tock_index;
            if (is_tock(config)) ++tock_index;
            ++event_index;
        }
        throw "unknown static event output port name";
    }

    template<typename Node, size_t Index>
    consteval ChannelLayout static_output_port_layout_at()
    {
        auto const configs = Node::outputs();
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
}
