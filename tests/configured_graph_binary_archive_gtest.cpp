#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/module/configured_graph_wire.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

struct ReplayableArchiveNode {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("output")};
    }
    void tick(iv::TickSampleContext<ReplayableArchiveNode> const&) const {}
};

struct ArchiveFixture {
    iv::SerializedConfiguredGraph archive;
    using Pass = iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>;
    std::array<iv::details::NodeCompilerRecord, 2> node_types{
        iv::details::node_compiler_record<iv::Constant>,
        iv::details::node_compiler_record<Pass>};

    ArchiveFixture()
    {
        using namespace iv;
        iv::GraphBuilder graph;
        auto const trigger = graph.event_input<"trigger">(iv::EventTypeId::trigger);
        (void)trigger;
        auto const gain = graph.input<"gain">(0.25f);
        auto const source = iv::details::configure_concrete_node<iv::Constant>(
            graph, iv::Sample{0.75f});
        auto const source_handle = source.node_bundle_handle();
        auto const pass = iv::details::configure_concrete_node<Pass>(graph);
        pass(gain);
        pass._annotate_input_source_info(
            PortKind::sample, "", "archive-pass", "/tmp/archive-module.cpp", 10, 18);
        graph.outputs("gain_out"_P = pass, "main"_P = source);

        auto configured = std::move(graph).finish();
        configured.node_bundles.set_registered_node_type_identity(
            source_handle,
            iv::RegisteredNodeTypeIdentity{
                .node_type_id = "iv.test.archive.constant",
                .provider_package_root = "/tmp/iv-test-provider",
            });
        archive = iv::serialize_configured_graph(std::move(configured));
    }

    iv::ConfiguredGraph decode(std::span<std::byte const> bytes) const
    {
        return iv::deserialize_configured_graph(
            bytes, node_types, archive.node_configs);
    }
};

TEST(ConfiguredGraphBinaryArchive, RoundTripsNativeScalarsAndRejectsCorruption)
{
    ArchiveFixture fixture;

    auto decoded = fixture.decode(fixture.archive.bytes);
    std::optional<iv::RegisteredNodeTypeIdentity> registered_identity;
    decoded.node_bundles.for_each_configured_bundle(
        [&](iv::ConfiguredNodeBundleView const& bundle) {
            if (bundle.registered_node_type_identity) {
                ASSERT_FALSE(registered_identity.has_value());
                registered_identity = *bundle.registered_node_type_identity;
            }
        });
    ASSERT_TRUE(registered_identity.has_value());
    EXPECT_EQ(registered_identity->node_type_id, "iv.test.archive.constant");
    EXPECT_EQ(
        registered_identity->provider_package_root,
        "/tmp/iv-test-provider");

    auto const virtual_record = std::ranges::find_if(
        decoded.virtual_nodes.records(),
        [](auto const& record) { return record.source_identity == "archive-pass"; });
    ASSERT_NE(virtual_record, decoded.virtual_nodes.records().end());
    ASSERT_EQ(virtual_record->sample_inputs.size(), 1u);
    ASSERT_EQ(virtual_record->sample_inputs.front().source_infos.size(), 1u);
    EXPECT_EQ(
        virtual_record->sample_inputs.front().source_infos.front().span.file_path,
        "/tmp/archive-module.cpp");
    EXPECT_EQ(
        virtual_record->sample_inputs.front().source_infos.front().span.begin, 10u);
    EXPECT_EQ(
        virtual_record->sample_inputs.front().source_infos.front().span.end, 18u);

    auto const& declared_inputs = decoded.public_ports.inputs(decoded.node_bundles);
    ASSERT_EQ(declared_inputs.size(), 2u);
    EXPECT_FALSE(iv::is_sample(declared_inputs[0]));
    EXPECT_EQ(declared_inputs[0].name, "trigger");
    EXPECT_TRUE(iv::is_sample(declared_inputs[1]));
    EXPECT_EQ(declared_inputs[1].name, "gain");

    auto const& inputs = decoded.public_ports.inputs(decoded.node_bundles);
    auto const& outputs = decoded.public_ports.outputs(decoded.node_bundles);

    ASSERT_EQ(inputs.size(), 2u);
    EXPECT_EQ(inputs[0].name, "trigger");
    ASSERT_FALSE(iv::is_sample(inputs[0]));
    EXPECT_EQ(iv::event_properties(inputs[0]).type, iv::EventTypeId::trigger);
    EXPECT_EQ(inputs[1].name, "gain");
    ASSERT_TRUE(iv::is_sample(inputs[1]));
    auto const& gain_properties = iv::sample_properties(inputs[1]);
    EXPECT_FLOAT_EQ(gain_properties.default_value, 0.25f);
    EXPECT_TRUE(std::isinf(gain_properties.min.value));
    EXPECT_TRUE(std::isinf(gain_properties.max.value));
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_EQ(outputs[0].name, "gain_out");
    EXPECT_EQ(outputs[1].name, "main");

    auto corrupt = fixture.archive.bytes;
    corrupt.front() = std::byte{};
    EXPECT_THROW(fixture.decode(corrupt), std::runtime_error);

    auto previous_version = fixture.archive.bytes;
    auto const obsolete_version = std::uint32_t{7};
    std::memcpy(previous_version.data() + sizeof(std::uint32_t),
        &obsolete_version, sizeof(obsolete_version));
    EXPECT_THROW(fixture.decode(previous_version), std::runtime_error);

    auto truncated = fixture.archive.bytes;
    truncated.pop_back();
    EXPECT_THROW(fixture.decode(truncated), std::runtime_error);

    auto trailing = fixture.archive.bytes;
    trailing.push_back(std::byte{});
    EXPECT_THROW(fixture.decode(trailing), std::runtime_error);
}

