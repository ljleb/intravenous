#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_playback_sample_history_module_id[] =
    "iv.test.graph_jit.state_context.playback_sample_history_module";

constexpr char graph_jit_playback_sample_history_replacement_module_id[] =
    "iv.test.graph_jit.state_context.playback_sample_history_replacement_module";

constexpr char graph_jit_playback_sample_window_module_id[] =
    "iv.test.graph_jit.state_context.playback_sample_window_module";

constexpr char graph_jit_playback_mixed_sample_module_id[] =
    "iv.test.graph_jit.state_context.playback_mixed_sample_module";

constexpr char graph_jit_playback_event_history_module_id[] =
    "iv.test.graph_jit.state_context.playback_event_history_module";

constexpr char graph_jit_playback_event_history_replacement_module_id[] =
    "iv.test.graph_jit.state_context.playback_event_history_replacement_module";

constexpr char graph_jit_playback_mixed_event_module_id[] =
    "iv.test.graph_jit.state_context.playback_mixed_event_module";

constexpr char graph_jit_playback_sample_rewire_module_id[] =
    "iv.test.graph_jit.state_context.playback_sample_rewire_module";

constexpr char graph_jit_playback_event_rewire_module_id[] =
    "iv.test.graph_jit.state_context.playback_event_rewire_module";

struct PlaybackHistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::array<float, 5> window{};
};

struct PlaybackHistoryPeerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::array<float, 5> window{};
    std::uint64_t marker = 0;
};

struct PlaybackStereoHistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::array<float, 3> left{};
    std::array<float, 3> right{};
};

struct PlaybackEventHistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t count = 0;
    std::array<std::uint64_t, 8> times{};
};

struct PlaybackEventHistoryPeerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t count = 0;
    std::array<std::uint64_t, 8> times{};
    std::uint64_t marker = 0;
};

// Drives a compiled root through real TickInvocationCall slots. Each callback's
// background playback is published as the TickMaterializationSnapshot that a
// background transaction would promote, so the test controls exactly what
// every slot delivers.
class TickPlayback {
    iv::PersistedDataStore data_{};
    iv::PersistedDataStore::ReaderSlot data_reader_ = data_.register_reader();
    iv::TickMaterializationStore materializations_{};
    iv::TickMaterializationStore::ReaderSlot materialization_reader_ =
        materializations_.register_reader();
    iv::CompiledGraph const& graph_;
    iv::TickInvocationWorkspace workspace_;
    std::uint64_t semantic_version_ = 0;

public:
    explicit TickPlayback(iv::CompiledGraph const& graph)
        : graph_(graph)
        , workspace_(
            graph.background_evaluation_plan,
            graph.project_generation,
            data_,
            graph.specialization.block_size)
    {}

    void publish(
        std::vector<iv::TickMaterializedSampleInput> samples,
        std::vector<iv::TickMaterializedEventInput> events = {})
    {
        (void)materializations_.promote(
            std::make_unique<iv::TickMaterializationSnapshot>(
                graph_.project_generation,
                ++semantic_version_,
                iv::PersistedDataVersion{},
                std::move(samples),
                std::move(events)));
    }

    template<class Storage>
    void tick(
        Storage& storage,
        iv::GlobalIndex sample_index,
        std::size_t block_size,
        bool transition = false)
    {
        iv::TickInvocationFrame frame{
            data_reader_,
            materialization_reader_,
            workspace_,
            sample_index,
            block_size};
        auto const operation = transition
            ? graph_.root_operations.transition_tick_block
            : graph_.root_operations.tick_block;
        ASSERT_NE(operation, nullptr);
        operation(
            storage.buffer().data(), &frame.call(), sample_index, block_size);
    }
};

// Background playback of the graph's one slot-fed sample input over
// [begin, end). value(position, channel) supplies every channel.
template<class Value>
iv::TickMaterializedSampleInput playback_samples(
    iv::CompiledGraph const& graph,
    std::int64_t begin,
    std::int64_t end,
    Value value)
{
    auto const& plan = graph.background_evaluation_plan;
    EXPECT_EQ(plan.tick_runtime.sequential_sample_inputs.size(), 1u);
    auto const port = plan.tick_runtime.sequential_sample_inputs.at(0).port;
    auto const layout = plan.ports.at(port).sample_layout;
    std::vector<std::size_t> channels(iv::channel_count(layout));
    std::iota(channels.begin(), channels.end(), std::size_t{0});
    std::vector<iv::Sample> values;
    for (auto const channel : channels) {
        for (auto position = begin; position < end; ++position) {
            values.push_back(iv::Sample{value(position, channel)});
        }
    }
    return iv::make_tick_materialized_sample_input(
        port,
        layout,
        std::move(channels),
        iv::IndexCoverage{range(begin, end)},
        std::move(values));
}

