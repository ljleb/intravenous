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
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::enabled;
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>({"input", "Input"})};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"output", "Output"}, iv::OutputProduction::sequential)};
    }
    void tick(iv::TickSampleContext<ReplayableArchiveNode> const&) const {}
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<ReplayableArchiveNode>& ctx) const
    {
        ctx.output<"output">().publish(ctx.input<"input">().coverage());
    }
};

struct ConstrainedArchiveNode {
    static constexpr auto domain = iv::continuous_value_types<
        iv::Sample,
        iv::FractionalGlobalIndex>;

    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample, iv::FractionalGlobalIndex>(
                "input", "Fréq", "Fréquence audio", iv::InputAccess::sequential),
            iv::event_input({"event_input", "Event input"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample, iv::FractionalGlobalIndex>(
                {"output", "Output"}, iv::OutputProduction::sequential),
            iv::event_output("event_output", "Events", "Trigger events", iv::EventTypeId::trigger, iv::OutputProduction::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<ConstrainedArchiveNode>& context) const
    {
        context.output<"output">().value_type()
            = context.input<"input">().value_type();
        context.output<"output">().pace()
            = context.input<"input">().pace();
        context.input<"input">().pace() = 512;
        context.input<"input">().pace().absolute() = 2;
        context.input<"input">().history() = 7;
        context.output<"output">().history() = 11;
        context.output<"output">().latency() = 3;
        context.input<"event_input">().history() = 5;
        context.output<"event_output">().history() = 13;
        context.output<"event_output">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<ConstrainedArchiveNode> const&) const {}
};

struct ArchiveFixture {
    iv::SerializedConfiguredGraph archive;
    using Pass = iv::SumN<iv::mono, iv::SampleStreamLayout::planar, 1>;
    std::array<iv::details::NodeCompilerRecord, 2> node_types{
        iv::details::node_compiler_record<iv::Constant>,
        iv::details::node_compiler_record<Pass>};

    ArchiveFixture()
    {
        using namespace iv;
        iv::GraphBuilder graph;
        auto const trigger = iv::programmatic::declare_event_input(graph, "trigger", {"Trigger"}, iv::EventTypeId::trigger);
        (void)trigger;
        auto const gain = iv::programmatic::declare_input(graph, "gain", {"Gain"}, {.default_value = 0.25f});
        auto const source = iv::details::configure_concrete_node<iv::Constant>(
            graph, iv::Sample{0.75f});
        source._annotate_source_info("archive-source", "/tmp/archive-module.cpp", 1, 2);
        auto const alias = source;
        alias._annotate_source_info("archive-source-alias", "/tmp/archive-module.cpp",
            3, 4, "alias", 2, true, true);
        auto const projection = alias[std::size_t{0}];
        projection._annotate_source_info("archive-port", "/tmp/archive-module.cpp", 5, 6);
        auto const source_handle = source.node_bundle_handle();
        auto const pass = iv::details::configure_concrete_node<Pass>(graph);
        pass._annotate_source_info("archive-pass", "/tmp/archive-module.cpp", 7, 9,
            "pass", 3, true);
        auto const pass_alias = pass;
        pass_alias._annotate_source_info("archive-pass-alias", "/tmp/archive-module.cpp", 10, 12,
            "pass_alias", 4, true);
        pass_alias(gain);
        pass._annotate_input_source_info(
            PortKind::sample, "", "archive-pass", "/tmp/archive-module.cpp", 10, 18);
        graph.outputs("gain_out"_P.with_display_name("Gain out") = pass, "main"_P.with_display_name("Main") = projection);

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

    auto const& source = decoded.symbolic_nodes.records().front();
    ASSERT_EQ(source.source_identity, "archive-source");
    ASSERT_EQ(source.node_bundle_handles.size(), 1u);
    auto const occurrences = decoded.node_bundles.bundle(source.node_bundle_handles.front())
        .node_ref_occurrences();
    ASSERT_EQ(occurrences.size(), 2u);
    EXPECT_FALSE(occurrences[0].upstream.has_value());
    EXPECT_EQ(occurrences[1].upstream, 0u);
    EXPECT_TRUE(source.has_lane());
    auto const& alias = decoded.symbolic_nodes.record(occurrences[1].symbolic_node_handle);
    EXPECT_EQ(alias.source_identity, "archive-source-alias");
    EXPECT_FALSE(alias.has_lane());
    auto const& records = decoded.symbolic_nodes.records();
    auto const projection = std::ranges::find(records, "archive-port", &iv::SymbolicNodeRecord::source_identity);
    ASSERT_NE(projection, records.end());
    ASSERT_EQ(projection->sample_output_routes.size(), 1u);
    ASSERT_EQ(projection->sample_output_routes[0].size(), 1u);
    auto const route = projection->sample_output_routes[0][0];
    EXPECT_EQ(route.node_ref_occurrence, 1u);
    auto const main = decoded.connections.configured_sample_connections().back().source_channels.front();
    EXPECT_EQ(main.node_ref_occurrence, route.node_ref_occurrence);
    EXPECT_EQ(main.channel, route.channel);

    auto const& sample_connections = decoded.connections.configured_sample_connections();
    auto const& pass_input = sample_connections.front();
    EXPECT_TRUE(pass_input.source_owned);
    ASSERT_TRUE(pass_input.target_symbolic_node);
    auto const& pass_alias = decoded.symbolic_nodes.record(*pass_input.target_symbolic_node);
    EXPECT_EQ(pass_alias.source_identity, "archive-pass-alias");
    ASSERT_EQ(pass_alias.sample_inputs.size(), 1u);
    EXPECT_EQ(pass_input.target_channels, pass_alias.sample_inputs.front().channels);
    EXPECT_FALSE(sample_connections.back().target_symbolic_node);

    auto const symbolic_record = std::ranges::find_if(
        decoded.symbolic_nodes.records(),
        [](auto const& record) { return record.source_identity == "archive-pass"; });
    ASSERT_NE(symbolic_record, decoded.symbolic_nodes.records().end());
    ASSERT_EQ(symbolic_record->sample_inputs.size(), 1u);
    ASSERT_EQ(symbolic_record->sample_inputs.front().source_infos.size(), 1u);
    EXPECT_EQ(
        symbolic_record->sample_inputs.front().source_infos.front().span.file_path,
        "/tmp/archive-module.cpp");
    EXPECT_EQ(
        symbolic_record->sample_inputs.front().source_infos.front().span.begin, 10u);
    EXPECT_EQ(
        symbolic_record->sample_inputs.front().source_infos.front().span.end, 18u);

    auto const& declared_inputs = decoded.public_ports.inputs(decoded.node_bundles);
    ASSERT_EQ(declared_inputs.size(), 2u);
    EXPECT_FALSE(iv::is_continuous(declared_inputs[0]));
    EXPECT_EQ(declared_inputs[0].name, "trigger");
    EXPECT_TRUE(iv::is_continuous(declared_inputs[1]));
    EXPECT_EQ(declared_inputs[1].name, "gain");

    auto const& inputs = decoded.public_ports.inputs(decoded.node_bundles);
    auto const& outputs = decoded.public_ports.outputs(decoded.node_bundles);

    ASSERT_EQ(inputs.size(), 2u);
    EXPECT_EQ(inputs[0].name, "trigger");
    ASSERT_FALSE(iv::is_continuous(inputs[0]));
    EXPECT_EQ(iv::event_properties(inputs[0]).type, iv::EventTypeId::trigger);
    EXPECT_EQ(inputs[1].name, "gain");
    ASSERT_TRUE(iv::is_continuous(inputs[1]));
    auto const& gain_properties = iv::continuous_properties(inputs[1]);
    auto const& gain_sequential =
        iv::sequential_input_metadata<iv::Sample>(
            iv::sequential_input_properties(gain_properties));
    EXPECT_FLOAT_EQ(gain_sequential.default_value, 0.25f);
    EXPECT_TRUE(std::isinf(gain_sequential.min.value));
    EXPECT_TRUE(std::isinf(gain_sequential.max.value));
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_EQ(outputs[0].name, "gain_out");
    EXPECT_EQ(outputs[1].name, "main");

    auto corrupt = fixture.archive.bytes;
    corrupt.front() = std::byte{};
    EXPECT_THROW(fixture.decode(corrupt), std::runtime_error);

    auto previous_version = fixture.archive.bytes;
    auto const obsolete_version = std::uint32_t{5};
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

TEST(ConfiguredGraphBinaryArchive, PreservesNamedTiledChildMetadata)
{
    using namespace iv;
    using Pass = SumN<mono, SampleStreamLayout::planar, 1>;
    GraphBuilder graph;
    auto tiled = details::configure_concrete_tiled_node<Pass, stereo>(graph);
    graph.outputs("main"_P.with_display_name("Main") = tiled);
    auto archive = serialize_configured_graph(std::move(graph).finish());
    std::array node_types{details::node_compiler_record<Pass>};
    auto decoded = deserialize_configured_graph(
        archive.bytes, node_types, archive.node_configs);

    bool found = false;
    decoded.node_bundles.for_each_configured_bundle(
        [&](iv::ConfiguredNodeBundleView const& bundle) {
            if (bundle.kind != ConfiguredNodeBundleKind::tiled) return;
            found = true;
            EXPECT_EQ(bundle.tiled_channel_type, ChannelTypeId::stereo);
            ASSERT_EQ(bundle.tiled_members.size(), 2u);
        });
    EXPECT_TRUE(found);
}

TEST(ConfiguredGraphBinaryArchive, PreservesReplayEligibilityAndOutputCoverage)
{
    using namespace iv;
    GraphBuilder graph;
    auto const node = details::configure_concrete_node<ReplayableArchiveNode>(graph);
    graph.outputs("replay"_P.with_display_name("Replay") = node);
    auto const archive = serialize_configured_graph(std::move(graph).finish());
    std::array const compiler_records{
        details::node_compiler_record<ReplayableArchiveNode>};
    auto const decoded = deserialize_configured_graph(
        archive.bytes, compiler_records, archive.node_configs);

    std::size_t replayable_nodes = 0;
    decoded.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            if (view.kind != ConfiguredNodeBundleKind::concrete) return;
            EXPECT_EQ(view.replay_mode, iv::TickReplayMode::enabled);
            ASSERT_EQ(view.tick_output_coverage.size(), 1u);
            auto const& rule = view.tick_output_coverage.front();
            EXPECT_EQ(rule.kind, PortKind::sample);
            EXPECT_EQ(rule.output, 0u);
            EXPECT_EQ(rule.terms, (std::vector{details::TickCoverageTerm{}}));
            ++replayable_nodes;
        });
    EXPECT_EQ(replayable_nodes, 1u);
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsConnectionLocalDetachMetadata)
{
    iv::ConfiguredSampleConnection sample{
        .source_type = iv::ChannelTypeId::stereo,
        .source_channels = {
            iv::ConfiguredSampleConnectionSource{
                .channel = {
                    .bundle = 4,
                    .port = 1,
                    .channel = 0,
                },
                .detach = iv::ConfiguredSampleConnectionDetach{
                    .loop_extra_latency = 9,
                    .initial_value_override = iv::Sample{-0.375f},
                },
            },
            iv::ConfiguredSampleConnectionSource{
                .channel = {
                    .bundle = 5,
                    .port = 2,
                    .channel = 0,
                },
            },
        },
        .target_type = iv::ChannelTypeId::mono,
        .target_channels = {
            iv::SampleInputChannelId{.bundle = 6, .port = 0, .channel = 0},
        },
    };
    iv::ConfiguredEventConnection event{
        .source_type = iv::EventTypeId::trigger,
        .sources = {
            iv::ConfiguredEventConnectionSource{
                .port = {.bundle = 7, .port = 2},
                .detach = iv::ConfiguredEventConnectionDetach{
                    .loop_extra_latency = 11,
                },
            },
        },
        .target_type = iv::EventTypeId::trigger,
        .targets = {iv::EventInputPortId{.bundle = 8, .port = 3}},
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
    ASSERT_EQ(samples[0].source_channels.size(), 2u);
    ASSERT_TRUE(samples[0].source_channels[0].detach.has_value());
    EXPECT_EQ(samples[0].source_channels[0].detach->loop_extra_latency, 9u);
    ASSERT_TRUE(
        samples[0].source_channels[0].detach->initial_value_override.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(
            *samples[0].source_channels[0].detach->initial_value_override),
        -0.375f);
    EXPECT_FALSE(samples[0].source_channels[1].detach.has_value());

    auto const events = decoded.connections.configured_event_connections();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].sources.size(), 1u);
    EXPECT_EQ(
        events[0].sources[0].port,
        (iv::EventOutputPortId{.bundle = 7, .port = 2}));
    ASSERT_TRUE(events[0].sources[0].detach.has_value());
    EXPECT_EQ(events[0].sources[0].detach->loop_extra_latency, 11u);
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsOrthogonalPortAccessConfigs)
{
    auto const fractional_default = iv::FractionalGlobalIndex::from_parts(
        iv::GlobalIndex::from_signed(-3), 0x123456789abcdef0ULL);
    auto const fractional_neutral = iv::FractionalGlobalIndex::from_parts(
        iv::GlobalIndex::from_signed(7), 0xfedcba9876543210ULL);
    auto const realtime_input = iv::materialize_continuous_config(
        iv::erase_port_config(iv::input<iv::Sample, iv::FractionalGlobalIndex>(
            {"realtime_input", "Realtime input"},
            iv::InputAccess::sequential,
            {
                .channel_layout = {
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                },
                .default_value = 0.25f,
                .neutral_value = -0.125f,
            },
            {
                .default_value = fractional_default,
                .neutral_value = fractional_neutral,
            })));
    auto const random_access_input = iv::materialize_continuous_config(
        iv::erase_port_config(iv::input<iv::Sample>(
            {"background_input", "Background input"}, iv::InputAccess::coverage)));
    auto const realtime_output = iv::materialize_continuous_config(
        iv::erase_port_config(iv::output<iv::FFTBlock>(
            {"realtime_output", "Realtime output"},
            iv::OutputProduction::sequential,
            iv::OutputRealization::buffered)));
    auto const tock_output = iv::materialize_continuous_config(
        iv::erase_port_config(iv::output<iv::Sample>(
            {"background_output", "Background output"}, iv::OutputProduction::coverage)));
    iv::EventInputConfig const sequential_event_input {
        .name = "realtime-event-input",
        .type = iv::EventTypeId::trigger,
        .access = iv::InputAccessKind::sequential,
    };
    iv::EventInputConfig const random_access_event_input {
        .name = "background-event-input",
        .type = iv::EventTypeId::trigger,
        .access = iv::InputAccessKind::coverage,
    };
    iv::EventOutputConfig const tick_event_output {
        .name = "realtime-event-output",
        .type = iv::EventTypeId::midi,
        .max_events_per_index = 0.24,
        .production = iv::OutputProductionKind::sequential,
        .realization = iv::OutputRealization::computed,
    };
    iv::EventOutputConfig const tock_event_output {
        .name = "background-event-output",
        .type = iv::EventTypeId::midi,
        .production = iv::OutputProductionKind::coverage,
        .realization = iv::OutputRealization::buffered,
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
    EXPECT_EQ(
        iv::continuous_value_type_domain(decoded_realtime_input),
        iv::continuous_value_type_domain(realtime_input));
    EXPECT_EQ(decoded_realtime_input.channel_layout, realtime_input.channel_layout);
    EXPECT_FALSE(iv::is_random_access(decoded_realtime_input));
    auto const& decoded_realtime_properties =
        iv::sequential_input_metadata<iv::Sample>(
            iv::sequential_input_properties(decoded_realtime_input));
    EXPECT_FLOAT_EQ(
        static_cast<float>(decoded_realtime_properties.neutral_value), -0.125f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(decoded_realtime_properties.default_value), 0.25f);
    auto const& decoded_fractional_properties =
        iv::sequential_input_metadata<iv::FractionalGlobalIndex>(
            iv::sequential_input_properties(decoded_realtime_input));
    EXPECT_EQ(
        decoded_fractional_properties.default_value, fractional_default);
    EXPECT_EQ(
        decoded_fractional_properties.neutral_value, fractional_neutral);
    EXPECT_TRUE(iv::is_random_access(decoded_background_input));
    EXPECT_FALSE(iv::is_sequential(decoded_background_input));
    EXPECT_TRUE(std::holds_alternative<
        iv::CoverageContinuousInputProperties>(
            decoded_background_input.access));

    EXPECT_EQ(decoded_realtime_output.name, realtime_output.name);
    EXPECT_EQ(
        iv::continuous_value_type_domain(decoded_realtime_output),
        iv::continuous_value_type_domain(realtime_output));
    EXPECT_FALSE(iv::is_random_access_output(decoded_realtime_output));
    EXPECT_TRUE(iv::is_buffered(decoded_realtime_output));
    EXPECT_TRUE(iv::is_random_access_output(decoded_background_output));
    EXPECT_FALSE(iv::is_sequential_output(decoded_background_output));
    EXPECT_TRUE(std::holds_alternative<
        iv::CoverageContinuousOutputProperties>(
            decoded_background_output.production));
    EXPECT_FALSE(iv::is_buffered(decoded_background_output));

    EXPECT_EQ(decoded_realtime_event_input.type, sequential_event_input.type);
    EXPECT_TRUE(iv::is_random_access(decoded_background_event_input));

    EXPECT_EQ(decoded_realtime_event_output.type, tick_event_output.type);
    EXPECT_DOUBLE_EQ(decoded_realtime_event_output.max_events_per_index, 0.24);
    EXPECT_FALSE(iv::is_buffered(decoded_realtime_event_output));
    EXPECT_TRUE(iv::is_random_access_output(decoded_background_event_output));
    EXPECT_TRUE(iv::is_buffered(decoded_background_event_output));
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsRandomAccessContentCapabilities)
{
    for (auto const capability : {iv::InputContentAccess::windowed, iv::InputContentAccess::complete_buffer}) {
        auto const sample = iv::materialize_continuous_config(iv::erase_port_config(
            iv::input<iv::Sample>({"in", "In"}, iv::InputAccess::coverage, capability)));
        auto const event = iv::materialize_event_config(iv::erase_port_config(
            iv::event_input({"events", "Events"}, iv::EventTypeId::trigger, iv::InputAccess::region, capability)));
        iv::binary_wire_details::Writer writer;
        iv::binary_wire_details::write_input(writer, sample);
        iv::binary_wire_details::write_event_input(writer, event);
        auto const bytes = std::move(writer).take();
        iv::binary_wire_details::Reader reader{bytes};
        EXPECT_EQ(iv::binary_wire_details::read_input(reader).content_access, capability);
        EXPECT_EQ(iv::binary_wire_details::read_event_input(reader).content_access, capability);
        reader.finish();
    }
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsTimestampOnlyEventInput)
{
    auto const input = iv::materialize_event_config(iv::erase_port_config(
        iv::event_timestamp_input({"marks", "Marks"})));
    ASSERT_TRUE(input.timestamp_only);
    ASSERT_EQ(input.type, iv::EventTypeId::trigger);
    ASSERT_EQ(input.access, iv::InputAccessKind::coverage);

    iv::binary_wire_details::Writer writer;
    iv::binary_wire_details::write_input_config(writer, iv::make_input_config(input));
    auto const bytes = std::move(writer).take();
    iv::binary_wire_details::Reader reader{bytes};
    EXPECT_EQ(iv::materialize_event_config(
        iv::binary_wire_details::read_input_config(reader)), input);
    reader.finish();
}

TEST(ConfiguredGraphBinaryArchive, RejectsRepeatedContinuousMetadataType)
{
    auto config = iv::materialize_continuous_config(
        iv::erase_port_config(iv::input<iv::Sample, iv::FractionalGlobalIndex>(
            {"input", "Input"}, iv::InputAccess::sequential)));
    auto& metadata = std::get<iv::SequentialContinuousInputProperties>(
        config.access).value_metadata;
    metadata.push_back(metadata.front());

    iv::binary_wire_details::Writer writer;
    EXPECT_THROW(
        iv::binary_wire_details::write_input(writer, config),
        std::runtime_error);
}

TEST(ConfiguredGraphBinaryArchive, RejectsEmptyContinuousMetadataDomain)
{
    auto config = iv::materialize_continuous_config(
        iv::erase_port_config(iv::input<iv::Sample>(
            {"input", "Input"}, iv::InputAccess::sequential)));
    std::get<iv::SequentialContinuousInputProperties>(
        config.access).value_metadata.clear();

    iv::binary_wire_details::Writer writer;
    EXPECT_THROW(
        iv::binary_wire_details::write_input(writer, config),
        std::runtime_error);
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsAuthoredPortConstraints)
{
    using namespace iv;
    GraphBuilder graph;
    auto const node =
        details::configure_concrete_node<ConstrainedArchiveNode>(graph);
    node._annotate_source_info("archive-labels", "/tmp/archive-module.cpp", 1, 2, "labels");
    graph.outputs("output"_P.with_display_name("Output") = node);

    auto const archive = serialize_configured_graph(std::move(graph).finish());
    std::array const compiler_records{
        details::node_compiler_record<ConstrainedArchiveNode>};
    auto const decoded = deserialize_configured_graph(
        archive.bytes, compiler_records, archive.node_configs);
    auto const& symbols = decoded.symbolic_nodes.records();
    auto const symbol = std::ranges::find(symbols, "archive-labels", &SymbolicNodeRecord::source_identity);
    ASSERT_NE(symbol, symbols.end());
    ASSERT_EQ(symbol->sample_inputs.size(), 1u);
    EXPECT_EQ(symbol->sample_inputs[0].display_name, "Fréquence audio");
    EXPECT_EQ(symbol->sample_inputs[0].short_display_name, "Fréq");
    ASSERT_EQ(symbol->event_outputs.size(), 1u);
    EXPECT_EQ(symbol->event_outputs[0].display_name, "Trigger events");
    EXPECT_EQ(symbol->event_outputs[0].short_display_name, "Events");

    std::size_t constrained_nodes = 0;
    decoded.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            if (view.kind != ConfiguredNodeBundleKind::concrete) return;
            ASSERT_NE(view.ports, nullptr);
            ASSERT_NE(view.port_constraints, nullptr);
            auto const inputs = view.ports->sample_inputs();
            auto const outputs = view.ports->sample_outputs();
            auto const event_inputs = view.ports->event_inputs();
            auto const event_outputs = view.ports->event_outputs();
            ASSERT_EQ(inputs.size(), 1u);
            ASSERT_EQ(outputs.size(), 1u);
            ASSERT_EQ(event_inputs.size(), 1u);
            ASSERT_EQ(event_outputs.size(), 1u);
            EXPECT_EQ(inputs.front().name, "input");
            EXPECT_EQ(inputs.front().display_name, "Fréquence audio");
            EXPECT_EQ(inputs.front().short_display_name, "Fréq");
            EXPECT_EQ(event_outputs.front().display_name, "Trigger events");
            EXPECT_EQ(event_outputs.front().short_display_name, "Events");
            EXPECT_EQ(
                continuous_value_type_domain(inputs.front()),
                ConstrainedArchiveNode::domain);
            EXPECT_EQ(
                continuous_value_type_domain(outputs.front()),
                ConstrainedArchiveNode::domain);
            ASSERT_EQ(
                view.port_constraints->value_type_equalities.size(), 1u);
            EXPECT_EQ(
                view.port_constraints->value_type_equalities.front(),
                (ContinuousValueTypeEqualityConstraint{
                    .first = {
                        .direction = PortConstraintDirection::output,
                        .kind = PortKind::sample,
                        .port_index = 0,
                    },
                    .second = {
                        .direction = PortConstraintDirection::input,
                        .kind = PortKind::sample,
                        .port_index = 0,
                    },
                }));
            ASSERT_EQ(view.port_constraints->pace_equalities.size(), 1u);
            ASSERT_EQ(view.port_constraints->pace_anchors.size(), 1u);
            EXPECT_EQ(
                view.port_constraints->pace_anchors.front().value.exponent(),
                9);
            ASSERT_EQ(
                view.port_constraints->absolute_pace_anchors.size(), 1u);
            EXPECT_EQ(
                view.port_constraints->absolute_pace_anchors.front()
                    .value.exponent(),
                1);
            ASSERT_EQ(view.port_constraints->history_anchors.size(), 4u);
            EXPECT_EQ(
                view.port_constraints->history_anchors,
                (std::vector<PortHistoryAnchorConstraint>{
                    {{PortConstraintDirection::input, PortKind::sample, 0}, 7},
                    {{PortConstraintDirection::output, PortKind::sample, 0}, 11},
                    {{PortConstraintDirection::input, PortKind::event, 0}, 5},
                    {{PortConstraintDirection::output, PortKind::event, 0}, 13},
                }));
            ASSERT_EQ(view.port_constraints->latency_anchors.size(), 2u);
            EXPECT_EQ(
                view.port_constraints->latency_anchors,
                (std::vector<PortLatencyAnchorConstraint>{
                    {{PortConstraintDirection::output, PortKind::sample, 0}, 3},
                    {{PortConstraintDirection::output, PortKind::event, 0}, 2},
                }));
            ++constrained_nodes;
        });
    EXPECT_EQ(constrained_nodes, 1u);
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsResamplerPortConstraints)
{
    using namespace iv;
    PortConstraintRef constexpr source{PortConstraintDirection::input, PortKind::sample, 1};
    NodePortConstraints const constraints{
        .independent_paces = {source},
        .preparation_leads = {source},
        .preparation_lead_equalities = {
            {
                .target = {PortConstraintDirection::input, PortKind::sample, 0},
                .source = source,
                .property = PreparationLeadTarget::history,
                .additional_samples = 2,
            },
            {{PortConstraintDirection::output, PortKind::sample, 0}, source, PreparationLeadTarget::latency},
        },
    };
    binary_wire_details::Writer writer;
    binary_wire_details::write_port_constraints(writer, constraints);
    auto const payload = std::move(writer).take();
    binary_wire_details::Reader reader(payload);
    EXPECT_EQ(binary_wire_details::read_port_constraints(reader), constraints);
    reader.finish();
}

TEST(ConfiguredGraphBinaryArchive, RoundTripsEveryOutputAccessRetentionMode)
{
    std::array const outputs {
        iv::materialize_continuous_config(iv::erase_port_config(
            iv::output<iv::Sample>(
                {"realtime_ephemeral", "Realtime ephemeral"}, iv::OutputProduction::sequential))),
        iv::materialize_continuous_config(iv::erase_port_config(
            iv::output<iv::Sample>(
                {"realtime_persisted", "Realtime persisted"},
                iv::OutputProduction::sequential,
                iv::OutputRealization::buffered))),
        iv::materialize_continuous_config(iv::erase_port_config(
            iv::output<iv::Sample>(
                {"background_ephemeral", "Background ephemeral"}, iv::OutputProduction::coverage))),
        iv::materialize_continuous_config(iv::erase_port_config(
            iv::output<iv::Sample>(
                {"background_persisted", "Background persisted"},
                iv::OutputProduction::coverage,
                iv::OutputRealization::buffered))),
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
        EXPECT_EQ(output.realization, expected.realization);
    }
    reader.finish();
}

} // namespace
