#include "devices/channel_buffer_sink.h"
#include "basic_nodes/buffers.h"
#include "dsl.h"
#include "graph_node.h"
#include "module_test_utils.h"
#include "node_layout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <string_view>

namespace {
    using namespace iv::literals;

    constexpr auto kBlockReadyTimeout = std::chrono::milliseconds(100);
    constexpr auto kAsyncWaitTimeout = std::chrono::milliseconds(50);

    template<typename Ref, typename... Args>
    concept NodeRefInvocable = requires(Ref ref, Args... args)
    {
        ref(args...);
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

    static_assert(iv::details::fixed_input_count_v<UnaryPassthrough> == 1);
    static_assert(NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, iv::SignalRef>);
    static_assert(NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, decltype("in"_P = std::declval<iv::SignalRef>())>);
    static_assert(!NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, iv::SignalRef, iv::SignalRef>);
    static_assert(!NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, decltype("in"_P = std::declval<iv::SignalRef>()), iv::SignalRef>);
    static_assert(!NodeRefInvocable<iv::StructuredNodeRef<UnaryPassthrough>, decltype("in"_P = std::declval<iv::SignalRef>()), decltype("in"_P = std::declval<iv::SignalRef>())>);
    static_assert(NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SignalRef,
        decltype("cutoff"_P = std::declval<iv::SignalRef>()),
        decltype("resonance"_P = std::declval<iv::SignalRef>())
    >);
    static_assert(NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SignalRef,
        iv::SignalRef,
        decltype("resonance"_P = std::declval<iv::SignalRef>())
    >);
    static_assert(!NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        decltype("cutoff"_P = std::declval<iv::SignalRef>()),
        iv::SignalRef
    >);
    static_assert(!NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SignalRef,
        decltype("cutoff"_P = std::declval<iv::SignalRef>()),
        iv::SignalRef
    >);
    static_assert(!NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SignalRef,
        decltype("cutoff"_P = std::declval<iv::SignalRef>()),
        decltype("cutoff"_P = std::declval<iv::SignalRef>())
    >);
    static_assert(!NodeRefInvocable<
        iv::StructuredNodeRef<MixedInputsNode>,
        iv::SignalRef,
        iv::SignalRef,
        iv::SignalRef,
        decltype("resonance"_P = std::declval<iv::SignalRef>())
    >);
    static_assert(ShiftRightInvocable<iv::SignalRef, iv::StructuredNodeRef<UnaryPassthrough>>);
    static_assert(ShiftLeftInvocable<iv::StructuredNodeRef<UnaryPassthrough>, iv::SignalRef>);
    static_assert(ShiftLeftInvocable<decltype("value"_P), iv::StructuredNodeRef<UnaryPassthrough>>);
    static_assert(ShiftRightInvocable<iv::StructuredNodeRef<UnaryPassthrough>, decltype("value"_P)>);
    static_assert(BitNotInvocable<iv::SignalRef>);
    static_assert(BitNotInvocable<iv::StructuredNodeRef<UnaryPassthrough>>);
    static_assert(std::same_as<iv::details::node_ref_for_t<iv::Broadcast>, iv::TypedNodeRef<iv::Broadcast>>);

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
        iv::ExecutionTargetRegistry& execution_target_registry,
        size_t executor_id,
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
            execution_target_registry,
            executor_id
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

    template<typename Executor>
    void request_and_tick(iv::test::FakeAudioDevice& audio_device, Executor& executor, size_t index, size_t block_size)
    {
        audio_device.device().begin_requested_block(index, block_size);
        executor.tick_block(index, block_size);
    }

    template<typename Executor>
    void request_tick_and_wait(iv::test::FakeAudioDevice& audio_device, Executor& executor, size_t index, size_t block_size)
    {
        request_and_tick(audio_device, executor, index, block_size);
        ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout))
            << "requested block [" << index << ", " << block_size << "] did not become ready";
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
        execution_target_registry,
        1
    );

    ASSERT_EQ(executor.max_block_size(), channel.size());
    request_tick_and_wait(audio_device, executor, 0, channel.size());

    auto block = audio_device.output_block();
    std::copy_n(block.begin(), channel.size(), channel.begin());
    audio_device.device().finish_requested_block();

    expect_constant_block(channel, 0.75f);
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
        execution_target_registry,
        1
    );

    request_tick_and_wait(audio_device, executor, 0, 8);

    auto left_block = audio_device.output_block(0);
    auto right_block = audio_device.output_block(1);
    expect_interleaved_channels(left_block, 0.25f, right_block, 0.5f);
    audio_device.device().finish_requested_block();
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
        execution_target_registry,
        1
    );

    audio_device.device().begin_requested_block(8, 4);
    executor.tick_block(4, 4);
    expect_zero_output(audio_device);

    executor.tick_block(8, 4);
    ASSERT_TRUE(audio_device.device().wait_until_block_ready());
    auto block = audio_device.output_block();
    expect_constant_block(block.first(4), 0.75f);
    audio_device.device().finish_requested_block();
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
    auto const passthrough = g.subgraph([](iv::GraphBuilder& nested) {
        auto const input = nested.input();
        nested.outputs(input);
    });
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
        execution_target_registry,
        1
    );

    request_tick_and_wait(audio_device, executor, 0, 8);

    expect_constant_block(audio_device.output_block(), value);
    audio_device.device().finish_requested_block();
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

    auto const piped = src >> stage_a >> stage_b;
    sink(piped);
    g.outputs();

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::NodeExecutor::create(
        iv::TypeErasedNode(g.build()),
        {},
        execution_target_registry,
        1
    );

    request_tick_and_wait(audio_device, executor, 0, 8);

    expect_constant_block(audio_device.output_block(), value);
    audio_device.device().finish_requested_block();
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
        execution_target_registry,
        1
    );

    audio_device.device().begin_requested_block(8, 4);
    executor.tick_block(8, 4);
    ASSERT_TRUE(audio_device.device().wait_until_block_ready());

    auto block = audio_device.output_block();
    expect_constant_block(block.first(4), 0.75f);

    audio_device.device().finish_requested_block();
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

    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    auto executor = make_constant_audio_executor(&value, execution_target_registry, 1);

    for (size_t i = 0; i < iterations; ++i) {
        size_t const index = i * block_size;
        SCOPED_TRACE(::testing::Message() << "iteration=" << i << " block_index=" << index);
        request_tick_and_wait(audio_device, executor, index, block_size);
        expect_constant_block(audio_device.output_block().first(block_size), value);
        audio_device.device().finish_requested_block();
    }
}