// Background playback of the graph's one slot-fed event input over
// [begin, end), with trigger events at times.
iv::TickMaterializedEventInput playback_events(
    iv::CompiledGraph const& graph,
    std::int64_t begin,
    std::int64_t end,
    std::vector<std::int64_t> const& times,
    std::size_t semantic_source_index)
{
    auto const& plan = graph.background_evaluation_plan;
    EXPECT_EQ(plan.tick_runtime.sequential_event_inputs.size(), 1u);
    auto const port = plan.tick_runtime.sequential_event_inputs.at(0).port;
    std::vector<iv::TimedEvent> events;
    for (auto const time : times) {
        events.push_back({.time = index(time), .value = iv::TriggerEvent{}});
    }
    return iv::make_tick_materialized_event_input(
        port,
        plan.ports.at(port).event_type,
        iv::IndexCoverage{range(begin, end)},
        std::move(events),
        std::vector<std::size_t>(times.size(), semantic_source_index));
}

template<class Mirror, class Storage>
Mirror* playback_state(iv::CompiledGraph const& graph, Storage& storage)
{
    Mirror* result = nullptr;
    for (std::size_t node = 0; node < graph.node_layout.nodes.size(); ++node) {
        if (graph.node_layout.nodes[node].state_size != sizeof(Mirror)) continue;
        EXPECT_EQ(result, nullptr) << "playback state size is ambiguous";
        result = static_cast<Mirror*>(storage.state_ptr(node));
    }
    EXPECT_NE(result, nullptr);
    return result;
}

std::vector<std::uint64_t> recorded_times(
    std::uint64_t count, std::array<std::uint64_t, 8> const& times)
{
    return {
        times.begin(),
        times.begin() + static_cast<std::ptrdiff_t>(
            std::min<std::uint64_t>(count, times.size())),
    };
}

// The rewired successor exchanges the sources of the module's two
// connections, so each consumer changes delivery kind while keeping its
// stable port-state identity.
std::shared_ptr<iv::ConfiguredGraph const> exchange_connection_sources(
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor)
{
    auto successor = std::make_shared<iv::ConfiguredGraph>(*predecessor);
    auto sample_connections = std::vector<iv::ConfiguredSampleConnection>(
        predecessor->connections.configured_sample_connections().begin(),
        predecessor->connections.configured_sample_connections().end());
    auto event_connections = std::vector<iv::ConfiguredEventConnection>(
        predecessor->connections.configured_event_connections().begin(),
        predecessor->connections.configured_event_connections().end());
    if (sample_connections.size() + event_connections.size() != 2
        || (sample_connections.size() != 2 && event_connections.size() != 2)) {
        throw std::runtime_error(
            "unexpected playback rewire connection count");
    }
    if (sample_connections.size() == 2) {
        std::swap(
            sample_connections[0].source_type,
            sample_connections[1].source_type);
        std::swap(
            sample_connections[0].source_channels,
            sample_connections[1].source_channels);
    } else {
        std::swap(
            event_connections[0].source_type,
            event_connections[1].source_type);
        std::swap(event_connections[0].sources, event_connections[1].sources);
    }
    successor->connections =
        iv::GraphBuilderConnections::from_configured_connections(
            sample_connections, event_connections);
    return successor;
}

