#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"
#include "devices/channel_buffer_sink.h"
#include "graph_node.h"
#include "module_test_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
    struct BufferSink {
        iv::Sample* destination;
        size_t size;

        auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        void tick_block(iv::TickBlockContext<BufferSink> const& ctx) const
        {
            auto block = ctx.inputs[0].get_block(ctx.block_size);
            for (size_t i = 0; i < ctx.block_size; ++i) {
                size_t const index = ctx.index + i;
                if (index < size) {
                    destination[index] = block[i];
                }
            }
        }
    };

    struct BlockOnlyCounter {
        struct State {
            iv::Sample value = 0.0f;
        };

        auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        size_t max_block_size() const
        {
            return 16;
        }

        void tick_block(iv::TickBlockContext<BlockOnlyCounter> const& ctx) const
        {
            auto& counter = ctx.state();
            std::array<iv::Sample, 16> block {};
            for (size_t sample = 0; sample < ctx.block_size; ++sample) {
                block[sample] = counter.value ++;
            }
            ctx.outputs[0].push_block(std::span<iv::Sample const>(block.data(), ctx.block_size));
        }
    };

    struct BlockPassthrough {
        auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        size_t max_block_size() const
        {
            return 8;
        }

        void tick_block(iv::TickBlockContext<BlockPassthrough> const& ctx) const
        {
            ctx.outputs[0].push_block(ctx.inputs[0].get_block(ctx.block_size));
        }
    };

    iv::NodeExecutor make_feedback_processor(
        iv::ExecutionTargetRegistry& execution_target_registry,
        size_t executor_id,
        iv::Sample* destination,
        size_t size
    )
    {
        iv::GraphBuilder g;

        auto const integrator = g.node<iv::Integrator>();
        auto const warper = g.node<iv::Warper>();
        auto const sink = g.node<BufferSink>(destination, size);

        integrator(warper["aliased"].detach(), 440.0f * 2.0f, 1.0f / 48000.0f);
        warper(integrator + 0.05f);
        sink(warper["anti_aliased"] * 0.2f);
        g.outputs();

        return iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            {},
            execution_target_registry,
            executor_id
        );
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

    template<typename Executor>
    void render_requested_blocks(
        iv::test::FakeAudioDevice& audio_device,
        Executor& executor,
        std::span<size_t const> block_sizes,
        std::span<iv::Sample> destination,
        size_t channel = 0
    )
    {
        size_t index = 0;
        for (size_t block_size : block_sizes) {
            audio_device.device().begin_requested_block(index, block_size);
            executor.tick_block(index, block_size);
            iv::test::require(audio_device.device().wait_until_block_ready(), "requested device block should become ready");

            auto block = audio_device.output_block(channel);
            std::copy_n(block.begin(), block_size, destination.subspan(index).begin());

            audio_device.device().finish_requested_block();
            index += block_size;
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

        iv::test::FakeAudioDevice sample_device({ .max_block_frames = sample_count });
        iv::test::FakeAudioDevice block_device({ .max_block_frames = sample_count });
        iv::ExecutionTargetRegistry sample_targets(iv::test::make_audio_device_provider(sample_device));
        iv::ExecutionTargetRegistry block_targets(iv::test::make_audio_device_provider(block_device));
        auto sample_processor = make_feedback_processor(sample_targets, 1, samplewise.data(), samplewise.size());
        auto block_processor = make_feedback_processor(block_targets, 2, blockwise.data(), blockwise.size());

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

        auto make_counter_processor = [](iv::ExecutionTargetRegistry& execution_target_registry, size_t executor_id, iv::Sample* destination, size_t size) {
            iv::GraphBuilder g;
            auto const counter = g.node<BlockOnlyCounter>();
            auto const sink = g.node<BufferSink>(destination, size);
            sink(counter);
            g.outputs();
            return iv::NodeExecutor::create(
                iv::TypeErasedNode(g.build()),
                {},
                execution_target_registry,
                executor_id
            );
        };

        iv::test::FakeAudioDevice sample_device({ .max_block_frames = sample_count });
        iv::test::FakeAudioDevice block_device({ .max_block_frames = sample_count });
        iv::ExecutionTargetRegistry sample_targets(iv::test::make_audio_device_provider(sample_device));
        iv::ExecutionTargetRegistry block_targets(iv::test::make_audio_device_provider(block_device));
        auto sample_processor = make_counter_processor(sample_targets, 1, samplewise.data(), samplewise.size());
        auto block_processor = make_counter_processor(block_targets, 2, blockwise.data(), blockwise.size());

        iv::test::run_processor_ticks(sample_processor, sample_count);
        std::array<size_t, 2> const counter_blocks { 8, 8 };
        iv::test::run_processor_blocks(block_processor, counter_blocks);

        require_close(samplewise, blockwise, 0.0f, "block-only node fallback diverged");
    }

    {
        constexpr size_t sample_count = 16;
        std::array<iv::Sample, sample_count> expected {};
        std::array<iv::Sample, sample_count> actual {};

        auto make_passthrough_processor = [](iv::ExecutionTargetRegistry& execution_target_registry, size_t executor_id, iv::Sample* destination, size_t size) {
            iv::GraphBuilder g;
            auto const counter = g.node<BlockOnlyCounter>();
            auto const passthrough = g.node<BlockPassthrough>();
            auto const sink = g.node<BufferSink>(destination, size);
            passthrough(counter);
            sink(passthrough);
            g.outputs();
            return iv::NodeExecutor::create(
                iv::TypeErasedNode(g.build()),
                {},
                execution_target_registry,
                executor_id
            );
        };

        iv::test::FakeAudioDevice expected_device({ .max_block_frames = sample_count });
        iv::test::FakeAudioDevice actual_device({ .max_block_frames = sample_count });
        iv::ExecutionTargetRegistry expected_targets(iv::test::make_audio_device_provider(expected_device));
        iv::ExecutionTargetRegistry actual_targets(iv::test::make_audio_device_provider(actual_device));
        auto expected_processor = make_passthrough_processor(expected_targets, 1, expected.data(), expected.size());
        auto actual_processor = make_passthrough_processor(actual_targets, 2, actual.data(), actual.size());

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

        auto make_wrapped_processor = [&](iv::ExecutionTargetRegistry& execution_target_registry, size_t executor_id) {
            iv::GraphBuilder g;
            auto const src_a = g.node<iv::ValueSource>(&a);
            auto const src_b = g.node<iv::ValueSource>(&b);
            auto const sink_a = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
                .device_id = 0,
                .channel = 0,
            });
            auto const sink_b = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
                .device_id = 0,
                .channel = 0,
            });
            sink_a(src_a);
            sink_b(src_b);
            g.outputs();

            return iv::NodeExecutor::create(
                iv::TypeErasedNode(g.build()),
                {},
                execution_target_registry,
                executor_id
            );
        };

        iv::test::FakeAudioDevice sample_device(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = samplewise.size(),
            }
        );
        iv::test::FakeAudioDevice block_device(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = blockwise.size(),
            }
        );

        iv::ExecutionTargetRegistry sample_targets(iv::test::make_audio_device_provider(sample_device));
        iv::ExecutionTargetRegistry block_targets(iv::test::make_audio_device_provider(block_device));
        auto sample_processor = make_wrapped_processor(sample_targets, 1);
        auto block_processor = make_wrapped_processor(block_targets, 2);

        std::array<size_t, 16> const sample_blocks {
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
        };
        render_requested_blocks(sample_device, sample_processor, sample_blocks, std::span<iv::Sample>(samplewise));
        std::array<size_t, 2> const wrapped_blocks { 8, 8 };
        render_requested_blocks(block_device, block_processor, wrapped_blocks, std::span<iv::Sample>(blockwise));

        require_close(samplewise, blockwise, 0.0f, "wrapped graph block execution diverged");
    }

    {
        constexpr size_t sample_count = 15;
        std::array<iv::Sample, sample_count> samplewise {};
        std::array<iv::Sample, sample_count> blockwise {};

        iv::Sample a = 0.25f;
        iv::Sample b = 0.5f;

        auto make_wrapped_processor = [&](iv::ExecutionTargetRegistry& execution_target_registry, size_t executor_id) {
            iv::GraphBuilder g;
            auto const src_a = g.node<iv::ValueSource>(&a);
            auto const src_b = g.node<iv::ValueSource>(&b);
            auto const sink_a = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
                .device_id = 0,
                .channel = 0,
            });
            auto const sink_b = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
                .device_id = 0,
                .channel = 0,
            });
            sink_a(src_a);
            sink_b(src_b);
            g.outputs();

            return iv::NodeExecutor::create(
                iv::TypeErasedNode(g.build()),
                {},
                execution_target_registry,
                executor_id
            );
        };

        iv::test::FakeAudioDevice sample_device(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = 15,
            }
        );
        iv::test::FakeAudioDevice block_device(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = 15,
            }
        );

        iv::ExecutionTargetRegistry sample_targets(iv::test::make_audio_device_provider(sample_device));
        iv::ExecutionTargetRegistry block_targets(iv::test::make_audio_device_provider(block_device));
        auto sample_processor = make_wrapped_processor(sample_targets, 1);
        auto block_processor = make_wrapped_processor(block_targets, 2);

        std::vector<size_t> sample_blocks(samplewise.size(), 1);
        render_requested_blocks(sample_device, sample_processor, sample_blocks, std::span<iv::Sample>(samplewise));
        std::array<size_t, 4> const wrapped_blocks { 8, 4, 2, 1 };
        render_requested_blocks(block_device, block_processor, wrapped_blocks, std::span<iv::Sample>(blockwise));

        require_close(samplewise, blockwise, 0.0f, "wrapped graph mismatched callback execution diverged");
    }

    {
        std::array<iv::Sample, 8> buffer {};
        iv::test::FakeAudioDevice audio_device({ .max_block_frames = buffer.size() });
        iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
        auto processor = make_feedback_processor(execution_targets, 1, buffer.data(), buffer.size());
        iv::test::expect_failure(
            [&] { processor.tick_block(0, 3); },
            "power of 2",
            "non-power-of-2 block size should fail"
        );
    }

    return 0;
}
