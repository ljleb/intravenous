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
            return std::array<SequentialInputConfig<Sample>, NumInputs>{};
        }

        static constexpr auto outputs()
        {
            return std::array<SequentialOutputConfig<Sample>, 1>{};
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
            return std::array {
                input<Sample>({"a", "A"}),
                input<Sample>({"b", "B"})
            };
        }

        static constexpr auto outputs()
        {
            return std::array {
                output<Sample>({"out", "Out"})
            };
        }

        void tick(TickSampleContext<BinaryOpNode<BinaryOp>> const& ctx) const
        {
            auto out = ctx.template output<"out">();
            auto left = ctx.template input<"a">();
            auto right = ctx.template input<"b">();
            out.push(BinaryOp{}(left.get(), right.get()));
        }
    };

    template<class ChannelType, SampleStreamLayout Layout, size_t NumInputs>
    class SumN {
    public:
        static_assert(NumInputs >= 1, "SumN requires at least one input");

        static constexpr auto inputs()
        {
            auto configs = std::array<SequentialInputConfig<Sample>, NumInputs>{};
            for (auto& config : configs) {
                config = input<Sample>(iv::PortNames::generated(), InputAccess::sequential, {
                    .channel_layout = {
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = Layout,
                    },
                });
            }
            return configs;
        }

        static constexpr auto outputs()
        {
            return std::array {
                output<Sample>({"out", "Out"}, OutputProduction::sequential, {
                    .channel_layout = ChannelLayout {
                        .channel_type = ChannelTypeTraits<ChannelType>::id,
                        .sample_layout = Layout,
                    },
                }),
            };
        }

        void tick(TickSampleContext<SumN> const& ctx) const
        {
            for (size_t channel = 0; channel < ChannelType::channel_count; ++channel) {
                Sample result = 0.0f;
                [&]<size_t... Index>(std::index_sequence<Index...>) {
                    ((result += ctx.template input<Index>().get(
                        0, channel)), ...);
                }(std::make_index_sequence<NumInputs>{});
                ctx.template output<0>()(channel) = result;
            }
        }
    };

    // A registered IV_NODE may name a fully instantiated node template. The
    // registry binds its stable ID to this one specialization, not to the
    // BinaryOpNode template family as a whole.
    using Sum = BinaryOpNode<std::plus<Sample>>;
    using Subtraction = BinaryOpNode<std::minus<Sample>>;
    using Product = BinaryOpNode<std::multiplies<Sample>>;
    using Quotient = BinaryOpNode<std::divides<Sample>>;

    template<size_t NumInputs>
    using ProductN = FixedBinaryOpNode<std::multiplies<Sample>, NumInputs>;

    struct Invert {
        static constexpr auto inputs()
        {
            return std::array<SequentialInputConfig<Sample>, 1>{};
        }

        static constexpr auto outputs()
        {
            return std::array<SequentialOutputConfig<Sample>, 1>{};
        }

        void tick(auto const& state) const
        {
            state.template output<0>().push(-state.template input<0>().get());
        }
    };

    struct Power {
        static constexpr auto inputs()
        {
            return std::array<SequentialInputConfig<Sample>, 2>{};
        }

        static constexpr auto outputs()
        {
            return std::array<SequentialOutputConfig<Sample>, 1>{};
        }

        void tick(auto const& ctx) const
        {
            ctx.template output<0>().push(std::pow(ctx.template input<0>().get(), ctx.template input<1>().get()));
        }
    };
}
