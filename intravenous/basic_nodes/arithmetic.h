#pragma once

#include "node/lifecycle.h"

#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace iv {
    template<typename BinaryOp>
    constexpr Sample binary_op_default_v = 0.0;

    template<typename T>
    constexpr T binary_op_default_v<std::multiplies<T>> = T(1.0);

    template<typename T>
    constexpr T binary_op_default_v<std::divides<T>> = T(1.0);

    template<typename BinaryOp, size_t NumInputs>
    class FixedBinaryOpNode {
        BinaryOp _binary_op;

    public:
        static_assert(NumInputs >= 1, "FixedBinaryOpNode requires at least one input");

        auto inputs() const
        {
            return std::array<InputConfig, NumInputs>{};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        static constexpr size_t num_inputs()
        {
            return NumInputs;
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

    template<size_t NumInputs>
    using Sum = FixedBinaryOpNode<std::plus<Sample>, NumInputs>;

    template<size_t NumInputs>
    using Subtract = FixedBinaryOpNode<std::minus<Sample>, NumInputs>;

    template<size_t NumInputs>
    using Product = FixedBinaryOpNode<std::multiplies<Sample>, NumInputs>;

    template<size_t NumInputs>
    using Quotient = FixedBinaryOpNode<std::divides<Sample>, NumInputs>;

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
            ctx.outputs[0].push(std::pow(ctx.inputs[0].get(), ctx.inputs[1].get()));
        }
    };
}