TEST(ArchitectureSmoke, DirectoryModuleRetainsModuleRefsAndRuns)
{
    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    auto processor = iv::test::make_executor(
        loader,
        audio_device,
        execution_target_registry,
        1,
        iv::test::test_modules_root() / "noisy_saw_project"
    );

    EXPECT_NE(processor.num_module_refs(), 0u);
    iv::test::run_processor_ticks(processor);
    EXPECT_FALSE(processor.is_shutdown_requested());
}

TEST(ArchitectureSmoke, DirectoryModuleEventArrayBindingsResolve)
{
    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();
    auto loaded = loader.load_root(
        iv::test::test_modules_root() / "noisy_saw_project",
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

TEST(ArchitectureSmoke, DuplicateExecutorIdIsRejected)
{
    iv::Sample a = 0.25f;
    iv::Sample b = 0.5f;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));

    auto executor_a = make_constant_audio_executor(&a, execution_target_registry, 1);
    (void)executor_a;

    expect_failure_contains(
        [&] {
            auto executor_b = make_constant_audio_executor(&b, execution_target_registry, 1);
            (void)executor_b;
        },
        "already registered"
    );
}

TEST(ArchitectureSmoke, TwoExecutorsShareRegistryAndAccumulate)
{
    iv::Sample a = 0.25f;
    iv::Sample b = 0.5f;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));

    auto executor_a = make_constant_audio_executor(&a, execution_target_registry, 1);
    auto executor_b = make_constant_audio_executor(&b, execution_target_registry, 2);

    request_and_tick(audio_device, executor_a, 0, 8);
    EXPECT_FALSE(audio_device.is_block_ready());
    expect_zero_output(audio_device);

    executor_b.tick_block(0, 8);
    ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout));
    expect_constant_block(audio_device.output_block(), 0.75f);
    audio_device.device().finish_requested_block();
}

