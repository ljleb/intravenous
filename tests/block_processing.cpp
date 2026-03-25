#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"
#include "devices/channel_buffer_sink.h"
#include "graph_node.h"
#include "module_test_utils.h"
#include "runtime/system.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
    struct BlockOnlyCounter {
        struct State {
            iv::Sample value = 0.0f;
        };

        constexpr auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        template<class Allocator>
        void init_buffer(Allocator& allocator) const
        {
            (void)allocator.template new_object<State>();
        }

        size_t max_block_size() const
        {
            return 16;
        }

        void tick_block(iv::BlockTickState const& state) const
        {
            auto& counter = state.get_state<State>();
            std::array<iv::Sample, 16> block {};
            for (size_t sample = 0; sample < state.block_size; ++sample) {
                block[sample] = counter.value++;
            }
            state.outputs[0].push_block(std::span<iv::Sample const>(block.data(), state.block_size));
        }
    };

    struct BlockPassthrough {
        constexpr auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        size_t max_block_size() const
        {
            return 8;
        }

        void tick_block(iv::BlockTickState const& state) const
        {
            state.outputs[0].push_block(state.inputs[0].get_block(state.block_size));
        }
    };

    iv::NodeProcessor make_feedback_processor(iv::Sample* destination, size_t size)
    {
        iv::GraphBuilder g;

        auto const integrator = g.node<iv::Integrator>();
        auto const warper = g.node<iv::Warper>();
        auto const sink = g.node<iv::BufferSink>(destination, size);

        integrator(warper["aliased"].detach(), 440.0f * 2.0f, 1.0f / 48000.0f);
        warper(integrator + 0.05f);
        sink(warper["anti_aliased"] * 0.2f);
        g.outputs();

        return iv::NodeProcessor(iv::TypeErasedNode(g.build()), {}, size);
    }

    void require_close(
        std::span<iv::Sample const> expected,
        std::span<iv::Sample const> actual,
        iv::Sample epsilon,
        char const* message
    )
    {
        iv::test::require(expected.size() == actual.size(), "sample span sizes must match");
        for (size_t i = 0; i < expected.size(); ++i) {
            if (std::fabs(expected[i] - actual[i]) > epsilon) {
                std::cerr << message << " at sample " << i
                          << ": expected " << expected[i]
                          << ", got " << actual[i] << '\n';
                std::exit(1);
            }
        }
    }
}