TEST(ConfiguredGraphBinaryArchive, PreservesIntrinsicReplayMetadata)
{
    using namespace iv;
    GraphBuilder graph;
    auto const node = details::configure_concrete_node<ReplayableArchiveNode>(graph);
    graph.outputs("replay"_P = node);
    auto const archive = serialize_configured_graph(std::move(graph).finish());
    std::array const compiler_records{
        details::node_compiler_record<ReplayableArchiveNode>};
    auto const decoded = deserialize_configured_graph(
        archive.bytes, compiler_records, archive.node_configs);

    std::size_t replayable_nodes = 0;
    decoded.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            if (view.kind != ConfiguredNodeBundleKind::concrete) return;
            EXPECT_TRUE(view.intrinsically_replayable);
            ++replayable_nodes;
        });
    EXPECT_EQ(replayable_nodes, 1u);
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsConnectionLocalDetachMetadata)
{
    iv::ConfiguredSampleConnection sample{
        .source_type = iv::ChannelTypeId::mono,
        .source_channels = {
            iv::SampleOutputChannelId{.bundle = 4, .port = 1, .channel = 0},
        },
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {
            iv::SampleInputChannelId{.bundle = 6, .port = 0, .channel = 0},
        },
        .detach = iv::ConfiguredSampleConnectionDetach{
            .loop_extra_latency = 9,
            .initial_value_override = iv::Sample{-0.375f},
        },
    };
    iv::ConfiguredEventConnection event{
        .source_type = iv::EventTypeId::trigger,
        .sources = {iv::EventOutputPortId{.bundle = 7, .port = 2}},
        .target_type = iv::EventTypeId::trigger,
        .targets = {iv::EventInputPortId{.bundle = 8, .port = 3}},
        .detach = iv::ConfiguredEventConnectionDetach{
            .loop_extra_latency = 11,
        },
    };
    iv::ConfiguredGraph configured;
    configured.connections = iv::GraphBuilderConnections::from_configured_connections(
        std::array{sample}, std::array{event});

    auto const archive = iv::serialize_configured_graph(configured);
    auto const decoded = iv::deserialize_configured_graph(
        archive.bytes,
        std::span<iv::details::NodeCompilerRecord const>{},
        archive.node_configs);

    auto const samples = decoded.connections.configured_sample_connections();
    ASSERT_EQ(samples.size(), 1u);
    ASSERT_TRUE(samples[0].detach.has_value());
    EXPECT_EQ(samples[0].detach->loop_extra_latency, 9u);
    ASSERT_TRUE(samples[0].detach->initial_value_override.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(*samples[0].detach->initial_value_override), -0.375f);

    auto const events = decoded.connections.configured_event_connections();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(events[0].detach.has_value());
    EXPECT_EQ(events[0].detach->loop_extra_latency, 11u);
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsOrthogonalPortAccessConfigs)
{
    iv::SampleInputConfig const realtime_input {
        .name = "realtime-input",
        .channel_layout = {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        },
        .access = iv::SequentialInputConfig{.history = 7},
        .neutral_value = -0.125f,
        .default_value = 0.25f,
    };
    iv::SampleInputConfig const random_access_input {
        .name = "background-input",
        .access = iv::RandomAccessInputConfig{},
    };
    iv::SampleOutputConfig const realtime_output {
        .name = "realtime-output",
        .production = iv::TickOutputConfig{.history = 11, .latency = 3},
        .retention = iv::OutputRetention::persisted,
    };
    iv::SampleOutputConfig const tock_output {
        .name = "background-output",
        .production = iv::TockOutputConfig{},
        .retention = iv::OutputRetention::ephemeral,
    };
    iv::EventInputConfig const sequential_event_input {
        .name = "realtime-event-input",
        .type = iv::EventTypeId::trigger,
        .access = iv::SequentialInputConfig{.history = 5},
    };
    iv::EventInputConfig const random_access_event_input {
        .name = "background-event-input",
        .type = iv::EventTypeId::trigger,
        .access = iv::RandomAccessInputConfig{},
    };
    iv::EventOutputConfig const tick_event_output {
        .name = "realtime-event-output",
        .type = iv::EventTypeId::midi,
        .max_events_per_index = 0.24,
        .production = iv::TickOutputConfig{.history = 13, .latency = 2},
        .retention = iv::OutputRetention::ephemeral,
    };
    iv::EventOutputConfig const tock_event_output {
        .name = "background-event-output",
        .type = iv::EventTypeId::midi,
        .production = iv::TockOutputConfig{},
        .retention = iv::OutputRetention::persisted,
    };

    iv::binary_wire_details::Writer writer;
    iv::binary_wire_details::write_input(writer, realtime_input);
    iv::binary_wire_details::write_input(writer, random_access_input);
    iv::binary_wire_details::write_output(writer, realtime_output);
    iv::binary_wire_details::write_output(writer, tock_output);
    iv::binary_wire_details::write_event_input(writer, sequential_event_input);
    iv::binary_wire_details::write_event_input(writer, random_access_event_input);
    iv::binary_wire_details::write_event_output(writer, tick_event_output);
    iv::binary_wire_details::write_event_output(writer, tock_event_output);
    auto const bytes = std::move(writer).take();

    iv::binary_wire_details::Reader reader(bytes);
    auto const decoded_realtime_input = iv::binary_wire_details::read_input(reader);
    auto const decoded_background_input = iv::binary_wire_details::read_input(reader);
    auto const decoded_realtime_output = iv::binary_wire_details::read_output(reader);
    auto const decoded_background_output = iv::binary_wire_details::read_output(reader);
    auto const decoded_realtime_event_input = iv::binary_wire_details::read_event_input(reader);
    auto const decoded_background_event_input = iv::binary_wire_details::read_event_input(reader);
    auto const decoded_realtime_event_output = iv::binary_wire_details::read_event_output(reader);
    auto const decoded_background_event_output = iv::binary_wire_details::read_event_output(reader);
    reader.finish();

    EXPECT_EQ(decoded_realtime_input.name, realtime_input.name);
    EXPECT_EQ(decoded_realtime_input.channel_layout, realtime_input.channel_layout);
    EXPECT_FALSE(iv::is_random_access(decoded_realtime_input));
    EXPECT_EQ(iv::port_history(decoded_realtime_input), 7u);
    EXPECT_FLOAT_EQ(static_cast<float>(decoded_realtime_input.neutral_value), -0.125f);
    EXPECT_FLOAT_EQ(static_cast<float>(decoded_realtime_input.default_value), 0.25f);
    EXPECT_TRUE(iv::is_random_access(decoded_background_input));
    EXPECT_FALSE(iv::is_sequential(decoded_background_input.access));

    EXPECT_EQ(decoded_realtime_output.name, realtime_output.name);
    EXPECT_FALSE(iv::is_tock(decoded_realtime_output));
    EXPECT_EQ(iv::port_history(decoded_realtime_output), 11u);
    EXPECT_EQ(iv::tick_latency(decoded_realtime_output), 3u);
    EXPECT_TRUE(iv::is_persisted(decoded_realtime_output));
    EXPECT_TRUE(iv::is_tock(decoded_background_output));
    EXPECT_FALSE(iv::is_tick(decoded_background_output.production));
    EXPECT_FALSE(iv::is_persisted(decoded_background_output));

    EXPECT_EQ(decoded_realtime_event_input.type, sequential_event_input.type);
    EXPECT_EQ(iv::port_history(decoded_realtime_event_input), 5u);
    EXPECT_TRUE(iv::is_random_access(decoded_background_event_input));

    EXPECT_EQ(decoded_realtime_event_output.type, tick_event_output.type);
    EXPECT_DOUBLE_EQ(decoded_realtime_event_output.max_events_per_index, 0.24);
    EXPECT_EQ(iv::port_history(decoded_realtime_event_output), 13u);
    EXPECT_EQ(iv::tick_latency(decoded_realtime_event_output), 2u);
    EXPECT_FALSE(iv::is_persisted(decoded_realtime_event_output));
    EXPECT_TRUE(iv::is_tock(decoded_background_event_output));
    EXPECT_TRUE(iv::is_persisted(decoded_background_event_output));
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsEveryOutputAccessRetentionMode)
{
    std::array const outputs {
        iv::SampleOutputConfig{
            .name = "realtime-ephemeral",
            .production = iv::TickOutputConfig{},
            .retention = iv::OutputRetention::ephemeral,
        },
        iv::SampleOutputConfig{
            .name = "realtime-persisted",
            .production = iv::TickOutputConfig{},
            .retention = iv::OutputRetention::persisted,
        },
        iv::SampleOutputConfig{
            .name = "background-ephemeral",
            .production = iv::TockOutputConfig{},
            .retention = iv::OutputRetention::ephemeral,
        },
        iv::SampleOutputConfig{
            .name = "background-persisted",
            .production = iv::TockOutputConfig{},
            .retention = iv::OutputRetention::persisted,
        },
    };
    iv::binary_wire_details::Writer writer;
    for (auto const& output : outputs) {
        iv::binary_wire_details::write_output(writer, output);
    }

    auto const bytes = std::move(writer).take();
    iv::binary_wire_details::Reader reader(bytes);
    for (auto const& expected : outputs) {
        auto const output = iv::binary_wire_details::read_output(reader);
        EXPECT_EQ(output.name, expected.name);
        EXPECT_EQ(output.production, expected.production);
        EXPECT_EQ(output.retention, expected.retention);
    }
    reader.finish();
}

} // namespace