TEST(ArchitectureSmoke, TwoExecutorsBlockBecomesReadyAfterSecondContribution)
{
    iv::Sample a = 0.25f;
    iv::Sample b = 0.5f;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));

    auto executor_a = make_constant_audio_executor(&a, execution_target_registry, 1);
    auto executor_b = make_constant_audio_executor(&b, execution_target_registry, 2);

    audio_device.device().begin_requested_block(0, 8);
    executor_a.tick_block(0, 8);
    EXPECT_FALSE(audio_device.is_block_ready());

    executor_b.tick_block(0, 8);
    EXPECT_TRUE(audio_device.is_block_ready());

    if (audio_device.is_block_ready()) {
        expect_constant_block(audio_device.output_block(), 0.75f);
        audio_device.device().finish_requested_block();
    }
}

TEST(ArchitectureSmoke, ExecutorTeardownRemovesContribution)
{
    iv::Sample a = 0.25f;
    iv::Sample b = 0.5f;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));

    auto executor_a = make_constant_audio_executor(&a, execution_target_registry, 1);
    {
        auto executor_b = make_constant_audio_executor(&b, execution_target_registry, 2);
        request_and_tick(audio_device, executor_a, 0, 8);
        executor_b.tick_block(0, 8);
        ASSERT_TRUE(audio_device.wait_until_block_ready_for(kBlockReadyTimeout));
        expect_constant_block(audio_device.output_block(), 0.75f);
        audio_device.device().finish_requested_block();
    }

    request_tick_and_wait(audio_device, executor_a, 8, 8);
    expect_constant_block(audio_device.output_block(), 0.25f);
    audio_device.device().finish_requested_block();
}

TEST(ArchitectureSmoke, ReloadReplacesAudioContribution)
{
    iv::Sample a = 0.25f;
    iv::Sample b = 0.5f;
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));

    auto executor = make_constant_audio_executor(&a, execution_target_registry, 1);

    request_tick_and_wait(audio_device, executor, 0, 8);
    expect_constant_block(audio_device.output_block(), 0.25f);
    audio_device.device().finish_requested_block();

    executor.reload(make_constant_audio_graph(&b));

    request_tick_and_wait(audio_device, executor, 8, 8);
    expect_constant_block(audio_device.output_block(), 0.5f);
    audio_device.device().finish_requested_block();
}

TEST(ArchitectureSmoke, RegistryReadyCheckDoesNotBlockWithoutAudioTargets)
{
    iv::test::FakeAudioDevice audio_device({ .num_channels = 1, .max_block_frames = 8 });
    iv::ExecutionTargetRegistry execution_target_registry(iv::test::make_audio_device_provider(audio_device));
    execution_target_registry.register_executor(1);
    execution_target_registry.validate_executor_block_size(1, 8);

    auto wait_result = std::async(std::launch::async, [&] {
        return execution_target_registry.sync_block(1, std::nullopt, 0, 8);
    });

    EXPECT_EQ(wait_result.wait_for(kAsyncWaitTimeout), std::future_status::ready)
        << "sync_block should not wait forever when an executor has no active audio devices";

    execution_target_registry.unregister_executor(1);
    (void)wait_result.wait_for(kBlockReadyTimeout);
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
            iv::test::make_audio_device_provider(audio_device),
            48000
        );
        auto executor = iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            {},
            execution_target_registry,
            1
        );

        executor.tick_block(0, 8);
    }

    ASSERT_TRUE(std::filesystem::exists(output_path)) << output_path.string();
    EXPECT_EQ(read_wav_sample_rate(output_path), audio_device.config().sample_rate);

    std::filesystem::remove(output_path, ec);
}
