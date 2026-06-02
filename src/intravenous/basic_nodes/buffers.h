#pragma once

#include "runtime/lane_ref.h"
#include "node/lifecycle.h"

#include <array>
#include <cassert>
#include <optional>
#include <span>
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

    class LaneInputValue {
        RealtimeLaneRef _lane;
        std::string _identity;

    public:
        static std::string nominal_identity(
            RealtimeLaneRef const& lane
        )
        {
            return lane.nominal_identity();
        }

        explicit LaneInputValue(
            RealtimeLaneRef lane
        ) :
            _lane(std::move(lane)),
            _identity(nominal_identity(lane))
        {}

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .name = "value" } };
        }

        std::string identity() const
        {
            return _identity;
        }

        void tick_block(TickBlockContext<LaneInputValue> const& ctx) const
        {
            auto block = _lane.pull_sample_block(ctx.index, ctx.block_size);
            ctx.outputs[0].push_block(block.samples());
        }
    };
}
