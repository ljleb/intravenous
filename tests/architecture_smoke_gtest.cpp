#include "devices/channel_buffer_sink.h"
#include "devices/miniaudio_device.h"
#include "block_rate_buffer.h"
#include "basic_nodes/buffers.h"
#include "dsl.h"
#include "graph_node.h"
#include "module_test_utils.h"
#include "node_layout.h"

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <string_view>

namespace {
    using namespace iv::literals;

    static constexpr auto kMidiToTriggerPlan = iv::EventConversionRegistry::plan(
        iv::EventTypeId::midi,
        iv::EventTypeId::trigger
    );
    static_assert(kMidiToTriggerPlan.step_count == 1);
    static_assert(kMidiToTriggerPlan[0] == iv::EventConversionStepId::midi_to_trigger);

    constexpr auto kBlockReadyTimeout = std::chrono::milliseconds(100);
    constexpr auto kAsyncWaitTimeout = std::chrono::milliseconds(50);

    template<typename Ref, typename... Args>
    concept NodeRefInvocable = requires(Ref ref, Args... args)
    {
        ref(args...);
    };

    template<typename L, typename R>
    concept GreaterInvocable = requires(L lhs, R rhs)
    {
        lhs > rhs;
    };

    template<typename L, typename R>
    concept LessInvocable = requires(L lhs, R rhs)
    {
        lhs < rhs;
    };

    template<typename L, typename R>
    concept ShiftRightInvocable = requires(L lhs, R rhs)
    {
        lhs >> rhs;
    };

    template<typename L, typename R>
    concept ShiftLeftInvocable = requires(L lhs, R rhs)
    {
        lhs << rhs;
    };

    template<typename T>
    concept BitNotInvocable = requires(T value)
    {
        ~value;
    };

    struct UnaryPassthrough {
        auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick(iv::TickSampleContext<UnaryPassthrough> const& ctx) const
        {
            ctx.outputs[0].push(ctx.inputs[0].get());
        }
    };

    struct MixedInputsNode {
        auto inputs() const
        {
            return std::array {
                iv::InputConfig { .name = "in" },
                iv::InputConfig { .name = "cutoff" },
                iv::InputConfig { .name = "resonance" },
            };
        }

        auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick(iv::TickSampleContext<MixedInputsNode> const& ctx) const
        {
            ctx.outputs[0].push(ctx.inputs[0].get() + ctx.inputs[1].get() + ctx.inputs[2].get());
        }
    };

    struct DisconnectedTriggerSource {
        auto event_outputs() const
        {
            return std::array<iv::EventOutputConfig, 1> {{
                { .name = "trigger", .type = iv::EventTypeId::trigger }
            }};
        }

        void tick_block(iv::TickBlockContext<DisconnectedTriggerSource> const& ctx) const
        {
            ctx.event_outputs[0].push(iv::TriggerEvent{}, 0, ctx.index, ctx.block_size);
        }
    };

    struct EventUnaryPassthrough {
        auto event_inputs() const
        {
            return std::array<iv::EventInputConfig, 1> {{
                { .name = "trigger", .type = iv::EventTypeId::trigger }
            }};
        }

        auto event_outputs() const
        {
            return std::array<iv::EventOutputConfig, 1> {{
                { .name = "trigger", .type = iv::EventTypeId::trigger }
            }};
        }

        void tick_block(iv::TickBlockContext<EventUnaryPassthrough> const& ctx) const
        {
            auto events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            ctx.event_outputs[0].push_block(events);
        }
    };

    struct OffsetTriggerSource {
        size_t sample_offset = 0;

        auto event_outputs() const
        {
            return std::array<iv::EventOutputConfig, 1> {{
                { .name = "trigger", .type = iv::EventTypeId::trigger }
            }};
        }

        void tick_block(iv::TickBlockContext<OffsetTriggerSource> const& ctx) const
        {
            if (sample_offset < ctx.block_size) {
                ctx.event_outputs[0].push(iv::TriggerEvent{}, sample_offset, ctx.index, ctx.block_size);
            }
        }
    };

    struct TriggerOrderRecorder {
        std::vector<size_t>* event_times = nullptr;

        auto event_inputs() const
        {
            return std::array<iv::EventInputConfig, 1> {{
                { .name = "trigger", .type = iv::EventTypeId::trigger }
            }};
        }

        void tick_block(iv::TickBlockContext<TriggerOrderRecorder> const& ctx) const
        {
            if (!event_times) {
                return;
            }
            event_times->clear();
            auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            for (iv::TimedEvent const& event : events) {
                event_times->push_back(event.time);
            }
        }
    };