int main()
{
    iv::test::install_crash_handlers();

    {
        constexpr size_t sample_count = 64;
        std::array<iv::Sample, sample_count> samplewise {};
        std::array<iv::Sample, sample_count> blockwise {};

        auto sample_processor = make_feedback_processor(samplewise.data(), samplewise.size());
        auto block_processor = make_feedback_processor(blockwise.data(), blockwise.size());

        iv::test::run_processor_ticks(sample_processor, sample_count);

        std::array<size_t, 3> const block_sizes { 32, 16, 16 };
        size_t total_block_samples = 0;
        for (size_t block_size : block_sizes) {
            total_block_samples += block_size;
        }
        iv::test::require(total_block_samples == sample_count, "test block sizes must match sample count");
        iv::test::run_processor_blocks(block_processor, block_sizes);

        require_close(samplewise, blockwise, 1e-6f, "plain graph block execution diverged");
    }

    {
        constexpr size_t sample_count = 16;
        std::array<iv::Sample, sample_count> samplewise {};
        std::array<iv::Sample, sample_count> blockwise {};

        auto make_counter_processor = [](iv::Sample* destination, size_t size) {
            iv::GraphBuilder g;
            auto const counter = g.node<BlockOnlyCounter>();
            auto const sink = g.node<iv::BufferSink>(destination, size);
            sink(counter);
            g.outputs();
            return iv::NodeProcessor(iv::TypeErasedNode(g.build()), {}, size);
        };

        auto sample_processor = make_counter_processor(samplewise.data(), samplewise.size());
        auto block_processor = make_counter_processor(blockwise.data(), blockwise.size());

        iv::test::run_processor_ticks(sample_processor, sample_count);
        std::array<size_t, 2> const counter_blocks { 8, 8 };
        iv::test::run_processor_blocks(block_processor, counter_blocks);

        require_close(samplewise, blockwise, 0.0f, "block-only node fallback diverged");
    }

    {
        constexpr size_t sample_count = 16;
        std::array<iv::Sample, sample_count> expected {};
        std::array<iv::Sample, sample_count> actual {};

        auto make_passthrough_processor = [](iv::Sample* destination, size_t size) {
            iv::GraphBuilder g;
            auto const counter = g.node<BlockOnlyCounter>();
            auto const passthrough = g.node<BlockPassthrough>();
            auto const sink = g.node<iv::BufferSink>(destination, size);
            passthrough(counter);
            sink(passthrough);
            g.outputs();
            return iv::NodeProcessor(iv::TypeErasedNode(g.build()), {}, size);
        };

        auto expected_processor = make_passthrough_processor(expected.data(), expected.size());
        auto actual_processor = make_passthrough_processor(actual.data(), actual.size());

        iv::test::run_processor_ticks(expected_processor, sample_count);
        std::array<size_t, 1> const passthrough_blocks { sample_count };
        iv::test::run_processor_blocks(actual_processor, passthrough_blocks);

        require_close(expected, actual, 0.0f, "block-aware passthrough diverged");
    }

    {
        std::array<iv::Sample, 16> samplewise {};
        std::array<iv::Sample, 16> blockwise {};

        iv::Sample a = 0.25f;
        iv::Sample b = 0.5f;

        auto make_wrapped_processor = [&](iv::System& system) {
            iv::GraphBuilder g;
            auto const src_a = g.node<iv::ValueSource>(&a);
            auto const src_b = g.node<iv::ValueSource>(&b);
            auto const sink_a = g.node<iv::SharedAccumulatingSink>(system.audio_device().sink_id(0));
            auto const sink_b = g.node<iv::SharedAccumulatingSink>(system.audio_device().sink_id(0));
            sink_a(src_a);
            sink_b(src_b);
            g.outputs();

            system.activate_root(true);
            return system.make_processor(iv::TypeErasedNode(g.build()));
        };

        iv::System sample_system(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = samplewise.size(),
            },
            false,
            false
        );
        iv::System block_system(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = blockwise.size(),
            },
            false,
            false
        );

        auto sample_processor = make_wrapped_processor(sample_system);
        auto block_processor = make_wrapped_processor(block_system);

        iv::test::run_processor_ticks(sample_processor, samplewise.size());
        std::array<size_t, 2> const wrapped_blocks { 8, 8 };
        iv::test::run_processor_blocks(block_processor, wrapped_blocks);

        std::copy_n(sample_system.audio_device().output_block(0).begin(), samplewise.size(), samplewise.begin());
        std::copy_n(block_system.audio_device().output_block(0).begin(), blockwise.size(), blockwise.begin());

        require_close(samplewise, blockwise, 0.0f, "wrapped graph block execution diverged");
    }

    {
        constexpr size_t sample_count = 15;
        std::array<iv::Sample, sample_count> samplewise {};
        std::array<iv::Sample, sample_count> blockwise {};

        iv::Sample a = 0.25f;
        iv::Sample b = 0.5f;

        auto make_wrapped_processor = [&](iv::System& system) {
            iv::GraphBuilder g;
            auto const src_a = g.node<iv::ValueSource>(&a);
            auto const src_b = g.node<iv::ValueSource>(&b);
            auto const sink_a = g.node<iv::SharedAccumulatingSink>(system.audio_device().sink_id(0));
            auto const sink_b = g.node<iv::SharedAccumulatingSink>(system.audio_device().sink_id(0));
            sink_a(src_a);
            sink_b(src_b);
            g.outputs();

            system.activate_root(true);
            return system.make_processor(iv::TypeErasedNode(g.build()));
        };

        iv::System sample_system(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = 15,
            },
            false,
            false
        );
        iv::System block_system(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = 15,
            },
            false,
            false
        );

        auto sample_processor = make_wrapped_processor(sample_system);
        auto block_processor = make_wrapped_processor(block_system);

        iv::test::run_processor_ticks(sample_processor, samplewise.size());
        std::array<size_t, 4> const wrapped_blocks { 8, 4, 2, 1 };
        iv::test::run_processor_blocks(block_processor, wrapped_blocks);

        std::copy_n(sample_system.audio_device().output_block(0).begin(), samplewise.size(), samplewise.begin());
        std::copy_n(block_system.audio_device().output_block(0).begin(), blockwise.size(), blockwise.begin());

        require_close(samplewise, blockwise, 0.0f, "wrapped graph mismatched callback execution diverged");
    }

    {
        std::array<iv::Sample, 8> buffer {};
        auto processor = make_feedback_processor(buffer.data(), buffer.size());
        iv::test::expect_failure(
            [&] { processor.tick_block({}, 0, 3); },
            "power of 2",
            "non-power-of-2 block size should fail"
        );
    }

    return 0;
}
