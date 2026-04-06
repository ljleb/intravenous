#include "basic_nodes/buffers.h"
#include "basic_nodes/midi.h"
#include "basic_nodes/shaping.h"
#include "devices/channel_buffer_sink.h"
#include "graph_node.h"
#include "module_test_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
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

    struct TriggerPassthrough {
        auto event_inputs() const
        {
            return std::array<iv::EventInputConfig, 1> {{
                { .name = "in", .type = iv::EventTypeId::trigger }
            }};
        }

        auto event_outputs() const
        {
            return std::array<iv::EventOutputConfig, 1> {{
                { .name = "out", .type = iv::EventTypeId::trigger }
            }};
        }

        void tick_block(iv::TickBlockContext<TriggerPassthrough> const& ctx) const
        {
            ctx.event_inputs[0].for_each_in_block(ctx.event_stream_storage(), ctx.index, ctx.block_size, [&](iv::TimedEvent const& event, size_t) {
                ctx.event_outputs[0].push(ctx.event_stream_storage(), event.value, event.time, ctx.index, ctx.block_size);
            });
        }
    };

    struct TriggerSource {
        auto event_outputs() const
        {
            return std::array<iv::EventOutputConfig, 1> {{
                { .name = "out", .type = iv::EventTypeId::trigger }
            }};
        }

        void tick_block(iv::TickBlockContext<TriggerSource> const&) const
        {}
    };

    struct DormantZeroNode {
        int* tick_blocks = nullptr;
        int* skip_blocks = nullptr;

        auto inputs() const
        {
            return std::array<iv::InputConfig, 1> {{
                { .name = "in" }
            }};
        }

        auto outputs() const
        {
            return std::array<iv::OutputConfig, 1> {{
                { .name = "out" }
            }};
        }

        size_t ttl_samples() const
        {
            return 0;
        }

        void tick_block(iv::TickBlockContext<DormantZeroNode> const& ctx) const
        {
            if (tick_blocks) {
                ++*tick_blocks;
            }
            ctx.outputs[0].push_silence(ctx.block_size);
        }

        void skip_block(iv::SkipBlockContext<DormantZeroNode> const& ctx) const
        {
            if (skip_blocks) {
                ++*skip_blocks;
            }
            ctx.outputs[0].push_silence(ctx.block_size);
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

    iv::MidiEvent make_note_on(std::uint8_t note, std::uint8_t velocity = 100, std::uint8_t channel = 0)
    {
        return iv::MidiEvent { .bytes = { static_cast<std::uint8_t>(0x90 | (channel & 0x0F)), note, velocity }, .size = 3 };
    }

    iv::MidiEvent make_note_off(std::uint8_t note, std::uint8_t channel = 0)
    {
        return iv::MidiEvent { .bytes = { static_cast<std::uint8_t>(0x80 | (channel & 0x0F)), note, 0 }, .size = 3 };
    }

    iv::MidiEvent make_pitch_bend(std::uint16_t value, std::uint8_t channel = 0)
    {
        return iv::MidiEvent {
            .bytes = {
                static_cast<std::uint8_t>(0xE0 | (channel & 0x0F)),
                static_cast<std::uint8_t>(value & 0x7F),
                static_cast<std::uint8_t>((value >> 7) & 0x7F)
            },
            .size = 3
        };
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
        std::cerr << "dormancy-test: node-enters-skip-block-when-silent-and-unchanged\n";

        constexpr size_t sample_count = 16;
        std::array<iv::Sample, sample_count> output {};
        std::array<iv::Sample, sample_count> silence {};
        int tick_blocks = 0;
        int skip_blocks = 0;

        auto make_processor = [&](iv::ExecutionTargetRegistry& execution_target_registry, size_t executor_id) {
            iv::GraphBuilder g;
            auto const input = g.node<iv::Constant>(0.0f);
            auto const dormant = g.node<DormantZeroNode>(&tick_blocks, &skip_blocks);
            auto const sink = g.node<BufferSink>(output.data(), output.size());
            dormant(input);
            sink(dormant);
            g.outputs();
            return iv::NodeExecutor::create(
                iv::TypeErasedNode(g.build()),
                {},
                execution_target_registry,
                executor_id
            );
        };

        iv::test::FakeAudioDevice device({ .max_block_frames = sample_count });
        iv::ExecutionTargetRegistry targets(iv::test::make_audio_device_provider(device));
        auto processor = make_processor(targets, 1);
        std::array<size_t, 2> const blocks { 8, 8 };
        iv::test::run_processor_blocks(processor, blocks);

        iv::test::require(tick_blocks == 1, "DormantZeroNode should tick only for the first silent block");
        iv::test::require(skip_blocks == 1, "DormantZeroNode should skip the second unchanged silent block");
        require_close(silence, output, 0.0f, "DormantZeroNode output should remain silent");
    }

    {
        std::cerr << "dormancy-test: node-ref-no-ttl-overrides-node-default\n";

        constexpr size_t sample_count = 16;
        std::array<iv::Sample, sample_count> output {};
        std::array<iv::Sample, sample_count> silence {};
        int tick_blocks = 0;
        int skip_blocks = 0;

        auto make_processor = [&](iv::ExecutionTargetRegistry& execution_target_registry, size_t executor_id) {
            iv::GraphBuilder g;
            auto const input = g.node<iv::Constant>(0.0f);
            auto const dormant = g.node<DormantZeroNode>(&tick_blocks, &skip_blocks).no_ttl();
            auto const sink = g.node<BufferSink>(output.data(), output.size());
            dormant(input);
            sink(dormant);
            g.outputs();
            return iv::NodeExecutor::create(
                iv::TypeErasedNode(g.build()),
                {},
                execution_target_registry,
                executor_id
            );
        };

        iv::test::FakeAudioDevice device({ .max_block_frames = sample_count });
        iv::ExecutionTargetRegistry targets(iv::test::make_audio_device_provider(device));
        auto processor = make_processor(targets, 2);
        std::array<size_t, 2> const blocks { 8, 8 };
        iv::test::run_processor_blocks(processor, blocks);

        iv::test::require(tick_blocks == 2, "NodeRef::no_ttl should keep the node executing on both blocks");
        iv::test::require(skip_blocks == 0, "NodeRef::no_ttl should suppress dormant skip_block execution");
        require_close(silence, output, 0.0f, "DormantZeroNode output should remain silent");
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

    {
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::boundary), iv::EventTypeId::boundary);
        iv::EventOutputPort output(
            shared,
            iv::EventTypeId::trigger,
            iv::EventConversionRegistry::instance().plan(iv::EventTypeId::trigger, iv::EventTypeId::boundary)
        );
        iv::EventInputPort input(shared);

        output.push(storage, iv::TriggerEvent {}, 0, 0, 1);

        auto first_block = input.get_block(storage, 0, 1);
        iv::test::require(first_block.size() == 1, "trigger->boundary should emit one begin boundary in the current block");
        iv::test::require(std::get<iv::BoundaryEvent>(first_block[0].value).is_begin, "first converted boundary should be begin");
        iv::test::require(first_block[0].time == 0, "first converted boundary should be reindexed to the local block");

        auto second_block = input.get_block(storage, 1, 1);
        iv::test::require(second_block.size() == 1, "trigger->boundary should emit one end boundary in the following block");
        iv::test::require(!std::get<iv::BoundaryEvent>(second_block[0].value).is_begin, "second converted boundary should be end");
        iv::test::require(second_block[0].time == 0, "second converted boundary should be reindexed in the next block");
    }

    {
        std::cerr << "midi-test: pitch-control\n";
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::midi), iv::EventTypeId::midi);
        iv::EventOutputPort source(shared, iv::EventTypeId::midi);
        iv::EventInputPort input(shared);
        std::array<iv::Sample, 16> output_buffer {};
        iv::SharedPortData shared_output(std::span<iv::Sample>(output_buffer), 0);
        iv::OutputPort output(shared_output, 0);
        std::array<iv::EventInputPort, 1> inputs { input };
        std::array<iv::OutputPort, 1> outputs { output };
        std::array<iv::Sample, 8> scratch {};
        iv::MidiPitch::State state {};
        state.block = scratch;

        source.push(storage, make_note_on(60), 1, 0, 8);
        source.push(storage, make_note_on(64), 3, 0, 8);
        source.push(storage, make_note_off(64), 5, 0, 8);
        source.push(storage, make_pitch_bend(16383), 6, 0, 8);

        iv::MidiPitch pitch;
        pitch.tick_block({
            iv::TickContext<iv::MidiPitch> {
                .inputs = {},
                .outputs = outputs,
                .event_inputs = inputs,
                .event_outputs = {},
                .event_streams = &storage,
                .buffer = std::as_writable_bytes(std::span(&state, 1)),
            },
            0,
            8,
        });

        {
            std::array<iv::Sample, 8> expected {};
            expected[0] = 0.0f;
            expected[1] = expected[2] = static_cast<iv::Sample>(iv::NOTE_NUMBER_TO_FREQUENCY[60]);
            expected[3] = expected[4] = static_cast<iv::Sample>(iv::NOTE_NUMBER_TO_FREQUENCY[64]);
            expected[5] = static_cast<iv::Sample>(iv::NOTE_NUMBER_TO_FREQUENCY[60]);
            iv::Sample const bent_c4 = static_cast<iv::Sample>(
                iv::NOTE_NUMBER_TO_FREQUENCY[60] * std::exp2((2.0 / 12.0) * (8191.0 / 8192.0))
            );
            expected[6] = expected[7] = bent_c4;
            require_close(expected, std::span<iv::Sample const>(output_buffer.data(), 8), 1e-4f, "MidiPitch first block mismatch");
        }

        source.push(storage, make_note_off(60), 1, 8, 8);
        pitch.tick_block({
            iv::TickContext<iv::MidiPitch> {
                .inputs = {},
                .outputs = outputs,
                .event_inputs = inputs,
                .event_outputs = {},
                .event_streams = &storage,
                .buffer = std::as_writable_bytes(std::span(&state, 1)),
            },
            8,
            8,
        });

        {
            std::array<iv::Sample, 8> expected {};
            iv::Sample const bent_c4 = static_cast<iv::Sample>(
                iv::NOTE_NUMBER_TO_FREQUENCY[60] * std::exp2((2.0 / 12.0) * (8191.0 / 8192.0))
            );
            expected[0] = bent_c4;
            require_close(expected, std::span<iv::Sample const>(output_buffer.data() + 8, 8), 1e-4f, "MidiPitch release block mismatch");
        }
    }

    {
        std::cerr << "midi-test: gate\n";
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::midi), iv::EventTypeId::midi);
        iv::EventOutputPort source(shared, iv::EventTypeId::midi);
        iv::EventInputPort input(shared);
        std::array<iv::Sample, 8> output_buffer {};
        iv::SharedPortData shared_output(std::span<iv::Sample>(output_buffer), 0);
        iv::OutputPort output(shared_output, 0);
        std::array<iv::EventInputPort, 1> inputs { input };
        std::array<iv::OutputPort, 1> outputs { output };
        std::array<iv::Sample, 8> scratch {};
        iv::MidiGate::State state {};
        state.block = scratch;

        source.push(storage, make_note_on(60), 1, 0, 8);
        source.push(storage, make_note_on(60), 2, 0, 8);
        source.push(storage, make_note_off(60), 4, 0, 8);
        source.push(storage, make_note_off(60), 6, 0, 8);

        iv::MidiGate gate;
        gate.tick_block({
            iv::TickContext<iv::MidiGate> {
                .inputs = {},
                .outputs = outputs,
                .event_inputs = inputs,
                .event_outputs = {},
                .event_streams = &storage,
                .buffer = std::as_writable_bytes(std::span(&state, 1)),
            },
            0,
            8,
        });

        std::array<iv::Sample, 8> expected { 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f };
        require_close(expected, output_buffer, 0.0f, "MidiGate block mismatch");
    }

    {
        std::cerr << "event-test: conversion-plans\n";
        auto const same_type = iv::EventConversionRegistry::instance().plan(iv::EventTypeId::trigger, iv::EventTypeId::trigger);
        iv::test::require(same_type.steps.empty(), "same-type event conversion plan should be empty");

        auto const midi_to_boundary = iv::EventConversionRegistry::instance().plan(iv::EventTypeId::midi, iv::EventTypeId::boundary);
        iv::test::require(midi_to_boundary.steps.size() == 1, "midi->boundary should use the direct conversion path");
        iv::test::require(
            midi_to_boundary.steps[0] == iv::EventConversionStepId::midi_to_boundary,
            "midi->boundary should prefer the direct lower-assumption conversion"
        );

        auto const boundary_to_midi = iv::EventConversionRegistry::instance().plan(iv::EventTypeId::boundary, iv::EventTypeId::midi);
        iv::test::require(boundary_to_midi.steps.size() == 1, "boundary->midi should use the direct conversion path");
        iv::test::require(
            boundary_to_midi.steps[0] == iv::EventConversionStepId::boundary_to_midi,
            "boundary->midi should prefer the direct lower-assumption conversion"
        );

        iv::test::expect_failure(
            [&] {
                (void)iv::EventConversionRegistry::instance().plan(static_cast<iv::EventTypeId>(99), iv::EventTypeId::trigger);
            },
            "invalid event type",
            "invalid event type should fail event conversion planning clearly"
        );
    }

    {
        std::cerr << "event-test: broadcast-mixed-targets\n";
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventOutputPort output(shared, iv::EventTypeId::trigger);
        iv::EventInputPort input(shared);

        output.push(storage, iv::TriggerEvent {}, 2, 0, 8);
        output.push(storage, iv::TriggerEvent {}, 6, 0, 8);

        auto first_window = input.get_block(storage, 0, 4);
        iv::test::require(first_window.size() == 1, "first sub-block should only observe the first trigger");
        iv::test::require(first_window[0].time == 2, "first sub-block trigger should keep local offset");

        auto second_window = input.get_block(storage, 4, 4);
        iv::test::require(second_window.size() == 1, "second sub-block should only observe the second trigger");
        iv::test::require(second_window[0].time == 2, "second sub-block trigger should be reindexed relative to the later block");
    }

    {
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventOutputPort output(shared, iv::EventTypeId::trigger);
        iv::EventInputPort input(shared);

        output.push(storage, iv::TriggerEvent {}, 3, 0, 4);
        output.push(storage, iv::TriggerEvent {}, 0, 4, 4);

        auto first_window = input.get_block(storage, 0, 4);
        iv::test::require(first_window.size() == 1, "first block should include events before the block end only");
        iv::test::require(first_window[0].time == 3, "first block event should keep its local offset at the inclusive lower edge");

        auto second_window = input.get_block(storage, 4, 4);
        iv::test::require(second_window.size() == 1, "second block should include events at its start");
        iv::test::require(second_window[0].time == 0, "event at block boundary should appear at local offset zero in the next block");
    }

    {
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventOutputPort output(shared, iv::EventTypeId::trigger);
        iv::EventInputPort input(shared);

        output.push(storage, iv::TriggerEvent {}, 1, 0, 8);
        output.push(storage, iv::TriggerEvent {}, 5, 0, 8);

        std::vector<size_t> first_times;
        input.for_each_in_block(storage, 0, 4, [&](iv::TimedEvent const& event, size_t) {
            first_times.push_back(event.time);
        });

        std::vector<size_t> second_times;
        input.for_each_in_block(storage, 4, 4, [&](iv::TimedEvent const& event, size_t) {
            second_times.push_back(event.time);
        });

        iv::test::require(first_times.size() == 1, "callback block iteration should observe the earlier event");
        iv::test::require(first_times[0] == 1, "callback block iteration should reindex the earlier event locally");
        iv::test::require(second_times.size() == 1, "callback block iteration should observe the later event");
        iv::test::require(second_times[0] == 1, "callback block iteration should reindex the later event locally");
    }

    {
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventOutputPort output(shared, iv::EventTypeId::trigger);
        iv::EventInputPort input(shared);

        output.push(storage, iv::TriggerEvent {}, 3, 0, 8);

        auto block = input.get_block(storage, 0, 8);
        iv::test::require(block.size() == 1, "identity event write should preserve one trigger");
        iv::test::require(std::holds_alternative<iv::TriggerEvent>(block[0].value), "identity event write should preserve trigger type");
        iv::test::require(block[0].time == 3, "identity event write should preserve local time");
    }

    {
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventOutputPort output(
            shared,
            iv::EventTypeId::midi,
            iv::EventConversionRegistry::instance().plan(iv::EventTypeId::midi, iv::EventTypeId::trigger)
        );
        iv::EventInputPort input(shared);

        output.push(storage, iv::MidiEvent { .bytes = { 0x90, 60, 100 }, .size = 3 }, 1, 0, 8);
        output.push(storage, iv::MidiEvent { .bytes = { 0x80, 60, 0 }, .size = 3 }, 2, 0, 8);

        auto block = input.get_block(storage, 0, 8);
        iv::test::require(block.size() == 1, "midi->trigger should ignore note-off events");
        iv::test::require(std::holds_alternative<iv::TriggerEvent>(block[0].value), "midi->trigger should produce a trigger");
        iv::test::require(block[0].time == 1, "midi->trigger should preserve note-on timing");
    }

    {
        iv::EventStreamStorage storage;
        iv::EventSharedPortData shared(storage.allocate(iv::EventTypeId::midi), iv::EventTypeId::midi);
        iv::EventOutputPort output(
            shared,
            iv::EventTypeId::boundary,
            iv::EventConversionRegistry::instance().plan(iv::EventTypeId::boundary, iv::EventTypeId::midi)
        );
        iv::EventInputPort input(shared);

        output.push(storage, iv::BoundaryEvent { .is_begin = true }, 2, 0, 8);
        output.push(storage, iv::BoundaryEvent { .is_begin = false }, 5, 0, 8);

        auto block = input.get_block(storage, 0, 8);
        iv::test::require(block.size() == 2, "boundary->midi should preserve one message per boundary");
        auto const first = std::get<iv::MidiEvent>(block[0].value);
        auto const second = std::get<iv::MidiEvent>(block[1].value);
        iv::test::require(first.size == 3 && first.bytes[0] == 0x90, "boundary begin should become note-on midi");
        iv::test::require(second.size == 3 && second.bytes[0] == 0x80, "boundary end should become note-off midi");
    }

    {
        iv::EventStreamStorage storage;
        iv::EventSharedPortData source_shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventSharedPortData out_a_shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventSharedPortData out_b_shared(storage.allocate(iv::EventTypeId::boundary), iv::EventTypeId::boundary);
        iv::EventOutputPort source_output(source_shared, iv::EventTypeId::trigger);
        iv::EventInputPort source_input(source_shared);
        iv::EventOutputPort out_a(out_a_shared, iv::EventTypeId::trigger);
        iv::EventOutputPort out_b(
            out_b_shared,
            iv::EventTypeId::trigger,
            iv::EventConversionRegistry::instance().plan(iv::EventTypeId::trigger, iv::EventTypeId::boundary)
        );
        iv::EventInputPort in_a(out_a_shared);
        iv::EventInputPort in_b(out_b_shared);

        source_output.push(storage, iv::TriggerEvent {}, 1, 0, 8);
        source_output.push(storage, iv::TriggerEvent {}, 7, 0, 8);

        iv::BroadcastEvent node(2);
        std::array<iv::EventInputPort, 1> inputs { source_input };
        std::array<iv::EventOutputPort, 2> outputs { out_a, out_b };
        node.tick_block({
            iv::TickContext<iv::BroadcastEvent> {
                .inputs = {},
                .outputs = {},
                .event_inputs = inputs,
                .event_outputs = outputs,
                .event_streams = &storage,
                .buffer = {},
            },
            0,
            8,
        });

        {
            auto block_a = in_a.get_block(storage, 0, 8);
            iv::test::require(block_a.size() == 2, "BroadcastEvent should duplicate all source events to output A");
            iv::test::require(block_a[0].time == 1 && block_a[1].time == 7, "BroadcastEvent should preserve event timing on output A");
        }
        {
            auto block_b = in_b.get_block(storage, 0, 8);
            iv::test::require(block_b.size() == 3, "BroadcastEvent should convert events independently per output");
            iv::test::require(std::holds_alternative<iv::BoundaryEvent>(block_b[0].value), "BroadcastEvent output B should receive boundary events after conversion");
            iv::test::require(std::get<iv::BoundaryEvent>(block_b[0].value).is_begin, "BroadcastEvent output B should emit begin boundaries first");
            iv::test::require(block_b[0].time == 1, "BroadcastEvent should preserve the first converted begin boundary timing");
            iv::test::require(!std::get<iv::BoundaryEvent>(block_b[1].value).is_begin && block_b[1].time == 2, "BroadcastEvent should emit the matching end boundary when it remains in the block");
            iv::test::require(std::get<iv::BoundaryEvent>(block_b[2].value).is_begin && block_b[2].time == 7, "BroadcastEvent should keep later begin boundaries in the source block");
        }

        out_a.push(storage, iv::TriggerEvent {}, 7, 0, 8);
        {
            auto block_a = in_a.get_block(storage, 0, 8);
            iv::test::require(block_a.size() == 3, "BroadcastEvent output A should accept later independent appends");
        }
        {
            auto unchanged_block_b = in_b.get_block(storage, 0, 8);
            iv::test::require(unchanged_block_b.size() == 3, "BroadcastEvent output B should remain unaffected by output A after broadcast");
        }
        {
            auto spill_block_b = in_b.get_block(storage, 8, 8);
            iv::test::require(spill_block_b.size() == 1, "BroadcastEvent converted output should preserve delayed end events into the next block");
            iv::test::require(!std::get<iv::BoundaryEvent>(spill_block_b[0].value).is_begin && spill_block_b[0].time == 0, "BroadcastEvent converted output should emit the spilled end boundary in the next block");
        }
    }

    {
        iv::EventStreamStorage storage;
        iv::EventSharedPortData in_a_shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventSharedPortData in_b_shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventSharedPortData out_shared(storage.allocate(iv::EventTypeId::trigger), iv::EventTypeId::trigger);
        iv::EventOutputPort src_a(in_a_shared, iv::EventTypeId::trigger);
        iv::EventOutputPort src_b(in_b_shared, iv::EventTypeId::trigger);
        iv::EventInputPort in_a(in_a_shared);
        iv::EventInputPort in_b(in_b_shared);
        iv::EventOutputPort out(out_shared, iv::EventTypeId::trigger);
        iv::EventInputPort merged(out_shared);

        src_a.push(storage, iv::TriggerEvent {}, 2, 0, 8);
        src_b.push(storage, iv::TriggerEvent {}, 2, 0, 8);
        src_b.push(storage, iv::TriggerEvent {}, 5, 0, 8);

        iv::EventConcatenation node(2);
        std::array<iv::EventInputPort, 2> inputs { in_a, in_b };
        std::array<iv::EventOutputPort, 1> outputs { out };
        node.tick_block({
            iv::TickContext<iv::EventConcatenation> {
                .inputs = {},
                .outputs = {},
                .event_inputs = inputs,
                .event_outputs = outputs,
                .event_streams = &storage,
                .buffer = {},
            },
            0,
            8,
        });

        auto block = merged.get_block(storage, 0, 8);
        iv::test::require(block.size() == 3, "EventConcatenation should preserve all input events");
        iv::test::require(block[0].time == 2, "EventConcatenation should preserve first input event timing");
        iv::test::require(block[1].time == 2, "EventConcatenation should append second input same-time event after first input");
        iv::test::require(block[2].time == 5, "EventConcatenation should preserve later input event timing");
    }

    {
        auto collect_absolute = [](iv::EventInputPort const& input, iv::EventStreamStorage& storage, std::span<size_t const> blocks) {
            std::vector<size_t> absolute;
            size_t index = 0;
            for (size_t block_size : blocks) {
                auto block = input.get_block(storage, index, block_size);
                for (size_t i = 0; i < block.size(); ++i) {
                    absolute.push_back(index + block[i].time);
                }
                index += block_size;
            }
            return absolute;
        };

        iv::EventStreamStorage storage_a;
        iv::EventSharedPortData shared_a(storage_a.allocate(iv::EventTypeId::boundary), iv::EventTypeId::boundary);
        iv::EventOutputPort output_a(
            shared_a,
            iv::EventTypeId::trigger,
            iv::EventConversionRegistry::instance().plan(iv::EventTypeId::trigger, iv::EventTypeId::boundary)
        );
        iv::EventInputPort input_a(shared_a);
        output_a.push(storage_a, iv::TriggerEvent {}, 0, 0, 4);
        std::array<size_t, 1> whole_blocks { 4 };
        auto whole = collect_absolute(input_a, storage_a, whole_blocks);

        iv::EventStreamStorage storage_b;
        iv::EventSharedPortData shared_b(storage_b.allocate(iv::EventTypeId::boundary), iv::EventTypeId::boundary);
        iv::EventOutputPort output_b(
            shared_b,
            iv::EventTypeId::trigger,
            iv::EventConversionRegistry::instance().plan(iv::EventTypeId::trigger, iv::EventTypeId::boundary)
        );
        iv::EventInputPort input_b(shared_b);
        output_b.push(storage_b, iv::TriggerEvent {}, 0, 0, 4);
        std::array<size_t, 2> split_blocks { 2, 2 };
        auto split = collect_absolute(input_b, storage_b, split_blocks);

        iv::test::require(whole == split, "event observation should be invariant under block partitioning");
    }

    {
        std::cerr << "event-test: graph-event-metadata\n";
        iv::GraphBuilder g;
        auto input = g.event_input("transport", iv::EventTypeId::boundary);
        g.event_outputs(input);
        g.outputs();

        auto graph = g.build();
        auto inputs = graph.event_inputs();
        auto outputs = graph.event_outputs();
        iv::test::require(inputs.size() == 1, "graph should expose one public event input");
        iv::test::require(outputs.size() == 1, "graph should expose one public event output");
        iv::test::require(inputs[0].name == "transport", "graph should preserve the public event input name");
        iv::test::require(inputs[0].type == iv::EventTypeId::boundary, "graph should preserve the public event input type");
        iv::test::require(outputs[0].type == iv::EventTypeId::boundary, "graph should preserve the public event output type");
    }

    {
        using namespace iv::literals;
        std::cerr << "event-test: node-event-wiring\n";
        iv::GraphBuilder g;
        auto input = g.event_input("gate", iv::EventTypeId::trigger);
        auto passthrough_a = g.node<TriggerPassthrough>();
        auto passthrough_b = g.node<TriggerPassthrough>();

        passthrough_a(input);
        passthrough_b("in"_P = input);
        g.event_outputs(passthrough_a >> iv::events >> "out"_P, iv::events << passthrough_b >> "out"_P);
        g.outputs();

        auto graph = g.build();
        auto outputs = graph.event_outputs();
        iv::test::require(outputs.size() == 2, "node event wiring should expose both connected event outputs");
        iv::test::require(outputs[0].type == iv::EventTypeId::trigger, "positional event wiring should preserve trigger type");
        iv::test::require(outputs[1].type == iv::EventTypeId::trigger, "named event wiring should preserve trigger type");
    }

    {
        std::cerr << "event-test: subgraph-event-metadata\n";
        iv::GraphBuilder child;
        auto input = child.event_input("gate", iv::EventTypeId::trigger);
        child.event_outputs(input);
        child.outputs();

        iv::GraphBuilder parent;
        auto nested = parent.node(std::move(child));
        auto const& nested_node = nested.node();
        iv::test::require(nested_node.event_inputs().size() == 1, "subgraph placeholder should expose child public event inputs");
        iv::test::require(nested_node.event_outputs().size() == 1, "subgraph placeholder should expose child public event outputs");
        iv::test::require(nested_node.event_inputs()[0].name == "gate", "subgraph placeholder should preserve child event input name");
        iv::test::require(nested_node.event_inputs()[0].type == iv::EventTypeId::trigger, "subgraph placeholder should preserve child event input type");
        iv::test::require(nested_node.event_outputs()[0].type == iv::EventTypeId::trigger, "subgraph placeholder should preserve child event output type");
    }

    {
        std::cerr << "event-test: executor-initializes-direct-event-node-wiring\n";
        using namespace iv::literals;
        iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
        iv::ExecutionTargetRegistry execution_target_registry(
            iv::test::make_audio_device_provider(audio_device),
            audio_device.device().config().sample_rate
        );

        iv::GraphBuilder g;
        auto input = g.event_input("gate", iv::EventTypeId::trigger);
        auto source = g.node<TriggerPassthrough>();
        auto sink = g.node<TriggerPassthrough>();
        source(input);
        sink(source >> iv::events >> "out"_P);
        g.outputs();

        iv::EventStreamStorage event_stream_storage;
        auto resources = iv::test::make_resource_context(audio_device);
        resources.event_streams = &event_stream_storage;
        auto executor = iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            std::move(resources),
            execution_target_registry,
            1
        );
        (void)executor;
    }

    {
        std::cerr << "event-test: executor-initializes-subgraph-event-node-wiring\n";
        using namespace iv::literals;
        iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
        iv::ExecutionTargetRegistry execution_target_registry(
            iv::test::make_audio_device_provider(audio_device),
            audio_device.device().config().sample_rate
        );

        iv::GraphBuilder child;
        auto child_input = child.event_input("gate", iv::EventTypeId::trigger);
        auto child_source = child.node<TriggerPassthrough>();
        auto child_sink = child.node<TriggerPassthrough>();
        child_source(child_input);
        child_sink(child_source >> iv::events >> "out"_P);
        child.outputs();

        iv::GraphBuilder parent;
        parent.node(std::move(child));
        parent.outputs();

        iv::EventStreamStorage event_stream_storage;
        auto resources = iv::test::make_resource_context(audio_device);
        resources.event_streams = &event_stream_storage;
        auto executor = iv::NodeExecutor::create(
            iv::TypeErasedNode(parent.build()),
            std::move(resources),
            execution_target_registry,
            1
        );
        (void)executor;
    }

    {
        std::cerr << "event-test: executor-initializes-event-fanout-wiring\n";
        using namespace iv::literals;
        iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
        iv::ExecutionTargetRegistry execution_target_registry(
            iv::test::make_audio_device_provider(audio_device),
            audio_device.device().config().sample_rate
        );

        iv::GraphBuilder g;
        auto input = g.event_input("gate", iv::EventTypeId::trigger);
        auto source = g.node<TriggerPassthrough>();
        auto sink_a = g.node<TriggerPassthrough>();
        auto sink_b = g.node<TriggerPassthrough>();
        source(input);
        auto event_out = source >> iv::events >> "out"_P;
        sink_a(event_out);
        sink_b(event_out);
        g.outputs();

        iv::EventStreamStorage event_stream_storage;
        auto resources = iv::test::make_resource_context(audio_device);
        resources.event_streams = &event_stream_storage;
        auto executor = iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            std::move(resources),
            execution_target_registry,
            1
        );
        (void)executor;
    }

    {
        std::cerr << "event-test: executor-initializes-source-event-fanout-wiring\n";
        using namespace iv::literals;
        iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
        iv::ExecutionTargetRegistry execution_target_registry(
            iv::test::make_audio_device_provider(audio_device),
            audio_device.device().config().sample_rate
        );

        iv::GraphBuilder g;
        auto source = g.node<TriggerSource>();
        auto sink_a = g.node<TriggerPassthrough>();
        auto sink_b = g.node<TriggerPassthrough>();
        auto event_out = source >> iv::events >> "out"_P;
        sink_a(event_out);
        sink_b(event_out);
        g.outputs();

        iv::EventStreamStorage event_stream_storage;
        auto resources = iv::test::make_resource_context(audio_device);
        resources.event_streams = &event_stream_storage;
        auto executor = iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            std::move(resources),
            execution_target_registry,
            1
        );
        (void)executor;
    }

    {
        std::cerr << "event-test: event-storage-growth\n";
        iv::EventStreamStorage storage;
        iv::EventSharedPortData stream_a(storage.allocate(iv::EventTypeId::trigger, 1), iv::EventTypeId::trigger);
        iv::EventSharedPortData stream_b(storage.allocate(iv::EventTypeId::trigger, 1), iv::EventTypeId::trigger);
        iv::EventOutputPort out_a(stream_a, iv::EventTypeId::trigger);
        iv::EventOutputPort out_b(stream_b, iv::EventTypeId::trigger);
        iv::EventInputPort in_a(stream_a);
        iv::EventInputPort in_b(stream_b);

        for (size_t i = 0; i < 32; ++i) {
            out_a.push(storage, iv::TriggerEvent {}, i, 0, 64);
        }
        out_b.push(storage, iv::TriggerEvent {}, 9, 0, 64);

        {
            auto block_a = in_a.get_block(storage, 0, 64);
            iv::test::require(block_a.size() == 32, "storage growth should retain every event in the grown stream");
            iv::test::require(block_a[0].time == 0, "storage growth should preserve the earliest event");
            iv::test::require(block_a[31].time == 31, "storage growth should preserve the latest event after multiple reallocations");
        }
        {
            auto block_b = in_b.get_block(storage, 0, 64);
            iv::test::require(block_b.size() == 1, "storage growth in one stream should not corrupt another stream");
            iv::test::require(block_b[0].time == 9, "other streams should preserve their events across pool growth");
        }
    }

    return 0;
}