    struct SourceOnlyNode {
        auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }
    };

    struct OverrideCanSkipNode {
        auto event_outputs() const
        {
            return std::array<iv::EventOutputConfig, 1> {{
                { .name = "trigger", .type = iv::EventTypeId::trigger }
            }};
        }

        bool can_skip_block() const
        {
            return true;
        }
    };

    struct CountingSilentPassthrough {
        int* tick_count = nullptr;

        auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick(iv::TickSampleContext<CountingSilentPassthrough> const& ctx) const
        {
            if (tick_count) {
                ++*tick_count;
            }
            (void)ctx.inputs[0].get();
            ctx.outputs[0].push(0.0f);
        }
    };

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

    struct CountingBlockPassthrough {
        std::atomic<size_t>* block_count = nullptr;

        auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick_block(iv::TickBlockContext<CountingBlockPassthrough> const& ctx) const
        {
            if (block_count) {
                block_count->fetch_add(1, std::memory_order_relaxed);
            }
            ctx.outputs[0].push_block(ctx.inputs[0].get_block(ctx.block_size));
        }
    };

    static_assert(iv::details::fixed_input_count_v<UnaryPassthrough> == 1);
    static_assert(NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, iv::SamplePortRef>);
    static_assert(NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, decltype("in"_P = std::declval<iv::SamplePortRef>())>);
    static_assert(!NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, iv::SamplePortRef, iv::SamplePortRef>);
    static_assert(!NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, decltype("in"_P = std::declval<iv::SamplePortRef>()), iv::SamplePortRef>);
    static_assert(!NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, decltype("in"_P = std::declval<iv::SamplePortRef>()), decltype("in"_P = std::declval<iv::SamplePortRef>())>);
    static_assert(NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SamplePortRef,
        decltype("cutoff"_P = std::declval<iv::SamplePortRef>()),
        decltype("resonance"_P = std::declval<iv::SamplePortRef>())
    >);
    static_assert(NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SamplePortRef,
        iv::SamplePortRef,
        decltype("resonance"_P = std::declval<iv::SamplePortRef>())
    >);
    static_assert(!NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        decltype("cutoff"_P = std::declval<iv::SamplePortRef>()),
        iv::SamplePortRef
    >);
    static_assert(!NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SamplePortRef,
        decltype("cutoff"_P = std::declval<iv::SamplePortRef>()),
        iv::SamplePortRef
    >);
    static_assert(!NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SamplePortRef,
        decltype("cutoff"_P = std::declval<iv::SamplePortRef>()),
        decltype("cutoff"_P = std::declval<iv::SamplePortRef>())
    >);
    static_assert(!NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SamplePortRef,
        iv::SamplePortRef,
        iv::SamplePortRef,
        decltype("resonance"_P = std::declval<iv::SamplePortRef>())
    >);
    static_assert(GreaterInvocable<iv::SamplePortRef, iv::StructuredNodeRef<UnaryPassthrough>>);
    static_assert(LessInvocable<iv::StructuredNodeRef<UnaryPassthrough>, iv::SamplePortRef>);
    static_assert(GreaterInvocable<iv::StructuredNodeRef<DisconnectedTriggerSource>, iv::TypedNodeRef<EventUnaryPassthrough>>);
    static_assert(LessInvocable<iv::TypedNodeRef<EventUnaryPassthrough>, iv::StructuredNodeRef<DisconnectedTriggerSource>>);
    static_assert(ShiftLeftInvocable<decltype("value"_P), iv::StructuredNodeRef<UnaryPassthrough>>);
    static_assert(ShiftRightInvocable<iv::StructuredNodeRef<UnaryPassthrough>, decltype("value"_P)>);
    static_assert(BitNotInvocable<iv::SamplePortRef>);
    static_assert(BitNotInvocable<iv::StructuredNodeRef<UnaryPassthrough>>);
    static_assert(std::same_as<iv::details::sample_node_ref_for_t<iv::Broadcast>, iv::TypedNodeRef<iv::Broadcast>>);

    void expect_constant_block(std::span<iv::Sample const> block, iv::Sample expected)
    {
        for (size_t i = 0; i < block.size(); ++i) {
            if (block[i] != expected) {
                FAIL() << "mismatch at sample " << i
                       << ": expected " << expected
                       << ", got " << block[i];
            }
        }
    }

    void expect_interleaved_channels(
        std::span<iv::Sample const> left,
        iv::Sample expected_left,
        std::span<iv::Sample const> right,
        iv::Sample expected_right
    )
    {
        ASSERT_EQ(left.size(), right.size());
        for (size_t i = 0; i < left.size(); ++i) {
            if (left[i] != expected_left) {
                FAIL() << "left mismatch at sample " << i
                       << ": expected " << expected_left
                       << ", got " << left[i];
            }
            if (right[i] != expected_right) {
                FAIL() << "right mismatch at sample " << i
                       << ": expected " << expected_right
                       << ", got " << right[i];
            }
        }
    }

    template<typename Fn>
    void expect_failure_contains(Fn&& fn, std::string_view needle)
    {
        try {
            fn();
        } catch (std::exception const& e) {
            EXPECT_NE(std::string_view(e.what()).find(needle), std::string_view::npos);
            return;
        }

        FAIL() << "expected exception containing: " << needle;
    }

    iv::NodeExecutor make_constant_audio_executor(
        iv::Sample* value,
        iv::ExecutionTargetRegistry execution_target_registry,
        size_t channel = 0
    )
    {
        iv::GraphBuilder g;
        auto const src = g.node<iv::ValueSource>(value);
        auto const sink = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
            .device_id = 0,
            .channel = channel,
        });
        sink(src);
        g.outputs();

        return iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            {},
            std::move(execution_target_registry).to_builder()
        );
    }

    iv::TypeErasedNode make_constant_audio_graph(iv::Sample* value, size_t channel = 0)
    {
        iv::GraphBuilder g;
        auto const src = g.node<iv::ValueSource>(value);
        auto const sink = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
            .device_id = 0,
            .channel = channel,
        });
        sink(src);
        g.outputs();
        return iv::TypeErasedNode(g.build());
    }

    void tick_executor_direct(iv::NodeExecutor& executor, size_t index, size_t block_size)
    {
        if (block_size == 0) {
            return;
        }
        iv::validate_block_size(block_size, "test block size must be a power of 2");
        if (block_size > executor.max_block_size()) {
            throw std::logic_error("test block size exceeds executor max block size");
        }

        executor.root().tick_block({
            iv::TickContext<iv::TypeErasedNode> {
                .inputs = {},
                .outputs = {},
                .event_inputs = {},
                .event_outputs = {},
                .buffer = executor.storage().buffer(),
            },
            index,
            block_size
        });
    }

    template<typename Executor>
    auto start_executor(Executor& executor)
    {
        return std::async(std::launch::async, [&] {
            executor.execute();
        });
    }

    template<typename Executor>
    void stop_executor(iv::test::FakeAudioDevice& audio_device, Executor& executor, std::future<void>& worker)
    {
        executor.request_shutdown();
        if (worker.wait_for(kAsyncWaitTimeout) != std::future_status::ready) {
            audio_device.begin_requested_block(0, 1);
            ASSERT_EQ(worker.wait_for(kBlockReadyTimeout), std::future_status::ready)
                << "execute() did not exit after shutdown";
            audio_device.finish_requested_block();
        }
        worker.get();
    }

    template<typename Executor>
    void request_block_and_wait(iv::test::FakeAudioDevice& audio_device, Executor&, size_t index, size_t block_size)
    {
        audio_device.begin_requested_block(index, block_size);
        ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout))
            << "requested block [" << index << ", " << block_size << "] did not become ready";
    }

    template<typename Executor>
    void request_block_with_manual_sync(iv::test::FakeAudioDevice& audio_device, Executor& executor, size_t index, size_t block_size)
    {
        audio_device.begin_requested_block(index, block_size);
        executor.device_orchestrator().wait_for_block();

        auto worker = std::async(std::launch::async, [&] {
            tick_executor_direct(executor, index, block_size);
            executor.device_orchestrator().sync_block(index, block_size);
        });

        ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout))
            << "requested block [" << index << ", " << block_size << "] did not become ready";

        audio_device.begin_requested_block(index + block_size, block_size);
        ASSERT_EQ(worker.wait_for(kBlockReadyTimeout), std::future_status::ready)
            << "manual sync did not exit after the requested block became ready";
        worker.get();
    }

    template<typename Executor, typename Fn>
    void execute_requested_block(
        iv::test::FakeAudioDevice& audio_device,
        Executor& executor,
        size_t index,
        size_t block_size,
        Fn&& verify
    )
    {
        auto worker = start_executor(executor);
        request_block_and_wait(audio_device, executor, index, block_size);
        verify();
        audio_device.finish_requested_block();
        stop_executor(audio_device, executor, worker);
    }

    void expect_zero_output(iv::test::FakeAudioDevice const& audio_device, size_t channel = 0)
    {
        expect_constant_block(audio_device.output_block(channel), 0.0f);
    }

    std::uint32_t read_wav_sample_rate(std::filesystem::path const& path)
    {
        std::ifstream in(path, std::ios::binary);
        EXPECT_TRUE(static_cast<bool>(in)) << "failed to open wav file: " << path.string();
        if (!in) {
            return 0;
        }

        std::array<unsigned char, 28> header {};
        in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        EXPECT_GE(in.gcount(), static_cast<std::streamsize>(header.size()))
            << "wav file too small: " << path.string();
        if (in.gcount() < static_cast<std::streamsize>(header.size())) {
            return 0;
        }

        return
            static_cast<std::uint32_t>(header[24]) |
            (static_cast<std::uint32_t>(header[25]) << 8) |
            (static_cast<std::uint32_t>(header[26]) << 16) |
            (static_cast<std::uint32_t>(header[27]) << 24);
    }

    std::vector<std::int16_t> read_wav_pcm16_samples(std::filesystem::path const& path)
    {
        std::ifstream in(path, std::ios::binary);
        EXPECT_TRUE(static_cast<bool>(in)) << "failed to open wav file: " << path.string();
        if (!in) {
            return {};
        }

        std::array<unsigned char, 44> header {};
        in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        EXPECT_GE(in.gcount(), static_cast<std::streamsize>(header.size()))
            << "wav file too small: " << path.string();
        if (in.gcount() < static_cast<std::streamsize>(header.size())) {
            return {};
        }

        std::uint32_t const data_size =
            static_cast<std::uint32_t>(header[40]) |
            (static_cast<std::uint32_t>(header[41]) << 8) |
            (static_cast<std::uint32_t>(header[42]) << 16) |
            (static_cast<std::uint32_t>(header[43]) << 24);
        EXPECT_EQ(data_size % sizeof(std::int16_t), 0u);
        if (data_size % sizeof(std::int16_t) != 0) {
            return {};
        }

        std::vector<std::int16_t> samples(data_size / sizeof(std::int16_t));
        in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(data_size));
        EXPECT_EQ(in.gcount(), static_cast<std::streamsize>(data_size))
            << "wav file payload shorter than header reported: " << path.string();
        return samples;
    }

    void expect_pcm16_constant(std::span<std::int16_t const> samples, iv::Sample expected)
    {
        for (size_t i = 0; i < samples.size(); ++i) {
            auto const actual = static_cast<iv::Sample>(samples[i]) / 32767.0f;
            if (std::abs(actual - expected) > 1e-4f) {
                FAIL() << "pcm mismatch at sample " << i
                       << ": expected " << expected
                       << ", got " << actual;
            }
        }
    }

    struct FakeAudioDeviceBackend {
        iv::test::FakeAudioDevice* device = nullptr;

        auto const& config() const
        {
            return device->config();
        }

        std::span<iv::Sample> wait_for_block_request()
        {
            return device->wait_for_block_request();
        }

        void submit_response()
        {
            device->submit_response();
        }
    };
}