// Rebuild the same ConfiguredGraph with one unused scope-boundary bundle in
// front of every original bundle. Stable virtual-node records and graph
// identity are preserved; only generation-local bundle coordinates move.
std::shared_ptr<iv::ConfiguredGraph const> shift_bundle_handles(
    std::shared_ptr<iv::ConfiguredGraph const> const& graph)
{
    auto shifted = std::make_shared<iv::ConfiguredGraph>(*graph);

    iv::GraphBuilderNodeBundles bundles;
    (void)bundles.append_scope_boundary();
    auto const offset = bundles.import_child(graph->node_bundles);
    if (offset == 0) {
        throw std::runtime_error(
            "playback bundle-shift fixture did not create an offset");
    }
    shifted->node_bundles = std::move(bundles);

    iv::GraphBuilderConnections connections;
    connections.import_child(graph->connections, offset);
    shifted->connections = std::move(connections);

    auto virtual_nodes = graph->virtual_nodes.records();
    auto shift_port = [offset](iv::NodeBundlePortId& port) {
        port.node_bundle_handle += offset;
    };
    auto shift_channel = [offset](auto& channel) {
        channel.bundle += offset;
    };
    for (auto& node : virtual_nodes) {
        for (auto& bundle : node.node_bundle_handles) bundle += offset;
        for (auto& mapping : node.sample_inputs) {
            for (auto& channel : mapping.channels) shift_channel(channel);
            for (auto& member : mapping.member_channels) {
                for (auto& channel : member) shift_channel(channel);
            }
        }
        for (auto& mapping : node.sample_outputs) {
            for (auto& channel : mapping.channels) shift_channel(channel);
            for (auto& member : mapping.member_channels) {
                for (auto& channel : member) shift_channel(channel);
            }
        }
        for (auto& mapping : node.event_inputs) {
            for (auto& port : mapping.node_bundle_ports) shift_port(port);
        }
        for (auto& mapping : node.event_outputs) {
            for (auto& port : mapping.node_bundle_ports) shift_port(port);
        }
    }
    // GraphBuilderNodeBundles::import_child() clears inverse virtual-node
    // handles because ordinary child import rebuilds them through
    // GraphBuilderVirtualNodes::import_child(). This fixture preserves the
    // original virtual-node IDs instead, so restore the inverse relation
    // explicitly after shifting every record's bundle coordinates.
    for (std::size_t virtual_handle = 0;
         virtual_handle < virtual_nodes.size();
         ++virtual_handle) {
        for (auto const bundle :
             virtual_nodes[virtual_handle].node_bundle_handles) {
            auto& inverse = shifted->node_bundles
                .bundle(bundle)
                .virtual_node_handles();
            if (!std::ranges::contains(inverse, virtual_handle)) {
                inverse.push_back(virtual_handle);
            }
        }
    }
    shifted->virtual_nodes =
        iv::GraphBuilderVirtualNodes::from_configured_records(virtual_nodes);

    auto public_ports = graph->public_ports.configured_record();
    public_ports.boundary += offset;
    shifted->public_ports =
        iv::GraphBuilderPublicPorts::from_configured_record(public_ports);
    return shifted;
}

iv::NodeLayout::Region const& input_timeline_region(
    iv::CompiledGraph const& graph,
    std::string_view prefix)
{
    iv::NodeLayout::Region const* result = nullptr;
    for (auto const& region : graph.node_layout.regions) {
        if (region.kind != iv::NodeLayout::Region::Kind::raw
            || !region.migration_identity.starts_with(prefix)) {
            continue;
        }
        if (result) {
            throw std::runtime_error(
                "playback fixture has multiple input timeline regions");
        }
        result = &region;
    }
    if (!result) {
        throw std::runtime_error(
            "playback fixture has no stable input timeline region");
    }
    return *result;
}

iv::graph_jit::RealtimePortStateRequirement const& stable_input_history(
    iv::CompiledGraph const& graph,
    iv::PortKind kind)
{
    auto const found = std::ranges::find_if(
        graph.realtime_port_state_requirements.states,
        [kind](auto const& requirement) {
            return requirement.direction
                    == iv::graph_jit::PortDirection::input
                && requirement.configured_port.port_kind == kind
                && requirement.role == iv::graph_jit::RealtimePortStateRole::
                    sequential_input_history
                && requirement.stable_identity.has_value();
        });
    if (found == graph.realtime_port_state_requirements.states.end()) {
        throw std::runtime_error(
            "playback fixture has no stable input-history requirement");
    }
    return *found;
}

} // namespace

