#pragma once

#include "runtime/timeline.h"
#include "node/lifecycle.h"

#include <array>
#include <cassert>
#include <optional>
#include <string>
#include <utility>

namespace iv {
    struct ValueSource {
        Sample const* _value;

        explicit ValueSource(Sample const* value) :
            _value(value)
        {
            IV_ASSERT(_value, "ValueSource requires a non-null value pointer");
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .name = "value" } };
        }

        void tick(auto const& ctx) const
        {
            ctx.outputs[0].push(*_value);
        }
    };

    class BufferSource {
        Sample* _source;
        size_t _size;
        size_t _time_offset;

    public:
        explicit BufferSource(Sample* source, size_t size, size_t time_offset = 0) :
            _source(source),
            _size(size),
            _time_offset(time_offset)
        {
            IV_ASSERT(_size >= 1, "BufferSource requires at least one sample");
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickSampleContext<BufferSource> const& ctx) const
        {
            auto& out = ctx.outputs[0];
            if (ctx.index < _time_offset) {
                out.push(_source[0]);
            } else if (ctx.index >= _time_offset + _size) {
                out.push(_source[_size - 1]);
            } else {
                out.push(_source[ctx.index - _time_offset]);
            }
        }
    };

    class TimelineInputValue {
        Timeline* _timeline;
        std::string _logical_node_id;
        std::optional<size_t> _member_ordinal;
        size_t _input_ordinal;
        Sample _default_value;

    public:
        struct State {
            Sample staging_value {};
            Sample buffered_value {};
        };

        static std::string nominal_identity(
            std::string_view logical_node_id,
            std::optional<size_t> member_ordinal,
            size_t input_ordinal
        )
        {
            std::string identity = "timeline-input:" + std::string(logical_node_id);
            if (member_ordinal.has_value()) {
                identity += ":member:" + std::to_string(*member_ordinal);
            }
            identity += ":" + std::to_string(input_ordinal);
            return identity;
        }

        TimelineInputValue(
            Timeline& timeline,
            std::string logical_node_id,
            std::optional<size_t> member_ordinal,
            size_t input_ordinal,
            Sample default_value
        ) :
            _timeline(&timeline),
            _logical_node_id(std::move(logical_node_id)),
            _member_ordinal(member_ordinal),
            _input_ordinal(input_ordinal),
            _default_value(default_value)
        {}

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .name = "value" } };
        }

        std::string identity() const
        {
            return nominal_identity(_logical_node_id, _member_ordinal, _input_ordinal);
        }

        void initialize(InitializationContext<TimelineInputValue> const& ctx) const
        {
            auto& state = ctx.state();
            state.staging_value = _default_value;
            state.buffered_value = _default_value;
            _timeline->register_live_input_value(
                _logical_node_id,
                _member_ordinal,
                _input_ordinal,
                &state.staging_value
            );
            state.buffered_value = state.staging_value;
        }

        void move(MoveContext<TimelineInputValue> const& ctx) const
        {
            auto& state = ctx.state();
            auto& previous_state = ctx.previous_state();
            _timeline->move_live_input_value(
                _logical_node_id,
                _member_ordinal,
                _input_ordinal,
                &previous_state.staging_value,
                &state.staging_value
            );
        }

        void release(ReleaseContext<TimelineInputValue> const& ctx) const
        {
            auto& state = ctx.state();
            _timeline->unregister_live_input_value(
                _logical_node_id,
                _member_ordinal,
                _input_ordinal,
                &state.staging_value
            );
        }

        void tick_block(TickBlockContext<TimelineInputValue> const& ctx) const
        {
            auto& state = ctx.state();
            state.buffered_value = state.staging_value;
            for (size_t i = 0; i < ctx.block_size; ++i) {
                ctx.outputs[0].push(state.buffered_value);
            }
        }
    };
}