TEST(ArchitectureSmoke, SameDeviceSameChannelAccumulates)
{
    iv::Sample a = 0.25f;
    iv::Sample b = 0.5f;
    std::array<iv::Sample, 8> channel {};
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = channel.size(),
            .preferred_block_size = channel.size(),
        }
    );

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

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    ASSERT_EQ(executor.max_block_size(), channel.size());
    execute_requested_block(audio_device, executor, 0, channel.size(), [&] {
        auto block = audio_device.output_block();
        std::copy_n(block.begin(), channel.size(), channel.begin());
        expect_constant_block(channel, 0.75f);
    });
}

TEST(ArchitectureSmoke, ChannelAlignmentPreservesLeftAndRight)
{
    iv::Sample left = 0.25f;
    iv::Sample right = 0.5f;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 2,
            .max_block_frames = 8,
        }
    );

    iv::GraphBuilder g;
    auto const src_left = g.node<iv::ValueSource>(&left);
    auto const src_right = g.node<iv::ValueSource>(&right);
    auto const sink_left = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 0,
    });
    auto const sink_right = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 1,
    });
    sink_left(src_left);
    sink_right(src_right);
    g.outputs();

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    execute_requested_block(audio_device, executor, 0, 8, [&] {
        auto left_block = audio_device.output_block(0);
        auto right_block = audio_device.output_block(1);
        expect_interleaved_channels(left_block, 0.25f, right_block, 0.5f);
    });
}

TEST(ArchitectureSmoke, RequestedBlockMismatchDoesNotSatisfyActiveRequest)
{
    iv::Sample value = 0.75f;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 16,
        }
    );

    auto executor = make_constant_audio_executor(&value, iv::test::make_audio_device_provider(audio_device));

    audio_device.begin_requested_block(0, 8);
    executor.device_orchestrator().wait_for_block();
    tick_executor_direct(executor, 0, 4);
    executor.device_orchestrator().sync_block(0, 4);
    expect_zero_output(audio_device);

    tick_executor_direct(executor, 4, 4);
    executor.request_shutdown();
    executor.device_orchestrator().sync_block(4, 4);
    ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout));
    auto block = audio_device.output_block();
    expect_constant_block(block.first(8), 0.75f);
    audio_device.finish_requested_block();
}

