#pragma once

#include "node_lifecycle.h"

#include <array>
#include <functional>
#include <vector>

namespace iv {
    template<typename BinaryOp>
    constexpr Sample binary_op_default_v = 0.0;

    template<typename T>
    constexpr T binary_op_default_v<std::multiplies<T>> = T(1.0);

    template<typename T>
    constexpr T binary_op_default_v<std::divides<T>> = T(1.0);

    template<typename BinaryOp>
    class BinaryOpNode {
        BinaryOp _binary_op;
        size_t _num_inputs;

    public:
        explicit BinaryOpNode(size_t num_inputs = 2) :
            _num_inputs(num_inputs)
        {}

        auto inputs() const
        {
            return std::vector<InputConfig>(_num_inputs);
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        auto num_inputs() const
        {
            return _num_inputs;
        }

        void tick(auto const& ctx) const
        {
            auto& out = ctx.outputs[0];
            Sample result = binary_op_default_v<BinaryOp>;
            for (auto& input : ctx.inputs) {
                result = _binary_op(result, input.get());
            }
            out.push(result);
        }
    };

    using Sum = BinaryOpNode<std::plus<Sample>>;
    using Subtract = BinaryOpNode<std::minus<Sample>>;
    using Product = BinaryOpNode<std::multiplies<Sample>>;
    using Quotient = BinaryOpNode<std::divides<Sample>>;

    struct Invert {
        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(auto const& state) const
        {
            state.outputs[0].push(-state.inputs[0].get());
        }
    };

    struct Power {
        auto inputs() const
        {
            return std::array<InputConfig, 2>{};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(auto const& ctx) const
        {
            ctx.outputs[0].push(std::powf(ctx.inputs[0].get(), ctx.inputs[1].get()));
        }
    };
}