TEST_F(
    GraphJitRuntimeFixture,
    InputTimelineRawMigrationUsesStablePortIdentityForSamplesAndEvents)
{
    // RealtimePortStateTransition.MatchesStableStateAcrossGenerationLocalPorts
    // already covers semantic requirement matching after a bundle-number
    // change. This fixture verifies that the exact-shape NodeStorage fast path
    // uses that same identity rule and does not copy a different node's bytes.
    auto verify = [&] (
        std::string_view module_id,
        std::string_view replacement_module_id,
        iv::PortKind kind,
        std::string_view region_prefix,
        std::uint64_t generation) {
        SCOPED_TRACE(region_prefix);
        auto graph = configured_module_graph(*revision, module_id);
        auto replacement_graph = configured_module_graph(
            *revision, replacement_module_id);
        ASSERT_TRUE(graph);
        ASSERT_TRUE(replacement_graph);
        auto shifted_graph = shift_bundle_handles(graph);

        auto predecessor = compile_graph(graph, generation);
        auto replacement = compile_graph(replacement_graph, generation + 1);
        auto shifted = compile_graph(shifted_graph, generation + 2);
        ASSERT_TRUE(predecessor.succeeded())
            << (predecessor.diagnostics.empty()
                    ? ""
                    : predecessor.diagnostics.front().message);
        ASSERT_TRUE(replacement.succeeded())
            << (replacement.diagnostics.empty()
                    ? ""
                    : replacement.diagnostics.front().message);
        ASSERT_TRUE(shifted.succeeded())
            << (shifted.diagnostics.empty()
                    ? ""
                    : shifted.diagnostics.front().message);

        auto const& predecessor_compiled = *predecessor.compiled_graph;
        auto const& replacement_compiled = *replacement.compiled_graph;
        auto const& shifted_compiled = *shifted.compiled_graph;
        auto const& predecessor_requirement = stable_input_history(
            predecessor_compiled, kind);
        auto const& replacement_requirement = stable_input_history(
            replacement_compiled, kind);
        auto const& shifted_requirement = stable_input_history(
            shifted_compiled, kind);
        ASSERT_TRUE(predecessor_requirement.stable_identity);
        ASSERT_TRUE(replacement_requirement.stable_identity);
        ASSERT_TRUE(shifted_requirement.stable_identity);
        EXPECT_EQ(
            predecessor_requirement.configured_port,
            replacement_requirement.configured_port);
        EXPECT_NE(
            predecessor_requirement.stable_identity->node,
            replacement_requirement.stable_identity->node);
        EXPECT_NE(
            predecessor_requirement.configured_port.node_bundle_handle,
            shifted_requirement.configured_port.node_bundle_handle);
        EXPECT_EQ(
            predecessor_requirement.stable_identity,
            shifted_requirement.stable_identity);

        auto const& predecessor_region = input_timeline_region(
            predecessor_compiled, region_prefix);
        auto const& replacement_region = input_timeline_region(
            replacement_compiled, region_prefix);
        auto const& shifted_region = input_timeline_region(
            shifted_compiled, region_prefix);
        EXPECT_NE(
            predecessor_region.migration_identity,
            replacement_region.migration_identity);
        EXPECT_EQ(
            predecessor_region.migration_identity,
            shifted_region.migration_identity);
        ASSERT_EQ(predecessor_region.size, replacement_region.size);
        ASSERT_EQ(predecessor_region.size, shifted_region.size);
        ASSERT_EQ(predecessor_region.alignment, replacement_region.alignment);
        ASSERT_EQ(predecessor_region.alignment, shifted_region.alignment);

        auto predecessor_storage =
            predecessor_compiled.node_layout.create_storage(resources);
        predecessor_storage.initialize();
        auto predecessor_bytes = predecessor_storage.buffer().subspan(
            predecessor_region.storage_offset, predecessor_region.size);
        std::ranges::fill(predecessor_bytes, std::byte{0x5a});

        auto fresh_replacement =
            replacement_compiled.node_layout.create_storage(resources);
        fresh_replacement.initialize();
        auto const fresh_region_bytes = fresh_replacement.buffer().subspan(
            replacement_region.storage_offset, replacement_region.size);
        auto const fresh_bytes = std::vector<std::byte>(
            fresh_region_bytes.begin(), fresh_region_bytes.end());
        auto migrated_replacement =
            replacement_compiled.node_layout.create_storage(resources);
        migrated_replacement.initialize(&predecessor_storage);
        EXPECT_TRUE(std::ranges::equal(
            fresh_bytes,
            migrated_replacement.buffer().subspan(
                replacement_region.storage_offset, replacement_region.size)));

        auto migrated_shifted =
            shifted_compiled.node_layout.create_storage(resources);
        migrated_shifted.initialize(&predecessor_storage);
        EXPECT_TRUE(std::ranges::all_of(
            migrated_shifted.buffer().subspan(
                shifted_region.storage_offset, shifted_region.size),
            [](std::byte value) { return value == std::byte{0x5a}; }));
    };

    verify(
        graph_jit_playback_sample_history_module_id,
        graph_jit_playback_sample_history_replacement_module_id,
        iv::PortKind::sample,
        "graphjit.sample.input_timeline",
        621);
    verify(
        graph_jit_playback_event_history_module_id,
        graph_jit_playback_event_history_replacement_module_id,
        iv::PortKind::event,
        "graphjit.event.input_timeline",
        631);
}