TEST(ArchitectureSmoke, SubgraphPassthroughFlattensToDirectSignalPath)
{
    iv::Sample value = 0.375f;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 8,
        }
    );

    iv::GraphBuilder g;
    auto const src = g.node<iv::ValueSource>(&value);
    auto const passthrough = g.subgraph([&] {
        g.outputs(src);
    });
    auto const sink = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 0,
    });
    sink(passthrough);
    g.outputs();

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    execute_requested_block(audio_device, executor, 0, 8, [&] {
        expect_constant_block(audio_device.output_block(), value);
    });
}

TEST(ArchitectureSmoke, CanSkipBlockDefaultsAndOverride)
{
    EXPECT_TRUE(iv::get_can_skip_block(UnaryPassthrough{}));
    EXPECT_FALSE(iv::get_can_skip_block(DisconnectedTriggerSource{}));
    EXPECT_FALSE(iv::get_can_skip_block(EventUnaryPassthrough{}));
    EXPECT_FALSE(iv::get_can_skip_block(SourceOnlyNode{}));
    EXPECT_TRUE(iv::get_can_skip_block(OverrideCanSkipNode{}));
}

TEST(ArchitectureSmoke, PipeOperatorChainsSingleInputSingleOutputNodes)
{
    iv::Sample value = 0.375f;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 8,
        }
    );

    iv::GraphBuilder g;
    auto const src = g.node<iv::ValueSource>(&value);
    auto const stage_a = g.node<UnaryPassthrough>();
    auto const stage_b = g.node<UnaryPassthrough>();
    auto const sink = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 0,
    });

    auto const piped = src > stage_a > stage_b;
    sink(piped);
    g.outputs();

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    execute_requested_block(audio_device, executor, 0, 8, [&] {
        expect_constant_block(audio_device.output_block(), value);
    });
}

TEST(ArchitectureSmoke, ExactRequestedBlockBecomesReady)
{
    iv::Sample value = 0.75f;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 16,
        }
    );

    iv::GraphBuilder g;
    auto const src = g.node<iv::ValueSource>(&value);
    auto const sink = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 0,
    });
    sink(src);
    g.outputs();

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    execute_requested_block(audio_device, executor, 8, 4, [&] {
        auto block = audio_device.output_block();
        expect_constant_block(block.first(4), 0.75f);
    });
}

TEST(ArchitectureSmoke, RepeatedRequestedBlocksDoNotOverflowBufferedDevice)
{
    iv::Sample value = 0.25f;
    constexpr size_t block_size = 256;
    constexpr size_t iterations = 136;

    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 4096,
            .preferred_block_size = block_size,
        }
    );

    auto executor = make_constant_audio_executor(&value, iv::test::make_audio_device_provider(audio_device));

    auto worker = start_executor(executor);
    for (size_t i = 0; i < iterations; ++i) {
        size_t const index = i * block_size;
        SCOPED_TRACE(::testing::Message() << "iteration=" << i << " block_index=" << index);
        request_block_and_wait(audio_device, executor, index, block_size);
        expect_constant_block(audio_device.output_block().first(block_size), value);
        audio_device.finish_requested_block();
    }
    stop_executor(audio_device, executor, worker);
}

TEST(ArchitectureSmoke, IndependentGraphBuilderStillLowersThroughNode)
{
    iv::Sample value = 0.375f;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 8,
        }
    );

    iv::GraphBuilder g;
    auto const src = g.node<iv::ValueSource>(&value);

    iv::GraphBuilder child;
    auto const input = child.input();
    child.outputs(input);

    auto const passthrough = g.node(child);
    auto const sink = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 0,
    });
    passthrough(src);
    sink(passthrough);
    g.outputs();

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    execute_requested_block(audio_device, executor, 0, 8, [&] {
        expect_constant_block(audio_device.output_block(), value);
    });
}

TEST(ArchitectureSmoke, ParentAwareSubgraphDormancyStopsTickingSilentScope)
{
    iv::Sample value = 0.0f;
    std::vector<iv::Sample> output(12, 0.0f);
    int tick_count = 0;

    iv::GraphBuilder g;
    auto const src = g.node<iv::ValueSource>(&value);
    auto const scoped = g.subgraph([&] {
        auto const stage = g.node<CountingSilentPassthrough>(CountingSilentPassthrough{ .tick_count = &tick_count });
        stage(src);
        g.outputs(stage);
    }).ttl(8);
    auto const sink = g.node<BufferSink>(output.data(), output.size());
    sink(scoped);
    g.outputs();

    iv::test::FakeAudioDevice audio_device({ .max_block_frames = output.size() });
    iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_targets).to_builder()
    );

    tick_executor_direct(executor, 0, 4);
    tick_executor_direct(executor, 4, 4);
    tick_executor_direct(executor, 8, 4);

    EXPECT_EQ(tick_count, 8);
}

TEST(ArchitectureSmoke, IndependentGraphBuilderDormancyStopsTickingSilentScope)
{
    iv::Sample value = 0.0f;
    std::vector<iv::Sample> output(12, 0.0f);
    int tick_count = 0;

    iv::GraphBuilder child;
    auto const child_input = child.input();
    auto const child_stage = child.node<CountingSilentPassthrough>(CountingSilentPassthrough{ .tick_count = &tick_count });
    child_stage(child_input);
    child.outputs(child_stage);

    iv::GraphBuilder g;
    auto const src = g.node<iv::ValueSource>(&value);
    auto const scoped = g.node(child).ttl(8);
    auto const sink = g.node<BufferSink>(output.data(), output.size());
    scoped(src);
    sink(scoped);
    g.outputs();

    iv::test::FakeAudioDevice audio_device({ .max_block_frames = output.size() });
    iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_targets).to_builder()
    );

    tick_executor_direct(executor, 0, 4);
    tick_executor_direct(executor, 4, 4);
    tick_executor_direct(executor, 8, 4);

    EXPECT_EQ(tick_count, 8);
}

