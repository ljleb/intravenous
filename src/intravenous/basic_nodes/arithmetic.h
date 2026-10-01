#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <cmath>
#include <functional>
#include <utility>
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
    public:
        static_assert(NumInputs >= 1, "FixedBinaryOpNode requires at least one input");

        static constexpr auto inputs()
        {
            return std::array<InputConfig, NumInputs>{};
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, 1>{};
        }

        static constexpr size_t num_inputs()
        {
            return NumInputs;
        }

        void tick(auto const& ctx) const
        {
            Sample result = binary_op_default_v<BinaryOp>;
            [&]<size_t... Index>(std::index_sequence<Index...>) {
                ((result = BinaryOp{}(
                    result, ctx.template input<Index>().get())), ...);
            }(std::make_index_sequence<NumInputs>{});
            ctx.template output<0>().push(result);
        }
    };

    template<typename BinaryOp>
    class BinaryOpNode {
    public:
        static constexpr auto inputs()
        {
            return std::array<InputConfig, 2>{};
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(auto const& ctx) const
        {
            auto out = ctx.template output<0>();
            auto in0 = ctx.template input<0>();
            auto in1 = ctx.template input<1>();
            out.push(BinaryOp{}(in0.get(), in1.get()));
        }
    };

    template<class ChannelType, SampleStreamLayout Layout, size_t NumInputs>
    class Sum {
    public:
        static_assert(NumInputs >= 1, "Sum requires at least one input");

        static constexpr auto inputs()
        {
            auto configs = std::array<InputConfig, NumInputs>{};
            for (auto& config : configs) {
                config.kind = SampleInputProperties{
                    .channel_layout = ChannelLayout{
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = Layout,
                    },
                };
            }
            return configs;
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, 1>{tick_sample_output("out", {
                .channel_layout = ChannelLayout{
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = Layout,
                },
            })};
        }

        void tick(TickSampleContext<Sum> const& ctx) const
        {
            for (size_t channel = 0; channel < ChannelType::channel_count; ++channel) {
                Sample result = 0.0f;
                [&]<size_t... Index>(std::index_sequence<Index...>) {
                    ((result += ctx.template input<Index>().get(
                        0, channel)), ...);
                }(std::make_index_sequence<NumInputs>{});
                ctx.template output<0>().write_frame(0, channel, result);
            }
        }
    };

    // A registered IV_NODE may name a fully instantiated node template. The
    // registry binds its stable ID to this one specialization, not to the
    // BinaryOpNode template family as a whole.
    using Subtract = BinaryOpNode<std::minus<Sample>>;

    template<size_t NumInputs>
    using Product = FixedBinaryOpNode<std::multiplies<Sample>, NumInputs>;

    using Quotient = BinaryOpNode<std::divides<Sample>>;

    struct Invert {
        static constexpr auto inputs()
        {
            return std::array<InputConfig, 1>{};
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(auto const& state) const
        {
            state.template output<0>().push(-state.template input<0>().get());
        }
    };

    struct Power {
        static constexpr auto inputs()
        {
            return std::array<InputConfig, 2>{};
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(auto const& ctx) const
        {
            ctx.template output<0>().push(std::pow(ctx.template input<0>().get(), ctx.template input<1>().get()));
        }
    };
}
