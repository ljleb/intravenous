#include <intravenous/graph_jit/output_gain.h>
#include <intravenous/graph_jit/output_gain_execution.h>
#include <intravenous/basic_nodes/output_gain.h>
#include <intravenous/runtime/node_instance_interface.h>
#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace {
using namespace iv;
using namespace iv::graph_jit::detail;

NodeDefinitionsSnapshot gain_definitions()
{
    NodeDefinitionsSnapshot result;
    auto package = std::make_shared<PackageRevision>();
    package->provenance = PackageProvenance::built_in;
    package->package_namespace = "builtin";
    package->node_compiler_records = {
        iv::details::node_compiler_record<OutputGainNode<ChannelTypeId::mono, SampleStreamLayout::planar>>,
        iv::details::node_compiler_record<OutputGainNode<ChannelTypeId::mono, SampleStreamLayout::interleaved>>,
    };
    result.package_revisions.push_back(std::move(package));
    return result;
}

template<class Production = OutputProduction::Sequential>
ConfiguredGraph gain_graph(bool dynamic = false, Sample gain = 1,
    OutputRealization realization = OutputRealization::computed, Production production = {})
{
    ConfiguredGraph graph;
    auto source = graph.node_bundles.append_concrete(ConcreteNode{
        .ports = {.output_configs = {erase_port_config(output<Sample>("out", production, realization))}}});
    graph.manual_controls.push_back({
        .id = {.node = {.instance_id = "source"}, .port_name = "out", .kind = ManualControlKind::output_gain},
        .settled_value = gain, .dynamic = dynamic, .output = NodeBundlePortId{source, PortKind::sample, 0}});
    return graph;
}

TEST(OutputGain, UnityHasNoPrivateStageOrControlStorage)
{
    auto graph = gain_graph();
    auto const count = graph.node_bundles.size();
    auto expanded = expand_output_gains(graph, gain_definitions());
    EXPECT_TRUE(expanded.stages.empty());
    EXPECT_TRUE(expanded.outputs.empty());
    EXPECT_TRUE(graph.manual_controls.empty());
    EXPECT_EQ(graph.node_bundles.size(), count);
    EXPECT_TRUE(graph.connections.configured_sample_connections().empty());
}

TEST(OutputGain, VirtualGainNeedsAConsumerOrAnObservation)
{
    auto graph = gain_graph(false, 2);
    graph.manual_controls.front().id.node.path.push_back(ProjectVirtualNodeSelector{.source_identity = "voice"});
    auto visible = graph;
    EXPECT_TRUE(expand_output_gains(graph, gain_definitions()).stages.empty());
    EXPECT_TRUE(graph.connections.configured_sample_connections().empty());

    std::array observed{SampleOutputChannelId{0, 0, 0}};
    auto expanded = expand_output_gains(visible, gain_definitions(), observed);
    ASSERT_EQ(expanded.stages.size(), 1u);
    EXPECT_EQ(expanded.routed_source(visible.node_bundles, observed.front()).bundle, expanded.stages.front().bundle);
}

TEST(OutputGain, OneTransformFeedsAllConsumersAndItsIngressKeepsTheRawSource)
{
    auto graph = gain_graph(false, -2);
    std::vector<ConfiguredSampleConnection> connections;
    for (int i = 0; i < 2; ++i) {
        auto sink = graph.node_bundles.append_concrete(ConcreteNode{
            .ports = {.input_configs = {erase_port_config(input<Sample>("in"))}}});
        connections.push_back({.source_channels = {{.channel = {0, 0, 0}}},
            .target_channels = {{sink, 0, 0}}});
    }
    graph.connections = GraphBuilderConnections::from_configured_connections(connections, {});
    auto expanded = expand_output_gains(graph, gain_definitions());
    ASSERT_EQ(expanded.stages.size(), 1u);
    auto stage = expanded.stages.front().bundle;
    EXPECT_FALSE(graph.node_bundles.materialize_concrete_description(stage).registered_node_type_identity);
    auto routed = graph.connections.configured_sample_connections();
    ASSERT_EQ(routed.size(), 3u);
    EXPECT_EQ(routed[0].source_channels[0].channel.bundle, stage);
    EXPECT_EQ(routed[1].source_channels[0].channel.bundle, stage);
    EXPECT_EQ(routed[2].source_channels[0].channel.bundle, 0u);
    EXPECT_EQ(routed[2].target_channels[0].bundle, stage);
    ASSERT_EQ(graph.manual_controls.size(), 1u);
    EXPECT_EQ(graph.manual_controls[0].id.kind, ManualControlKind::output_gain);
    EXPECT_EQ(graph.manual_controls[0].input.node_bundle_handle, stage);
    EXPECT_FALSE(graph.manual_controls[0].output);
    EXPECT_EQ(graph.manual_controls[0].settled_value, Sample{-2});
    auto const defaults = manual_input_defaults(graph.node_bundles.resolve_sample_input(
        {stage, PortKind::sample, 1}).config);
    ASSERT_TRUE(defaults.has_value());
    EXPECT_EQ(defaults->value, Sample{1});
    EXPECT_TRUE(defaults->enabled);
}