TEST(ArchitectureSmoke, LargeRequestedBlockIndicesRemainReadyWithoutWarmup)
{
    iv::Sample value = 0.25f;
    constexpr size_t block_size = 256;
    constexpr size_t center_index = 8966400;
    constexpr size_t window_blocks = 32;
    size_t const start_index = center_index - (window_blocks / 2) * block_size;

    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 4096,
            .preferred_block_size = block_size,
        }
    );

    auto executor = make_constant_audio_executor(&value, iv::test::make_audio_device_provider(audio_device));

    auto worker = start_executor(executor);
    for (size_t i = 0; i < window_blocks; ++i) {
        size_t const index = start_index + i * block_size;
        SCOPED_TRACE(::testing::Message() << "iteration=" << i << " block_index=" << index);
        request_block_and_wait(audio_device, executor, index, block_size);
        expect_constant_block(audio_device.output_block().first(block_size), value);
        audio_device.finish_requested_block();
    }
    stop_executor(audio_device, executor, worker);
}

TEST(ArchitectureSmoke, DirectoryModuleRetainsModuleRefsAndRuns)
{
    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    auto processor = iv::test::make_executor(
        loader,
        audio_device,
        std::move(execution_target_registry),
        iv::test::test_modules_root() / "nested_loader_project"
    );

    EXPECT_NE(processor.num_module_refs(), 0u);
    iv::test::run_processor_ticks(audio_device, processor);
}

TEST(ArchitectureSmoke, DirectoryModuleEventArrayBindingsResolve)
{
    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();
    auto loaded = loader.load_root(
        iv::test::test_modules_root() / "event_loader_project",
        iv::test::module_render_config(audio_device),
        &audio_device.sample_period()
    );

    iv::NodeLayoutBuilder builder(8);
    {
        iv::DeclarationContext<iv::TypeErasedNode> ctx(builder, loaded.root);
        loaded.root.declare(ctx);
    }
    auto layout = std::move(builder).build();

    auto const event_type = iv::NodeLayoutBuilder::array_type_token<iv::EventSharedPortData>();

    std::vector<std::string> exported_event_ids;
    for (auto const& binding : layout.exported_arrays) {
        if (binding.element_type == event_type) {
            exported_event_ids.push_back(binding.id);
        }
    }
    std::sort(exported_event_ids.begin(), exported_event_ids.end());

    std::vector<std::string> missing_event_ids;
    for (auto const& binding : layout.imported_arrays) {
        if (binding.element_type != event_type) {
            continue;
        }
        if (std::find(exported_event_ids.begin(), exported_event_ids.end(), binding.id) == exported_event_ids.end()) {
            missing_event_ids.push_back(binding.id);
        }
    }
    std::sort(missing_event_ids.begin(), missing_event_ids.end());
    missing_event_ids.erase(std::unique(missing_event_ids.begin(), missing_event_ids.end()), missing_event_ids.end());

    std::unordered_map<std::string, size_t> exported_event_id_counts;
    for (auto const& id : exported_event_ids) {
        ++exported_event_id_counts[id];
    }
    std::vector<std::string> duplicate_exported_event_ids;
    for (auto const& [id, count] : exported_event_id_counts) {
        if (count > 1) {
            duplicate_exported_event_ids.push_back(id + " x" + std::to_string(count));
        }
    }
    std::sort(duplicate_exported_event_ids.begin(), duplicate_exported_event_ids.end());

    if (!missing_event_ids.empty()) {
        std::cerr << "missing exported EventSharedPortData ids:\n";
        for (auto const& id : missing_event_ids) {
            std::cerr << "  " << id << '\n';
        }
    }
    if (!duplicate_exported_event_ids.empty()) {
        std::cerr << "duplicate exported EventSharedPortData ids:\n";
        for (auto const& id : duplicate_exported_event_ids) {
            std::cerr << "  " << id << '\n';
        }
    }

    if (!missing_event_ids.empty()) {
        std::ostringstream out;
        out << "missing exported EventSharedPortData ids:\n";
        for (auto const& id : missing_event_ids) {
            out << id << '\n';
        }
        FAIL() << out.str();
    }

    if (!duplicate_exported_event_ids.empty()) {
        std::ostringstream out;
        out << "duplicate exported EventSharedPortData ids:\n";
        for (auto const& id : duplicate_exported_event_ids) {
            out << id << '\n';
        }
        FAIL() << out.str();
    }
}

TEST(ArchitectureSmoke, ConversionToEmptyDropsAllEvents)
{
    auto const midi_to_empty = iv::EventConversionRegistry::instance().plan(
        iv::EventTypeId::midi,
        iv::EventTypeId::empty
    );
    auto const trigger_to_empty = iv::EventConversionRegistry::instance().plan(
        iv::EventTypeId::trigger,
        iv::EventTypeId::empty
    );
    auto const boundary_to_empty = iv::EventConversionRegistry::instance().plan(
        iv::EventTypeId::boundary,
        iv::EventTypeId::empty
    );

    std::vector<iv::TimedEvent> converted;
    iv::EventConversionRegistry::instance().convert(
        midi_to_empty,
        iv::TimedEvent {
            .time = 3,
            .value = iv::MidiEvent { .bytes = { 0x90, 60, 127 }, .size = 3 }
        },
        [&](iv::TimedEvent const& event) { converted.push_back(event); }
    );
    iv::EventConversionRegistry::instance().convert(
        trigger_to_empty,
        iv::TimedEvent {
            .time = 4,
            .value = iv::TriggerEvent {}
        },
        [&](iv::TimedEvent const& event) { converted.push_back(event); }
    );
    iv::EventConversionRegistry::instance().convert(
        boundary_to_empty,
        iv::TimedEvent {
            .time = 5,
            .value = iv::BoundaryEvent { .is_begin = true }
        },
        [&](iv::TimedEvent const& event) { converted.push_back(event); }
    );

    EXPECT_TRUE(converted.empty());
    EXPECT_EQ(iv::calculate_event_port_buffer_capacity(256, iv::EventTypeId::empty), 0u);
}

