#pragma once

#include "node.h"
#include <array>
#include <cassert>

namespace iv {
    class BufferSink {
        Sample* _destination;
        size_t _size;
        size_t _time_offset;

    public:
        explicit BufferSink(Sample* destination, size_t size, size_t time_offset = 0) :
            _destination(destination),
            _size(size),
            _time_offset(time_offset)
        {}

        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            if (state.index >= _time_offset && state.index < _time_offset + _size) {
                _destination[state.index] = state.inputs[0].get();
            }
        }
    };

    struct ValueSource {
        Sample const* _value;

        explicit ValueSource(Sample const* value) :
            _value(value)
        {
            assert(_value);
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .name = "value" } };
        }

        void tick(TickState const& ts) const
        {
            ts.outputs[0].push(*_value);
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
            assert(_size >= 1);
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state) const
        {
            auto& out = state.outputs[0];
            if (state.index < _time_offset) {
                out.push(_source[0]);
            } else if (state.index >= _time_offset + _size) {
                out.push(_source[_size - 1]);
            } else {
                out.push(_source[state.index - _time_offset]);
            }
        }
    };
}