TEST(OutputGain, InteractiveUnityHasAControlButPreparationFreezesItsValue)
{
    auto live = gain_graph(true);
    ASSERT_EQ(expand_output_gains(live, gain_definitions()).stages.size(), 1u);
    EXPECT_TRUE(live.manual_controls[0].dynamic);
    auto prepared = gain_graph(true, .25f);
    prepared.manual_controls[0].requires_preparation = true;
    ASSERT_EQ(expand_output_gains(prepared, gain_definitions()).stages.size(), 1u);
    EXPECT_FALSE(prepared.manual_controls[0].dynamic);
    EXPECT_EQ(prepared.manual_controls[0].settled_value, Sample{.25f});
}

TEST(OutputGain, CoverageGainUsesTickOnlyUntilAnAuthoredConsumerNeedsBackgroundPreparation)
{
    auto graph = gain_graph(true, 2, OutputRealization::computed, OutputProduction::coverage);
    auto tick = graph.node_bundles.append_concrete(ConcreteNode{
        .ports = {.input_configs = {erase_port_config(input<Sample>("in"))},
            .output_configs = {erase_port_config(output<Sample>("out"))}},
        .operations = {.runtime = {.tick_block = +[](void const*, ReflectedNodeTickContext const&, GlobalIndex, std::size_t) {}}},
    });
    auto tock = graph.node_bundles.append_concrete(ConcreteNode{
        .ports = {.input_configs = {erase_port_config(input<Sample>("in", InputAccess::coverage))}},
    });
    std::array<ConfiguredSampleConnection, 3> connections{{
        {.source_channels = {{.channel = {0, 0, 0}}}, .target_channels = {{tick, 0, 0}}},
        {.source_channels = {{.channel = {tick, 0, 0}}}, .target_channels = {{tock, 0, 0}}},
        {.source_channels = {{.channel = {0, 0, 0}}}, .target_channels = {{tock, 0, 0}}},
    }};
    auto live = graph;
    live.connections = GraphBuilderConnections::from_configured_connections(std::span{connections}.first(1), {});
    classify_output_gain_consumers(live);
    EXPECT_FALSE(live.manual_controls[0].requires_preparation);
    auto live_gain = expand_output_gains(live, gain_definitions());
    ASSERT_EQ(live_gain.stages.size(), 1u);
    EXPECT_TRUE(live_gain.stages[0].tick);
    EXPECT_TRUE(live.manual_controls[0].dynamic);
    EXPECT_TRUE(is_sequential_output(live.node_bundles.resolve_sample_output(
        {live_gain.stages[0].bundle, PortKind::sample, 0}).config));

    // A Sequential consumer is itself replayed to produce data for Tock.
    auto replayed = graph;
    replayed.connections = GraphBuilderConnections::from_configured_connections(std::span{connections}.first(2), {});
    classify_output_gain_consumers(replayed);
    EXPECT_TRUE(replayed.manual_controls[0].requires_preparation);

    // A direct Coverage consumer and a Sequential consumer share one Tock gain.
    graph.connections = GraphBuilderConnections::from_configured_connections(connections, {});
    classify_output_gain_consumers(graph);
    EXPECT_TRUE(graph.manual_controls[0].requires_preparation);
    auto prepared_gain = expand_output_gains(graph, gain_definitions());
    ASSERT_EQ(prepared_gain.stages.size(), 1u);
    EXPECT_FALSE(prepared_gain.stages[0].tick);
    EXPECT_TRUE(graph.manual_controls.empty());
    EXPECT_EQ(graph.connections.configured_sample_connections()[0].source_channels[0].channel.bundle,
        prepared_gain.stages[0].bundle);
    EXPECT_EQ(graph.connections.configured_sample_connections()[2].source_channels[0].channel.bundle,
        prepared_gain.stages[0].bundle);
}