TEST(ArchitectureSmoke, DisconnectedEventOutputUsesZeroCapacitySinkBudget)
{
    iv::GraphBuilder g;
    [[maybe_unused]] auto const source = g.node<DisconnectedTriggerSource>();
    g.outputs();

    iv::TypeErasedNode root = iv::TypeErasedNode(g.build());
    iv::NodeLayoutBuilder builder(8);
    {
        iv::DeclarationContext<iv::TypeErasedNode> ctx(builder, root);
        root.declare(ctx);
    }
    auto layout = std::move(builder).build();

    auto const timed_event_type = iv::NodeLayoutBuilder::array_type_token<iv::TimedEvent>();
    size_t zero_capacity_event_regions = 0;
    size_t nonzero_event_regions = 0;
    for (auto const& region : layout.regions) {
        if (
            region.kind == iv::NodeLayout::Region::Kind::local_array &&
            region.element_type == timed_event_type
        ) {
            if (region.element_count == 0) {
                ++zero_capacity_event_regions;
            } else {
                ++nonzero_event_regions;
            }
        }
    }

    EXPECT_GT(zero_capacity_event_regions, 0u);
    EXPECT_EQ(nonzero_event_regions, 0u);

    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    auto executor = iv::NodeExecutor::create(
        std::move(root),
        {},
        std::move(execution_target_registry).to_builder()
    );

    EXPECT_NO_THROW(tick_executor_direct(executor, 0, 8));
}

TEST(ArchitectureSmoke, EventConcatenationMergesInputsInTimeOrder)
{
    std::vector<size_t> event_times;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });

    iv::GraphBuilder g;
    auto const late = g.node<OffsetTriggerSource>(OffsetTriggerSource{ .sample_offset = 5 });
    auto const early = g.node<OffsetTriggerSource>(OffsetTriggerSource{ .sample_offset = 2 });
    auto const concat = g.node<iv::EventConcatenation>(2, iv::EventTypeId::trigger);
    auto const sink = g.node<TriggerOrderRecorder>(TriggerOrderRecorder{ .event_times = &event_times });

    concat.connect_event_input(0, late >> iv::events);
    concat.connect_event_input(1, early >> iv::events);
    sink.connect_event_input("trigger", concat >> iv::events);
    g.outputs();

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    auto executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    tick_executor_direct(executor, 0, 8);
    EXPECT_EQ(event_times, (std::vector<size_t>{ 2, 5 }));
}

TEST(ArchitectureSmoke, PolyphonicMixMergesEventLanesInTimeOrder)
{
    std::vector<size_t> event_times;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });

    iv::GraphBuilder g;
    auto const late = g.node<OffsetTriggerSource>(OffsetTriggerSource{ .sample_offset = 6 });
    auto const early = g.node<OffsetTriggerSource>(OffsetTriggerSource{ .sample_offset = 1 });
    auto const mix = g.node<iv::PolyphonicMix>(
        2,
        std::vector<iv::OutputConfig> {},
        std::vector<iv::EventOutputConfig> { iv::EventOutputConfig{ .name = "trigger", .type = iv::EventTypeId::trigger } }
    );
    auto const sink = g.node<TriggerOrderRecorder>(TriggerOrderRecorder{ .event_times = &event_times });

    mix.connect_event_input(0, late >> iv::events);
    mix.connect_event_input(1, early >> iv::events);
    sink.connect_event_input("trigger", mix >> iv::events);
    g.outputs();

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    auto executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    tick_executor_direct(executor, 0, 8);
    EXPECT_EQ(event_times, (std::vector<size_t>{ 1, 6 }));
}

TEST(ArchitectureSmoke, EventPortRingGetBlockPreservesOrderAcrossWraparound)
{
    std::array<iv::TimedEvent, 4> storage {};
    iv::EventSharedPortData shared(std::span<iv::TimedEvent>(storage), 0, 0, iv::EventTypeId::trigger);
    iv::EventOutputPort output(shared, iv::EventTypeId::trigger);
    iv::EventInputPort input(shared);

    output.push(iv::TimedEvent { .time = 0, .value = iv::TriggerEvent {} });
    output.push(iv::TimedEvent { .time = 1, .value = iv::TriggerEvent {} });

    EXPECT_EQ(input.get_block(2, 1).size(), 0u);

    output.push(iv::TimedEvent { .time = 2, .value = iv::TriggerEvent {} });
    output.push(iv::TimedEvent { .time = 3, .value = iv::TriggerEvent {} });
    output.push(iv::TimedEvent { .time = 4, .value = iv::TriggerEvent {} });

    auto const block = input.get_block(2, 3);
    ASSERT_EQ(block.size(), 3u);
    EXPECT_EQ(block[0].time, 2u);
    EXPECT_EQ(block[1].time, 3u);
    EXPECT_EQ(block[2].time, 4u);
}

TEST(ArchitectureSmoke, EventPortDropsOverflowingEvents)
{
    std::array<iv::TimedEvent, 2> storage {};
    iv::EventSharedPortData shared(std::span<iv::TimedEvent>(storage), 0, 0, iv::EventTypeId::trigger);
    iv::EventOutputPort output(shared, iv::EventTypeId::trigger);
    iv::EventInputPort input(shared);

    output.push(iv::TimedEvent { .time = 0, .value = iv::TriggerEvent {} });
    output.push(iv::TimedEvent { .time = 1, .value = iv::TriggerEvent {} });
    output.push(iv::TimedEvent { .time = 2, .value = iv::TriggerEvent {} });

    auto const block = input.get_block(0, 4);
    ASSERT_EQ(block.size(), 2u);
    EXPECT_EQ(block[0].time, 0u);
    EXPECT_EQ(block[1].time, 1u);
}

TEST(ArchitectureSmoke, ExecutorTeardownAllowsFreshExecutor)
{
    iv::Sample a = 0.25f;
    iv::Sample b = 0.5f;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });

    {
        auto executor_a = make_constant_audio_executor(&a, iv::test::make_audio_device_provider(audio_device));
        execute_requested_block(audio_device, executor_a, 0, 8, [&] {
            expect_constant_block(audio_device.output_block(), 0.25f);
        });
    }

    auto executor_b = make_constant_audio_executor(&b, iv::test::make_audio_device_provider(audio_device));
    execute_requested_block(audio_device, executor_b, 8, 8, [&] {
        expect_constant_block(audio_device.output_block(), 0.5f);
    });
}

TEST(ArchitectureSmoke, ReloadReplacesAudioContribution)
{
    iv::Sample a = 0.25f;
    iv::Sample b = 0.5f;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    auto executor = make_constant_audio_executor(&a, iv::test::make_audio_device_provider(audio_device));

    request_block_with_manual_sync(audio_device, executor, 0, 8);
    expect_constant_block(audio_device.output_block(), 0.25f);
    audio_device.finish_requested_block();

    executor.reload(make_constant_audio_graph(&b));

    request_block_with_manual_sync(audio_device, executor, 8, 8);
    expect_constant_block(audio_device.output_block(), 0.5f);
    audio_device.finish_requested_block();
}