TEST_F(GraphJitRuntimeFixture, PlaybackSampleInputKeepsItsConsumedHistory)
{
    auto compiled = compile(graph_jit_playback_sample_history_module_id, 501);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<PlaybackHistoryConsumerStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};

    // History before the first invocation is the input's neutral value.
    playback.publish({playback_samples(graph, 0, 64, [](auto position, auto) {
        return 1000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(0), 64);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(
        consumer->window,
        (std::array<float, 5>{0.0f, 0.0f, 0.0f, 0.0f, 1000.0f}));

    // Republished earlier positions do not rewrite consumed history.
    playback.publish({playback_samples(graph, 0, 128, [](auto position, auto) {
        return position < 64 ? -1.0f : 2000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(64), 64);
    EXPECT_EQ(
        consumer->window,
        (std::array<float, 5>{1060.0f, 1061.0f, 1062.0f, 1063.0f, 2064.0f}));

    // After a seek, history remains the preceding values in invocation order.
    playback.publish({playback_samples(graph, 1000, 1064, [](auto position, auto) {
        return 3000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(1000), 64);
    EXPECT_EQ(consumer->last_index, index(1000).bits());
    EXPECT_EQ(
        consumer->window,
        (std::array<float, 5>{2124.0f, 2125.0f, 2126.0f, 2127.0f, 4000.0f}));
}

TEST_F(GraphJitRuntimeFixture, PlaybackSampleInputWithoutHistoryReadsItsWindow)
{
    auto compiled = compile(graph_jit_playback_sample_window_module_id, 502);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<SampleConsumerProbeStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};

    // The window begins at a position that is not a multiple of the slot
    // capacity.
    playback.publish({playback_samples(graph, 100, 108, [](auto position, auto) {
        return 1000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(100), 8);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(consumer->last_block_size, 8u);
    EXPECT_FLOAT_EQ(consumer->first, 1100.0f);
    EXPECT_FLOAT_EQ(consumer->last, 1107.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 8828.0f);
}

TEST_F(GraphJitRuntimeFixture, MixedSampleInputKeepsItsCombinedHistory)
{
    auto compiled = compile(graph_jit_playback_mixed_sample_module_id, 503);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<PlaybackStereoHistoryConsumerStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};

    // The left channel is the realtime ramp; the background supplies only
    // the right channel.
    playback.publish({playback_samples(graph, 64, 128, [](auto position, auto channel) {
        return channel == 0 ? 0.0f : 1000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(64), 64);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(consumer->left, (std::array<float, 3>{0.0f, 0.0f, 64.0f}));
    EXPECT_EQ(consumer->right, (std::array<float, 3>{0.0f, 0.0f, 1064.0f}));

    playback.publish({playback_samples(graph, 64, 192, [](auto position, auto channel) {
        if (channel == 0) return 0.0f;
        return position < 128 ? -1.0f : 2000.0f + static_cast<float>(position);
    })});
    playback.tick(storage, index(128), 64);
    EXPECT_EQ(consumer->left, (std::array<float, 3>{126.0f, 127.0f, 128.0f}));
    EXPECT_EQ(
        consumer->right, (std::array<float, 3>{1126.0f, 1127.0f, 2128.0f}));
}

TEST_F(GraphJitRuntimeFixture, PlaybackEventInputKeepsItsConsumedHistory)
{
    auto compiled = compile(graph_jit_playback_event_history_module_id, 504);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<PlaybackEventHistoryConsumerStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};
    auto constexpr unindexed = std::numeric_limits<std::size_t>::max();

    playback.publish({}, {playback_events(graph, 0, 64, {10, 50}, unindexed)});
    playback.tick(storage, index(0), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{10, 50}));

    // Republished earlier positions do not rewrite consumed history.
    playback.publish(
        {}, {playback_events(graph, 0, 128, {70, 120}, unindexed)});
    playback.tick(storage, index(64), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{50, 70, 120}));

    // A seek moves retained history with the transport: the event consumed
    // at 120 stays 8 samples before the block that follows it.
    playback.publish(
        {}, {playback_events(graph, 1000, 1064, {1010}, unindexed)});
    playback.tick(storage, index(1000), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{992, 1010}));
}

TEST_F(GraphJitRuntimeFixture, MixedEventInputKeepsItsMergedHistory)
{
    auto compiled = compile(graph_jit_playback_mixed_event_module_id, 505);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto storage = graph.node_layout.create_storage(resources);
    storage.initialize();
    auto* consumer =
        playback_state<PlaybackEventHistoryConsumerStateMirror>(graph, storage);
    ASSERT_NE(consumer, nullptr);
    TickPlayback playback{graph};

    // The realtime source triggers at index + 3 and index + block - 1; the
    // background source is the second semantic source.
    playback.publish({}, {playback_events(graph, 0, 64, {20, 40}, 1)});
    playback.tick(storage, index(0), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{3, 20, 40, 63}));

    playback.publish({}, {playback_events(graph, 0, 128, {80}, 1)});
    playback.tick(storage, index(64), 64);
    EXPECT_EQ(
        recorded_times(consumer->count, consumer->times),
        (std::vector<std::uint64_t>{40, 63, 67, 80, 127}));
}

TEST_F(GraphJitRuntimeFixture, RevisionMovesSampleHistoryAcrossDeliveryKinds)
{
    auto predecessor_graph = configured_module_graph(
        *revision, graph_jit_playback_sample_rewire_module_id);
    ASSERT_TRUE(predecessor_graph);
    auto predecessor = compile_graph(predecessor_graph, 601);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    auto const& predecessor_compiled = *predecessor.compiled_graph;
    auto predecessor_storage =
        predecessor_compiled.node_layout.create_storage(resources);
    predecessor_storage.initialize();
    auto* realtime_fed = playback_state<PlaybackHistoryConsumerStateMirror>(
        predecessor_compiled, predecessor_storage);
    auto* background_fed = playback_state<PlaybackHistoryPeerStateMirror>(
        predecessor_compiled, predecessor_storage);
    ASSERT_NE(realtime_fed, nullptr);
    ASSERT_NE(background_fed, nullptr);
    {
        TickPlayback playback{predecessor_compiled};
        for (std::int64_t begin = 0; begin < 128; begin += 64) {
            playback.publish({playback_samples(
                predecessor_compiled, begin, begin + 64,
                [](auto position, auto) {
                    return 1000.0f + static_cast<float>(position);
                })});
            playback.tick(predecessor_storage, index(begin), 64);
        }
    }
    ASSERT_EQ(
        realtime_fed->window,
        (std::array<float, 5>{60.0f, 61.0f, 62.0f, 63.0f, 64.0f}));
    ASSERT_EQ(
        background_fed->window,
        (std::array<float, 5>{
            1060.0f, 1061.0f, 1062.0f, 1063.0f, 1064.0f}));

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 602,
        .graph = exchange_connection_sources(predecessor_graph),
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
        iv::GraphJitTransitionCompileRequest{
            .successor = successor_request,
            .steady_successor = steady_successor.compiled_graph,
            .active_predecessor = predecessor.compiled_graph,
            .active_predecessor_phase =
                iv::CompiledGraphRealtimePhase::steady,
        });
    ASSERT_TRUE(specialized.succeeded())
        << (specialized.diagnostics.empty()
                ? ""
                : specialized.diagnostics.front().message);
    auto const& successor = *specialized.compiled_graph;
    ASSERT_TRUE(successor.cutover);

    auto successor_storage = successor.node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        successor.cutover->state_transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(128));

    // The consumer that was realtime-fed is now fed by playback, and the
    // other way around. Each keeps the history it consumed before cutover.
    auto* now_background_fed = playback_state<PlaybackHistoryConsumerStateMirror>(
        successor, successor_storage);
    auto* now_realtime_fed = playback_state<PlaybackHistoryPeerStateMirror>(
        successor, successor_storage);
    ASSERT_NE(now_background_fed, nullptr);
    ASSERT_NE(now_realtime_fed, nullptr);
    TickPlayback playback{successor};
    auto const playback_value = [](auto position, auto) {
        return 5000.0f + static_cast<float>(position);
    };
    playback.publish({playback_samples(successor, 128, 192, playback_value)});
    playback.tick(
        successor_storage,
        index(128),
        64,
        successor.root_operations.transition_tick_block != nullptr);
    EXPECT_EQ(
        now_background_fed->window,
        (std::array<float, 5>{124.0f, 125.0f, 126.0f, 127.0f, 5128.0f}));
    EXPECT_EQ(
        now_realtime_fed->window,
        (std::array<float, 5>{1124.0f, 1125.0f, 1126.0f, 1127.0f, 128.0f}));

    // Four values of history have elapsed, so the steady root continues.
    playback.publish({playback_samples(successor, 192, 256, playback_value)});
    playback.tick(successor_storage, index(192), 64);
    EXPECT_EQ(
        now_background_fed->window,
        (std::array<float, 5>{5188.0f, 5189.0f, 5190.0f, 5191.0f, 5192.0f}));
    EXPECT_EQ(
        now_realtime_fed->window,
        (std::array<float, 5>{188.0f, 189.0f, 190.0f, 191.0f, 192.0f}));
}

TEST_F(GraphJitRuntimeFixture, RevisionMovesEventHistoryAcrossDeliveryKinds)
{
    auto predecessor_graph = configured_module_graph(
        *revision, graph_jit_playback_event_rewire_module_id);
    ASSERT_TRUE(predecessor_graph);
    auto predecessor = compile_graph(predecessor_graph, 611);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    auto const& predecessor_compiled = *predecessor.compiled_graph;
    auto predecessor_storage =
        predecessor_compiled.node_layout.create_storage(resources);
    predecessor_storage.initialize();
    auto* realtime_fed = playback_state<PlaybackEventHistoryConsumerStateMirror>(
        predecessor_compiled, predecessor_storage);
    auto* background_fed = playback_state<PlaybackEventHistoryPeerStateMirror>(
        predecessor_compiled, predecessor_storage);
    ASSERT_NE(realtime_fed, nullptr);
    ASSERT_NE(background_fed, nullptr);
    auto constexpr unindexed = std::numeric_limits<std::size_t>::max();
    {
        TickPlayback playback{predecessor_compiled};
        playback.publish(
            {}, {playback_events(predecessor_compiled, 0, 64, {10}, unindexed)});
        playback.tick(predecessor_storage, index(0), 64);
        playback.publish(
            {}, {playback_events(predecessor_compiled, 64, 128, {100}, unindexed)});
        playback.tick(predecessor_storage, index(64), 64);
    }
    // The realtime source triggers at index + 3 and index + block - 1.
    ASSERT_EQ(
        recorded_times(realtime_fed->count, realtime_fed->times),
        (std::vector<std::uint64_t>{63, 67, 127}));
    ASSERT_EQ(
        recorded_times(background_fed->count, background_fed->times),
        (std::vector<std::uint64_t>{100}));

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 612,
        .graph = exchange_connection_sources(predecessor_graph),
        .definitions = definitions,
    };
    auto steady_successor = jit->compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit->compile_transition(
        iv::GraphJitTransitionCompileRequest{
            .successor = successor_request,
            .steady_successor = steady_successor.compiled_graph,
            .active_predecessor = predecessor.compiled_graph,
            .active_predecessor_phase =
                iv::CompiledGraphRealtimePhase::steady,
        });
    ASSERT_TRUE(specialized.succeeded())
        << (specialized.diagnostics.empty()
                ? ""
                : specialized.diagnostics.front().message);
    auto const& successor = *specialized.compiled_graph;
    ASSERT_TRUE(successor.cutover);

    auto successor_storage = successor.node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        successor.cutover->state_transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(128));

    auto* now_background_fed =
        playback_state<PlaybackEventHistoryConsumerStateMirror>(
            successor, successor_storage);
    auto* now_realtime_fed = playback_state<PlaybackEventHistoryPeerStateMirror>(
        successor, successor_storage);
    ASSERT_NE(now_background_fed, nullptr);
    ASSERT_NE(now_realtime_fed, nullptr);
    TickPlayback playback{successor};
    playback.publish(
        {}, {playback_events(successor, 128, 192, {150}, unindexed)});
    playback.tick(
        successor_storage,
        index(128),
        64,
        successor.root_operations.transition_tick_block != nullptr);
    EXPECT_EQ(
        recorded_times(now_background_fed->count, now_background_fed->times),
        (std::vector<std::uint64_t>{127, 150}));
    EXPECT_EQ(
        recorded_times(now_realtime_fed->count, now_realtime_fed->times),
        (std::vector<std::uint64_t>{100, 131, 191}));
}

} // namespace iv::test::graph_jit_tests