TEST(OutputGain, BufferedOutputSeparatesLiveScalingFromHistoricalReads)
{
    auto graph = gain_graph(false, 2, OutputRealization::buffered);
    graph.manual_controls[0].requires_preparation = true;
    auto expanded = expand_output_gains(graph, gain_definitions());
    ASSERT_EQ(expanded.stages.size(), 2u);
    NodeBundlePortId const source{0, PortKind::sample, 0};
    auto const live = expanded.outputs.at(source);
    auto const historical = expanded.background_outputs.at(source);
    EXPECT_NE(live, historical);
    auto const raw_config = graph.node_bundles.resolve_sample_output(source).config;
    auto const live_config = graph.node_bundles.resolve_sample_output(live).config;
    auto const historical_config = graph.node_bundles.resolve_sample_output(historical).config;
    EXPECT_EQ(raw_config.realization, OutputRealization::buffered);
    EXPECT_TRUE(is_sequential_output(live_config));
    EXPECT_EQ(live_config.realization, OutputRealization::computed);
    EXPECT_EQ(output_production(historical_config), OutputProductionKind::coverage);
    EXPECT_EQ(historical_config.realization, OutputRealization::buffered);
    auto connections = graph.connections.configured_sample_connections();
    ASSERT_EQ(connections.size(), 2u);
    for (auto const& connection : connections)
        EXPECT_EQ(connection.source_channels[0].channel.bundle, source.node_bundle_handle);
    EXPECT_TRUE(expanded.stages[0].tick);
    EXPECT_FALSE(expanded.stages[1].tick);
    EXPECT_EQ(expanded.stages[0].configuration, expanded.stages[1].configuration);
}

TEST(OutputGain, ATiledChildGainReachesTheAggregateWithoutChangingOtherChannels)
{
    auto graph = gain_graph(false, .5f);
    auto const right = graph.node_bundles.append_concrete(ConcreteNode{
        .ports = {.output_configs = {erase_port_config(output<Sample>("out"))}}});
    std::array<NodeBundleHandle, 2> const members{0, right};
    auto const tile = graph.node_bundles.append_tiled(members,
        {ChannelTypeId::stereo, SampleStreamLayout::planar});
    auto stereo = input<Sample>("in", {.channel_layout = {ChannelTypeId::stereo, SampleStreamLayout::planar}});
    auto const sink = graph.node_bundles.append_concrete(ConcreteNode{
        .ports = {.input_configs = {erase_port_config(stereo)}}});
    ConfiguredSampleConnection connection{
        .source_type = ChannelTypeId::stereo,
        .source_channels = {{.channel = {tile, 0, 0}}, {.channel = {tile, 0, 1}}},
        .target_type = ChannelTypeId::stereo,
        .target_channels = {{sink, 0, 0}, {sink, 0, 1}}};
    graph.connections = GraphBuilderConnections::from_configured_connections(std::span{&connection, 1}, {});
    auto expanded = expand_output_gains(graph, gain_definitions());
    ASSERT_EQ(expanded.stages.size(), 1u);
    auto routed = graph.connections.configured_sample_connections();
    ASSERT_EQ(routed.size(), 2u);
    ASSERT_EQ(routed[0].source_channels.size(), 2u);
    EXPECT_EQ(routed[0].source_channels[0].channel.bundle, expanded.stages[0].bundle);
    EXPECT_EQ(routed[0].source_channels[0].channel.channel, 0u);
    EXPECT_EQ(routed[0].source_channels[1].channel.bundle, tile);
    EXPECT_EQ(routed[0].source_channels[1].channel.channel, 1u);
}