TEST(ArchitectureSmoke, RegistryReadyCheckDoesNotBlockWithoutAudioTargets)
{
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    auto& execution_targets = execution_target_registry;

    auto wait_result = std::async(std::launch::async, [&] {
        return execution_targets.wait_for_block(0, 8);
    });

    EXPECT_EQ(wait_result.wait_for(kAsyncWaitTimeout), std::future_status::ready)
        << "sync_block should not wait forever when an executor has no active audio devices";

    (void)wait_result.wait_for(kBlockReadyTimeout);
}

TEST(ArchitectureSmoke, MiniaudioDestructorUnblocksPendingCallback)
{
    std::unique_ptr<iv::MiniaudioLogicalDevice> device;
    try {
        device = std::make_unique<iv::MiniaudioLogicalDevice>(iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 2,
            .max_block_frames = 256,
            .preferred_block_size = 64,
        });
    } catch (std::exception const& e) {
        GTEST_SKIP() << e.what();
    }

    auto request = device->wait_for_block_request();
    ASSERT_FALSE(request.empty());

    auto destroy = std::async(std::launch::async, [&] {
        device.reset();
    });
    ASSERT_EQ(destroy.wait_for(std::chrono::seconds(2)), std::future_status::ready)
        << "MiniaudioLogicalDevice destructor blocked with a pending callback response";
    destroy.get();
}

TEST(ArchitectureSmoke, ExecuteReloadSwitchesAtExecutorBlockBoundaryWithinActiveRequest)
{
    iv::Sample old_value = 0.25f;
    iv::Sample new_value = 0.5f;
    std::atomic<size_t> block_count = 0;
    std::atomic<bool> reload_sent = false;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 512,
            .preferred_block_size = 64,
        }
    );

    iv::GraphBuilder g;
    auto const src = g.node<iv::ValueSource>(&old_value);
    auto const counter = g.node<CountingBlockPassthrough>(CountingBlockPassthrough{ .block_count = &block_count });
    auto const sink = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 0,
    });
    counter(src);
    sink(counter);
    g.outputs();

    auto executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        iv::test::make_audio_device_provider(audio_device).to_builder()
    );

    auto worker = std::async(std::launch::async, [&] {
        executor.execute([&]() -> std::optional<iv::ModuleLoader::LoadedGraph> {
            if (reload_sent.load(std::memory_order_relaxed)) {
                return std::nullopt;
            }
            if (block_count.load(std::memory_order_relaxed) < 1) {
                return std::nullopt;
            }
            reload_sent.store(true, std::memory_order_relaxed);
            return iv::ModuleLoader::LoadedGraph(
                make_constant_audio_graph(&new_value),
                {},
                {},
                "live_reload",
                {},
                1
            );
        });
    });

    audio_device.begin_requested_block(0, 192);
    ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout))
        << "reload execute() did not satisfy the active request";

    auto block = audio_device.output_block().first(192);
    expect_constant_block(block.first(64), old_value);
    expect_constant_block(block.subspan(64, 128), new_value);
    audio_device.finish_requested_block();

    stop_executor(audio_device, executor, worker);
}

TEST(ArchitectureSmoke, OrchestratorBuilderReplaysRegistrationsAcrossBuildAndReturn)
{
    std::array<iv::Sample, 8> a;
    std::array<iv::Sample, 8> b;
    std::array<iv::Sample, 8> c;
    a.fill(0.25f);
    b.fill(0.5f);
    c.fill(1.0f);

    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 8,
            .preferred_block_size = 8,
        }
    );

    iv::OrchestratorBuilder builder;
    builder.audio_device(0, 0).register_sink(0, a);
    builder.audio_device(0, 0).register_sink(0, b);
    builder.add_audio_device(0, iv::LogicalAudioDevice(FakeAudioDeviceBackend{ &audio_device }));

    auto orchestrator = std::move(builder).build();
    audio_device.begin_requested_block(0, 8);
    orchestrator.wait_for_block();
    orchestrator.request_shutdown();
    orchestrator.sync_block(0, 8);
    ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout));
    expect_constant_block(audio_device.output_block().first(8), 0.75f);
    audio_device.finish_requested_block();

    auto rebound = std::move(orchestrator).to_builder();
    rebound.audio_device(0, 0).update_sink(0, a, c);
    rebound.audio_device(0, 0).unregister_sink(0, b);

    auto orchestrator2 = std::move(rebound).build();
    audio_device.begin_requested_block(8, 8);
    orchestrator2.wait_for_block();
    orchestrator2.request_shutdown();
    orchestrator2.sync_block(8, 8);
    ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout));
    expect_constant_block(audio_device.output_block().first(8), 1.0f);
    audio_device.finish_requested_block();
}

TEST(ArchitectureSmoke, BlockRateBufferPreservesOrderAcrossWraparound)
{
    iv::BlockRateBuffer<int> buffer(8);

    auto first_write = buffer.allocate_write(6);
    for (size_t i = 0; i < 6; ++i) {
        first_write[i] = static_cast<int>(i + 1);
    }

    auto first_read = buffer.read_block(4);
    ASSERT_EQ(first_read.size(), 4u);
    EXPECT_EQ(first_read[0], 1);
    EXPECT_EQ(first_read[1], 2);
    EXPECT_EQ(first_read[2], 3);
    EXPECT_EQ(first_read[3], 4);

    auto second_write = buffer.allocate_write(4);
    for (size_t i = 0; i < 4; ++i) {
        second_write[i] = static_cast<int>(i + 7);
    }

    auto second_read = buffer.read_block(6);
    ASSERT_EQ(second_read.size(), 6u);
    EXPECT_EQ(second_read[0], 5);
    EXPECT_EQ(second_read[1], 6);
    EXPECT_EQ(second_read[2], 7);
    EXPECT_EQ(second_read[3], 8);
    EXPECT_EQ(second_read[4], 9);
    EXPECT_EQ(second_read[5], 10);
}

TEST(ArchitectureSmoke, SingleExecutorExecuteSurvivesVaryingRequestSizes)
{
    iv::Sample value = 0.25f;
    auto request_notification = std::make_shared<std::condition_variable>();
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 1,
            .max_block_frames = 4096,
            .preferred_block_size = 256,
        },
        request_notification
    );
    auto executor = make_constant_audio_executor(&value, iv::test::make_audio_device_provider(audio_device));

    auto worker = std::async(std::launch::async, [&] {
        executor.execute();
    });

    std::array<size_t, 8> const request_sizes { 64, 128, 192, 256, 320, 384, 448, 512 };
    size_t request_index = 0;

    for (size_t iteration = 0; iteration < 512; ++iteration) {
        size_t const request_size = request_sizes[iteration % request_sizes.size()];
        audio_device.begin_requested_block(request_index, request_size);

        ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout))
            << "single-executor execute() stalled for request [" << request_index << ", " << request_size << "]";

        audio_device.finish_requested_block();
        request_index += request_size;
    }

    stop_executor(audio_device, executor, worker);
}

TEST(ArchitectureSmoke, StereoExecutorExecuteSurvivesVaryingRequestSizes)
{
    iv::Sample left = 0.25f;
    iv::Sample right = 0.5f;
    auto request_notification = std::make_shared<std::condition_variable>();
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 48000,
            .num_channels = 2,
            .max_block_frames = 4096,
            .preferred_block_size = 256,
        },
        request_notification
    );
    iv::ExecutionTargetRegistry execution_target_registry(
        iv::test::make_audio_device_provider(audio_device)
    );

    iv::GraphBuilder g;
    auto const src_left = g.node<iv::ValueSource>(&left);
    auto const src_right = g.node<iv::ValueSource>(&right);
    auto const sink_left = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 0,
    });
    auto const sink_right = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
        .device_id = 0,
        .channel = 1,
    });
    sink_left(src_left);
    sink_right(src_right);
    g.outputs();

    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        std::move(execution_target_registry).to_builder()
    );

    auto worker = std::async(std::launch::async, [&] {
        executor.execute();
    });

    std::array<size_t, 8> const request_sizes { 64, 128, 192, 256, 320, 384, 448, 512 };
    size_t request_index = 0;

    for (size_t iteration = 0; iteration < 512; ++iteration) {
        size_t const request_size = request_sizes[iteration % request_sizes.size()];
        audio_device.begin_requested_block(request_index, request_size);

        ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout))
            << "stereo execute() stalled for request [" << request_index << ", " << request_size << "]";

        expect_constant_block(audio_device.output_block(0).first(request_size), left);
        expect_constant_block(audio_device.output_block(1).first(request_size), right);
        audio_device.finish_requested_block();
        request_index += request_size;
    }

    stop_executor(audio_device, executor, worker);
}

TEST(ArchitectureSmoke, FileSinkUsesDeviceSampleRateForWavOutput)
{
    iv::Sample value = 0.25f;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 44100,
            .num_channels = 1,
            .max_block_frames = 8,
            .preferred_block_size = 8,
        }
    );

    auto output_path = iv::test::repo_root() / "build" / "architecture_smoke_file_sink.wav";
    std::error_code ec;
    std::filesystem::remove(output_path, ec);

    {
        iv::GraphBuilder g;
        auto const src = g.node<iv::ValueSource>(&value);
        auto const sink = g.node<iv::FileSink>(iv::FileSink{
            .path = output_path,
            .channel = 0,
        });
        sink(src);
        g.outputs();

        iv::ExecutionTargetRegistry execution_target_registry(
            iv::test::make_audio_device_provider(audio_device)
        );
        auto executor = iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            {},
            std::move(execution_target_registry).to_builder()
        );

        executor.device_orchestrator().wait_for_block();
        tick_executor_direct(executor, 0, 8);
        executor.device_orchestrator().request_shutdown();
        executor.device_orchestrator().sync_block(0, 8);
    }

    ASSERT_TRUE(std::filesystem::exists(output_path)) << output_path.string();
    EXPECT_EQ(read_wav_sample_rate(output_path), audio_device.config().sample_rate);

    std::filesystem::remove(output_path, ec);
}

TEST(ArchitectureSmoke, FileSinkPreservesContinuityAcrossVaryingAudioRequests)
{
    iv::Sample value = 0.25f;
    iv::test::FakeAudioDevice audio_device(
        iv::RenderConfig{
            .sample_rate = 44100,
            .num_channels = 1,
            .max_block_frames = 512,
            .preferred_block_size = 64,
        }
    );

    auto output_path = iv::test::repo_root() / "build" / "architecture_smoke_file_sink_continuity.wav";
    std::error_code ec;
    std::filesystem::remove(output_path, ec);

    size_t total_requested_frames = 0;
    {
        iv::GraphBuilder g;
        auto const src = g.node<iv::ValueSource>(&value);
        auto const audio_sink = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
            .device_id = 0,
            .channel = 0,
        });
        auto const file_sink = g.node<iv::FileSink>(iv::FileSink{
            .path = output_path,
            .channel = 0,
        });
        audio_sink(src);
        file_sink(src);
        g.outputs();

        iv::ExecutionTargetRegistry execution_target_registry(
            iv::test::make_audio_device_provider(audio_device)
        );
        auto executor = iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            {},
            std::move(execution_target_registry).to_builder()
        );

        auto worker = start_executor(executor);
        std::array<size_t, 5> const request_sizes { 64, 192, 128, 256, 64 };
        size_t request_index = 0;
        for (size_t request_size : request_sizes) {
            request_block_and_wait(audio_device, executor, request_index, request_size);
            expect_constant_block(audio_device.output_block().first(request_size), value);
            audio_device.finish_requested_block();
            request_index += request_size;
            total_requested_frames += request_size;
        }
        stop_executor(audio_device, executor, worker);
    }

    ASSERT_TRUE(std::filesystem::exists(output_path)) << output_path.string();
    EXPECT_EQ(read_wav_sample_rate(output_path), audio_device.config().sample_rate);

    auto samples = read_wav_pcm16_samples(output_path);
    ASSERT_EQ(samples.size(), total_requested_frames);
    expect_pcm16_constant(samples, value);

    std::filesystem::remove(output_path, ec);
}