TEST(OutputGain, RegisteredScalingPreservesSampleAndComplexSourceBytes)
{
    auto const& sample = continuous_value_type_info(ContinuousValueTypeId::sample);
    ASSERT_NE(sample.apply_output_gain, nullptr);
    Sample const raw = 3;
    Sample scaled;
    sample.apply_output_gain(&scaled, &raw, 1, -.5f);
    EXPECT_EQ(raw, Sample{3});
    EXPECT_EQ(scaled, Sample{-1.5f});
    std::array<FFTBin, 3> const bins{{{1, 2}, {-3, 4}, {0, -2}}};
    std::array<FFTBin, 3> transformed{};
    auto const& fft = continuous_value_type_info(ContinuousValueTypeId::fft_block);
    ASSERT_NE(fft.apply_output_gain, nullptr);
    fft.apply_output_gain(transformed.data(), bins.data(), bins.size(), .25f);
    EXPECT_EQ(transformed[0], FFTBin(.25f, .5f));
    EXPECT_EQ(transformed[1], FFTBin(-.75f, 1));
    EXPECT_EQ(transformed[2], FFTBin(0, -.5f));
    EXPECT_EQ(bins[1], FFTBin(-3, 4));
    auto const& positions = continuous_value_type_info(ContinuousValueTypeId::fractional_global_index);
    EXPECT_EQ(positions.apply_output_gain, nullptr);
}

TEST(OutputGain, DemandWindowsKeepOverlappingRegionIdentityAndRawContents)
{
    std::array<Sample, 2> raw_a{Sample{2}, Sample{3}}, raw_b{Sample{-1}, Sample{4}};
    std::array<Sample, 2> scaled_a{}, scaled_b{};
    std::array<std::uint8_t, 2> written_a{}, written_b{};
    GlobalIndexRange const window{GlobalIndex::from_signed(100), 2};
    GlobalIndexRange const logical{GlobalIndex::from_signed(0), std::uint64_t{1} << 40};
    ContinuousRegionStorageView source_a{.storage = reinterpret_cast<std::byte*>(raw_a.data()), .range = window};
    ContinuousRegionStorageView source_b{.storage = reinterpret_cast<std::byte*>(raw_b.data()), .range = window};
    ContinuousRegionStorageView target_a{.storage = reinterpret_cast<std::byte*>(scaled_a.data()),
        .written_values = written_a.data(), .range = window};
    ContinuousRegionStorageView target_b{.storage = reinterpret_cast<std::byte*>(scaled_b.data()),
        .written_values = written_b.data(), .range = window};
    std::array<ContinuousRegionStorageView, 2> sources{{
        {.range = logical, .windows = std::span{&source_a, 1}},
        {.range = logical, .windows = std::span{&source_b, 1}},
    }};
    std::array<ContinuousRegionStorageView, 2> targets{{
        {.range = logical, .windows = std::span{&target_a, 1}},
        {.range = logical, .windows = std::span{&target_b, 1}},
    }};
    std::array<RandomAccessContinuousInputPort, 1> inputs{{
        {.regions = sources.data(), .region_count = sources.size()},
    }};
    std::array<TockContinuousOutputPort, 1> outputs{{
        {.regions = targets.data(), .region_count = targets.size()},
    }};
    OutputGainConfiguration gain{.gain = -.5f};
    tock_output_gain(&gain, {.inputs = inputs, .outputs = outputs});
    EXPECT_EQ(scaled_a, (std::array{Sample{-1}, Sample{-1.5f}}));
    EXPECT_EQ(scaled_b, (std::array{Sample{.5f}, Sample{-2}}));
    EXPECT_EQ(raw_a, (std::array{Sample{2}, Sample{3}}));
    EXPECT_EQ(raw_b, (std::array{Sample{-1}, Sample{4}}));
    EXPECT_EQ(written_a, (std::array<std::uint8_t, 2>{1, 1}));
    EXPECT_EQ(written_b, (std::array<std::uint8_t, 2>{1, 1}));
    EXPECT_EQ(targets[0].range, logical);
    EXPECT_EQ(targets[1].range, logical);
    EXPECT_EQ(targets[0].value_storage(GlobalIndex::from_signed(99), 0), nullptr);
}

TEST(OutputGain, NonfiniteGainsAreRejectedBeforePrivateExpansion)
{
    auto graph = gain_graph(false, std::numeric_limits<float>::infinity());
    EXPECT_THROW(expand_output_gains(graph, gain_definitions()), std::invalid_argument);
}
} // namespace
