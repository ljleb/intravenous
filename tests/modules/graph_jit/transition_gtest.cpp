#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_latent_window_compact_carry_module_id[] =
    "iv.test.graph_jit.state_context.latent_window_compact_carry_module";

constexpr char graph_jit_latent_window_ring_module_id[] =
    "iv.test.graph_jit.state_context.latent_window_ring_module";

constexpr char graph_jit_latent_window_consumer_compact_carry_module_id[] =
    "iv.test.graph_jit.state_context.latent_window_consumer_compact_carry_module";

constexpr char graph_jit_latent_window_consumer_ring_module_id[] =
    "iv.test.graph_jit.state_context.latent_window_consumer_ring_module";

constexpr char graph_jit_compensated_ramp_second_reader_module_id[] =
    "iv.test.graph_jit.state_context.compensated_ramp_second_reader_module";

constexpr char graph_jit_compensated_ramp_history_reader_module_id[] =
    "iv.test.graph_jit.state_context.compensated_ramp_history_reader_module";

constexpr char graph_jit_compensated_ramp_history_probe_module_id[] =
    "iv.test.graph_jit.state_context.compensated_ramp_history_probe_module";

constexpr char graph_jit_moving_state_module_id[] =
    "iv.test.graph_jit.state_context.moving_state_module";

constexpr char graph_jit_compensated_sequence_module_id[] =
    "iv.test.graph_jit.state_context.compensated_sequence_module";

constexpr char graph_jit_compensated_sequence_rewire_module_id[] =
    "iv.test.graph_jit.state_context.compensated_sequence_rewire_module";

constexpr char graph_jit_compensated_latent_history_probe_module_id[] =
    "iv.test.graph_jit.state_context.compensated_latent_history_probe_module";

constexpr char graph_jit_compensated_converted_history_reader_module_id[] =
    "iv.test.graph_jit.state_context.compensated_converted_history_reader_module";

constexpr char graph_jit_compensated_planar_composition_module_id[] =
    "iv.test.graph_jit.state_context.compensated_planar_composition_module";

constexpr char graph_jit_compensated_interleaved_composition_module_id[] =
    "iv.test.graph_jit.state_context.compensated_interleaved_composition_module";

constexpr char graph_jit_feedback_counter_module_id[] =
    "iv.test.graph_jit.state_context.feedback_counter_module";

constexpr char graph_jit_long_compensation_module_id[] =
    "iv.test.graph_jit.state_context.long_compensation_module";

constexpr char graph_jit_leading_node_module_id[] =
    "iv.test.graph_jit.state_context.leading_node_module";

constexpr char graph_jit_source_latency_module_id[] =
    "iv.test.graph_jit.state_context.source_latency_module";

constexpr char graph_jit_routed_feedback_module_id[] =
    "iv.test.graph_jit.state_context.routed_feedback_module";

constexpr char graph_jit_neutral_feedback_counter_module_id[] =
    "iv.test.graph_jit.state_context.neutral_feedback_counter_module";

constexpr char graph_jit_routed_rewire_module_id[] =
    "iv.test.graph_jit.state_context.routed_rewire_module";

constexpr char graph_jit_tiled_rewire_module_id[] =
    "iv.test.graph_jit.state_context.tiled_rewire_module";

constexpr char graph_jit_interleaved_mono_rewire_module_id[] =
    "iv.test.graph_jit.state_context.interleaved_mono_rewire_module";

constexpr char graph_jit_neutral_latency_module_id[] =
    "iv.test.graph_jit.state_context.neutral_latency_module";

constexpr char graph_jit_tiled_compensation_module_id[] =
    "iv.test.graph_jit.state_context.tiled_compensation_module";

constexpr char graph_jit_detach_feedback_module_id[] =
    "iv.test.graph_jit.state_context.detach_feedback_module";

constexpr char graph_jit_downmixed_rewire_module_id[] =
    "iv.test.graph_jit.state_context.downmixed_rewire_module";

constexpr char graph_jit_downmixed_tile_rewire_module_id[] =
    "iv.test.graph_jit.state_context.downmixed_tile_rewire_module";

constexpr char graph_jit_downmixed_compensation_module_id[] =
    "iv.test.graph_jit.state_context.downmixed_compensation_module";

constexpr char graph_jit_downmixed_tile_compensation_module_id[] =
    "iv.test.graph_jit.state_context.downmixed_tile_compensation_module";

constexpr char graph_jit_retained_neutral_latency_module_id[] =
    "iv.test.graph_jit.state_context.retained_neutral_latency_module";

constexpr char graph_jit_unequal_latency_composition_module_id[] =
    "iv.test.graph_jit.state_context.unequal_latency_composition_module";

constexpr char graph_jit_added_feedback_channel_module_id[] =
    "iv.test.graph_jit.state_context.added_feedback_channel_module";

constexpr char graph_jit_revising_tile_rewire_module_id[] =
    "iv.test.graph_jit.state_context.revising_tile_rewire_module";

constexpr char graph_jit_event_feedback_module_id[] =
    "iv.test.graph_jit.state_context.event_feedback_module";

constexpr char graph_jit_added_event_consumer_module_id[] =
    "iv.test.graph_jit.state_context.added_event_consumer_module";

constexpr char graph_jit_latent_direct_event_module_id[] =
    "iv.test.graph_jit.state_context.latent_direct_event_module";

constexpr char graph_jit_latent_window_growth_module_id[] =
    "iv.test.graph_jit.state_context.latent_window_growth_module";

std::vector<iv::NodeLayout::RegionHandle> sample_feedback_alignment_regions(
    iv::NodeStorage const& storage)
{
    if (!storage.layout) return {};

    std::vector<iv::NodeLayout::RegionHandle> result;
    for (std::size_t region_index = 0;
         region_index < storage.layout->regions.size(); ++region_index) {
        auto const& region = storage.layout->regions[region_index];
        if (region.kind != iv::NodeLayout::Region::Kind::raw
            || !region.migration_identity.starts_with(
                "graphjit.sample.routed_feedback:")) {
            continue;
        }
        if (region.migration_identity.find(":alignment:") == std::string::npos)
            continue;
        result.push_back(iv::NodeLayout::RegionHandle{.index = region_index});
    }
    return result;
}

struct SampleFeedbackAlignmentRegionSnapshot {
    std::string migration_identity{};
    std::vector<std::byte> bytes{};
};

std::vector<SampleFeedbackAlignmentRegionSnapshot>
snapshot_sample_feedback_alignment_regions(iv::NodeStorage const& storage)
{
    std::vector<SampleFeedbackAlignmentRegionSnapshot> result;
    for (auto const handle : sample_feedback_alignment_regions(storage)) {
        auto const bytes = storage.region_bytes(handle);
        result.push_back(SampleFeedbackAlignmentRegionSnapshot{
            .migration_identity =
                storage.layout->regions[handle.index].migration_identity,
            .bytes = std::vector<std::byte>(bytes.begin(), bytes.end()),
        });
    }
    return result;
}

void expect_sample_feedback_alignment_regions_equal(
    std::span<SampleFeedbackAlignmentRegionSnapshot const> expected,
    iv::NodeStorage const& actual)
{
    auto const actual_regions = sample_feedback_alignment_regions(actual);
    ASSERT_EQ(actual_regions.size(), expected.size());
    for (auto const& expected_region : expected) {
        auto const actual_region = std::ranges::find_if(
            actual_regions,
            [&](iv::NodeLayout::RegionHandle handle) {
                return actual.layout->regions[handle.index].migration_identity
                    == expected_region.migration_identity;
            });
        ASSERT_NE(actual_region, actual_regions.end());
        auto const actual_bytes = actual.region_bytes(*actual_region);
        ASSERT_EQ(actual_bytes.size(), expected_region.bytes.size());
        EXPECT_TRUE(std::ranges::equal(actual_bytes, expected_region.bytes));
    }
}

ConvertedSampleFeedbackStateMirror* converted_sample_feedback_state(
    iv::NodeStorage const& storage)
{
    if (!storage.layout) return nullptr;
    ConvertedSampleFeedbackStateMirror* result = nullptr;
    for (std::size_t i = 0; i < storage.layout->nodes.size(); ++i) {
        if (storage.layout->nodes[i].state_size
            != sizeof(ConvertedSampleFeedbackStateMirror)) {
            continue;
        }
        auto* candidate = static_cast<ConvertedSampleFeedbackStateMirror*>(
            storage.state_ptr(i));
        if (candidate == nullptr || candidate->marker != 0xc04e7ed1u) continue;
        if (result != nullptr) return nullptr;
        result = candidate;
    }
    return result;
}

void expect_converted_feedback_suffix_equal(
    ConvertedSampleFeedbackStateMirror const& reference,
    std::size_t reference_begin,
    ConvertedSampleFeedbackStateMirror const& actual)
{
    ASSERT_GE(reference.calls, reference_begin);
    auto const suffix_calls = reference.calls - reference_begin;
    std::size_t actual_begin = 0;
    if (actual.calls == reference.calls) {
        // A future node-state migration policy may preserve the probe state.
        // In that case compare only observations produced after migration.
        actual_begin = reference_begin;
    } else {
        ASSERT_EQ(actual.calls, suffix_calls);
    }
    ASSERT_LE(actual_begin + suffix_calls, actual.calls);
    EXPECT_EQ(actual.scc_feedback_latency, reference.scc_feedback_latency);
    EXPECT_EQ(actual.marker, reference.marker);
    for (std::size_t i = 0; i < suffix_calls; ++i) {
        auto const reference_i = reference_begin + i;
        auto const actual_i = actual_begin + i;
        EXPECT_EQ(actual.indices[actual_i], reference.indices[reference_i]);
        EXPECT_EQ(actual.block_sizes[actual_i], reference.block_sizes[reference_i]);
        EXPECT_FLOAT_EQ(
            actual.first_left[actual_i], reference.first_left[reference_i]);
        EXPECT_FLOAT_EQ(
            actual.first_right[actual_i], reference.first_right[reference_i]);
        EXPECT_FLOAT_EQ(
            actual.last_left[actual_i], reference.last_left[reference_i]);
        EXPECT_FLOAT_EQ(
            actual.last_right[actual_i], reference.last_right[reference_i]);
    }
}

struct LatentWindowSourceStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t checked = 0;
    std::uint64_t mismatches = 0;
};

struct LatentWindowConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t checked = 0;
    std::uint64_t mismatches = 0;
    std::int64_t first_position = 0;
};

struct MovingStateProbeStateMirror {
    std::uint64_t ticks = 0;
    std::uint64_t moves = 0;
    std::uint64_t marker = 0;
};

// State of RoutedHistoryProbe.
struct RoutedHistoryProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<float, 64> right{};
};

// State of InterleavedLeftProbe and ConvertedSequenceProbe: the values of the
// last 64 positions, filed by index.
struct IndexedValuesProbeStateMirror {
    std::uint64_t calls = 0;
    std::array<float, 64> values{};
};

// State of PlanarStereoAlignmentProbe and InterleavedStereoAlignmentProbe.
struct StereoAlignmentProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t mismatches = 0;
};

// State of CompensatedRampProbe and CompensatedRampHistoryProbe.
struct CompensatedRampProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t checked = 0;
    std::uint64_t mismatches = 0;
    std::int64_t first_position = 0;
    std::uint64_t read_latency = 0;
};

struct HistoryConsumerStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    float current = 0.0f;
    float history_1 = 0.0f;
    float history_5 = 0.0f;
    std::uint32_t marker = 0;
};

std::shared_ptr<iv::ConfiguredGraph const>
rewired_large_history_graph(
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor)
{
    auto successor = std::make_shared<iv::ConfiguredGraph>(*predecessor);
    auto sample_connections = std::vector<iv::ConfiguredSampleConnection>(
        predecessor->connections.configured_sample_connections().begin(),
        predecessor->connections.configured_sample_connections().end());
    if (sample_connections.size() != 2) {
        throw std::runtime_error(
            "unexpected large-history transition connection count");
    }
    auto const event_connections = std::vector<iv::ConfiguredEventConnection>(
        predecessor->connections.configured_event_connections().begin(),
        predecessor->connections.configured_event_connections().end());
    sample_connections[1].source_type = sample_connections[0].source_type;
    sample_connections[1].source_channels =
        sample_connections[0].source_channels;
    successor->connections =
        iv::GraphBuilderConnections::from_configured_connections(
            sample_connections, event_connections);
    return successor;
}

// One latent Tick sample output scenario. The source authors a window of
// history + latency values; consumer modules also read it through a
// latency-compensated input with history 4.
struct LatentSampleCutover {
    std::string_view module_id;
    iv::graph_jit::SampleRealtimePortStateStorage source_storage{};
    std::size_t source_history = 0;
    std::size_t source_latency = 0;
    bool has_consumer = false;
};

// Runs the scenario across one cutover the way GraphExecutor activates a
// compiled cutover: execute the cold transfers at the cutover position, run
// the transition root until every Sequential expiry cursor has advanced by its
// value count, then run the steady root. Every block checks every value of the
// source's owned window, and the consumer's latency-compensated view.
void expect_latent_sample_window_survives_cutover(
    iv::GraphJit& jit,
    iv::PackageRevision const& revision,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    LatentSampleCutover const& scenario,
    std::uint64_t generation)
{
    SCOPED_TRACE(std::string{scenario.module_id});
    constexpr std::size_t block = 64;
    constexpr std::int64_t consumer_history = 4;
    auto const window = scenario.source_history + scenario.source_latency;

    auto graph = configured_module_graph(revision, scenario.module_id);
    ASSERT_TRUE(graph);
    auto predecessor = jit.compile(iv::GraphJitCompileRequest{
        .project_generation = generation,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);

    // The source output owns one window, realized in the storage under test.
    auto const& requirements =
        predecessor.compiled_graph->realtime_port_state_requirements.states;
    auto const is_output = [](auto const& requirement) {
        return requirement.direction == iv::graph_jit::PortDirection::output;
    };
    EXPECT_EQ(std::ranges::count_if(requirements, is_output), 1);
    auto const output = std::ranges::find_if(requirements, is_output);
    ASSERT_NE(output, requirements.end());
    EXPECT_EQ(
        output->role,
        iv::graph_jit::RealtimePortStateRole::tick_output_history);
    EXPECT_EQ(output->extent_samples, window);
    auto const& realizations =
        predecessor.compiled_graph->realtime_port_state_realizations.sample_states;
    auto const realization = std::ranges::find(
        realizations,
        static_cast<std::size_t>(std::distance(requirements.begin(), output)),
        &iv::graph_jit::SampleRealtimePortStateRealization::requirement_index);
    ASSERT_NE(realization, realizations.end());
    ASSERT_EQ(realization->retained_sources.size(), 1u);
    auto const& retained = realization->retained_sources.front();
    EXPECT_EQ(retained.storage, scenario.source_storage);
    EXPECT_EQ(retained.timeline_offset_frames, 0);
    EXPECT_EQ(retained.carry_future_frames, 0u);
    EXPECT_EQ(realization->authored_storage.timeline_offset_frames, 0);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = generation + 1,
        .graph = graph,
        .definitions = definitions,
    };
    auto steady_successor = jit.compile(successor_request);
    ASSERT_TRUE(steady_successor.succeeded())
        << (steady_successor.diagnostics.empty()
                ? ""
                : steady_successor.diagnostics.front().message);
    auto specialized = jit.compile_transition(
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

    auto run_block = [&](iv::CompiledGraph const& compiled,
                         iv::NodeStorage& storage,
                         std::int64_t begin,
                         bool transition) {
        SCOPED_TRACE("block at " + std::to_string(begin));
        auto* source = find_unique_state<LatentWindowSourceStateMirror>(
            compiled.node_layout, storage);
        ASSERT_NE(source, nullptr);
        auto* consumer = scenario.has_consumer
            ? find_unique_state<LatentWindowConsumerStateMirror>(
                compiled.node_layout, storage)
            : nullptr;
        ASSERT_EQ(consumer != nullptr, scenario.has_consumer);
        auto const source_calls = source->calls;
        auto const consumer_calls = consumer ? consumer->calls : 0;
        auto const operation = transition
            ? compiled.root_operations.transition_tick_block
            : compiled.root_operations.tick_block;
        ASSERT_NE(operation, nullptr);
        operation(
            storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            block);

        EXPECT_EQ(source->calls, source_calls + 1);
        EXPECT_EQ(
            source->checked,
            std::min<std::uint64_t>(static_cast<std::uint64_t>(begin), window));
        EXPECT_EQ(source->mismatches, 0u);
        if (consumer == nullptr) return;
        auto const first =
            begin - static_cast<std::int64_t>(scenario.source_latency);
        auto const end = first + static_cast<std::int64_t>(block);
        auto const lowest = std::max<std::int64_t>(first - consumer_history, 0);
        EXPECT_EQ(consumer->calls, consumer_calls + 1);
        EXPECT_EQ(consumer->first_position, first);
        EXPECT_EQ(
            consumer->checked,
            static_cast<std::uint64_t>(std::max<std::int64_t>(end - lowest, 0)));
        EXPECT_EQ(consumer->mismatches, 0u);
    };

    // Fill the whole window before the cutover, and after it keep running the
    // steady root until the successor has replaced every value it inherited.
    auto const window_blocks = (window + block - 1) / block;
    auto const cutover = static_cast<std::int64_t>((window_blocks + 2) * block);

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    for (std::int64_t begin = 0; begin < cutover;
         begin += static_cast<std::int64_t>(block)) {
        run_block(*predecessor.compiled_graph, predecessor_storage, begin, false);
        if (::testing::Test::HasFatalFailure()) return;
    }

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
        index(cutover));

    auto read_cursor = [&](std::size_t offset) {
        std::uint64_t value = 0;
        std::memcpy(
            &value, successor_storage.buffer().data() + offset, sizeof(value));
        return value;
    };
    auto const& expiry =
        successor.cutover->state_transition.sample_expiry_cursors;
    auto const event_expiry =
        successor.cutover->state_transition.event_expiry_samples;
    std::vector<std::uint64_t> activation;
    for (auto const& cursor : expiry) {
        activation.push_back(read_cursor(cursor.node_storage_offset));
    }
    auto transition = !expiry.empty() || event_expiry != 0;
    std::uint64_t event_progress = 0;
    std::size_t steady_blocks = 0;
    auto begin = cutover;
    for (std::size_t step = 0;
         step < 4 * (window_blocks + 2) && steady_blocks < window_blocks + 2;
         ++step, begin += static_cast<std::int64_t>(block)) {
        run_block(successor, successor_storage, begin, transition);
        if (::testing::Test::HasFatalFailure()) return;
        if (!transition) {
            ++steady_blocks;
            continue;
        }
        event_progress += block;
        auto samples_complete = true;
        for (std::size_t cursor = 0; cursor < expiry.size(); ++cursor) {
            samples_complete = samples_complete
                && read_cursor(expiry[cursor].node_storage_offset)
                        - activation[cursor]
                    >= expiry[cursor].value_count;
        }
        transition = !(samples_complete && event_progress >= event_expiry);
    }
    EXPECT_EQ(steady_blocks, window_blocks + 2);
}

// The predecessor's sample connections, in the order its module made them.
std::vector<iv::ConfiguredSampleConnection> sample_connections_of(
    std::shared_ptr<iv::ConfiguredGraph const> const& graph,
    std::size_t connection_count)
{
    auto sample_connections = std::vector<iv::ConfiguredSampleConnection>(
        graph->connections.configured_sample_connections().begin(),
        graph->connections.configured_sample_connections().end());
    if (sample_connections.size() != connection_count) {
        throw std::runtime_error("unexpected sample connection count");
    }
    return sample_connections;
}

// A copy of the predecessor whose sample connections are replaced.
std::shared_ptr<iv::ConfiguredGraph const> with_sample_connections(
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor,
    std::vector<iv::ConfiguredSampleConnection> const& sample_connections)
{
    auto const event_connections = std::vector<iv::ConfiguredEventConnection>(
        predecessor->connections.configured_event_connections().begin(),
        predecessor->connections.configured_event_connections().end());
    auto successor = std::make_shared<iv::ConfiguredGraph>(*predecessor);
    successor->connections =
        iv::GraphBuilderConnections::from_configured_connections(
            sample_connections, event_connections);
    return successor;
}

// Copies a predecessor graph and connects one single-channel sample connection
// to the source of another. Connections are indexed in the order the module
// made them. Before the edit, `current_peer`, when given, also reads `moved`'s
// source channel, so a module whose connections change fails here instead of
// editing a different one.
std::shared_ptr<iv::ConfiguredGraph const> move_sample_connection_source(
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor,
    std::size_t connection_count,
    std::size_t moved,
    std::optional<std::size_t> current_peer,
    std::size_t new_source)
{
    auto sample_connections =
        sample_connections_of(predecessor, connection_count);
    auto const& moved_sources = sample_connections[moved].source_channels;
    if (moved_sources.size() != 1) {
        throw std::runtime_error("moved sample connection has several sources");
    }
    if (current_peer
        && std::ranges::find(
               sample_connections[*current_peer].source_channels,
               moved_sources.front().channel,
               &iv::ConfiguredSampleConnectionSource::channel)
            == sample_connections[*current_peer].source_channels.end()) {
        throw std::runtime_error("unexpected sample connection sources");
    }
    if (moved_sources == sample_connections[new_source].source_channels) {
        throw std::runtime_error("moved sample connection already reads that source");
    }
    sample_connections[moved].source_type =
        sample_connections[new_source].source_type;
    sample_connections[moved].source_channels =
        sample_connections[new_source].source_channels;
    return with_sample_connections(predecessor, sample_connections);
}

// Copies a predecessor graph and connects one mono sample connection to mono
// output 0 of the node that connection `targeting` feeds.
std::shared_ptr<iv::ConfiguredGraph const>
connect_sample_connection_to_target_output(
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor,
    std::size_t connection_count,
    std::size_t moved,
    std::size_t targeting)
{
    auto sample_connections =
        sample_connections_of(predecessor, connection_count);
    auto const& targets = sample_connections[targeting].target_channels;
    auto& sources = sample_connections[moved].source_channels;
    if (targets.size() != 1 || sources.size() != 1) {
        throw std::runtime_error("unexpected sample connection channel count");
    }
    auto const output = iv::SampleOutputChannelId{
        .bundle = targets.front().bundle,
        .port = 0,
        .channel = 0,
    };
    if (sources.front().channel == output) {
        throw std::runtime_error("sample connection already reads that output");
    }
    sample_connections[moved].source_type = iv::ChannelTypeId::mono;
    sources = {iv::ConfiguredSampleConnectionSource{.channel = output}};
    return with_sample_connections(predecessor, sample_connections);
}

// Runs a graph edit that keeps a CompensatedRampProbe or
// CompensatedRampHistoryProbe and both of its connections. After every block,
// before and after the edit, the probe's ramp input must read each ramp value
// at its own position, `read_latency` values late. Over a ramp source with no
// latency that is 7: the delay node feeding the probe's slower input has
// internal latency 5 and output latency 2.
void expect_compensated_ramp_probe_reads_positions_across_graph_edit(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor_graph,
    std::shared_ptr<iv::ConfiguredGraph const> const& successor_graph,
    std::int64_t probe_history,
    std::int64_t read_latency,
    std::uint64_t generation)
{
    constexpr std::int64_t block = 64;
    constexpr std::int64_t cutover = 16 * block;
    // Long enough for a transition realization that carries the history
    // reader's 5000 values to expire, followed by steady blocks.
    constexpr std::int64_t end = cutover + 100 * block;
    run_across_graph_edit(
        jit, definitions, resources, predecessor_graph, successor_graph,
        generation, cutover, end,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            auto const* probe = find_unique_state<CompensatedRampProbeStateMirror>(
                compiled.node_layout, storage);
            ASSERT_NE(probe, nullptr);
            auto const first = begin - read_latency;
            auto const lowest = std::max<std::int64_t>(first - probe_history, 0);
            EXPECT_EQ(probe->read_latency, static_cast<std::uint64_t>(read_latency));
            EXPECT_EQ(probe->first_position, first);
            EXPECT_EQ(
                probe->checked,
                static_cast<std::uint64_t>(
                    std::max<std::int64_t>(first + block - lowest, 0)));
            EXPECT_EQ(probe->mismatches, 0u);
        });
}

// Runs a graph edit around a CompensatedSequenceProbe or SampleSequenceProbe and
// checks every value its recorded input reads, before and after the cutover at
// sequence_cutover, against `expected(position)`. The input reads
// `predecessor_read_latency` values late before the edit and
// `successor_read_latency` after it.
template<class Expected>
void expect_probe_sequence_across_graph_edit(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor_graph,
    std::shared_ptr<iv::ConfiguredGraph const> const& successor_graph,
    std::uint64_t predecessor_read_latency,
    std::uint64_t successor_read_latency,
    Expected const& expected,
    std::uint64_t generation)
{
    constexpr std::int64_t block = 64;
    std::size_t mismatches = 0;
    run_across_graph_edit(
        jit, definitions, resources, predecessor_graph, successor_graph,
        generation, sequence_cutover, sequence_cutover + 16 * block,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            auto const* probe =
                find_unique_state<SequenceProbeStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(probe, nullptr);
            EXPECT_EQ(probe->first_index, begin);
            EXPECT_EQ(
                probe->read_latency,
                begin < sequence_cutover
                    ? predecessor_read_latency
                    : successor_read_latency);
            ASSERT_EQ(probe->recorded, static_cast<std::uint64_t>(block));
            for (std::int64_t offset = 0; offset < block; ++offset) {
                auto const position = begin + offset;
                auto const value = probe->values[static_cast<std::size_t>(offset)];
                auto const wanted = expected(position);
                if (value == wanted) continue;
                if (mismatches++ < 8) {
                    ADD_FAILURE() << "position " << position << " read " << value
                                  << ", expected " << wanted;
                }
            }
        });
    EXPECT_EQ(mismatches, 0u) << "positions that read an unexpected value";
}

// Runs the edit that moves a composition module's history reader from the delay
// node to the ramp, and checks after every block that the stereo probe's two
// channels have never differed.
void expect_composed_channels_aligned_across_reader_move(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    iv::PackageRevision const& revision,
    std::string_view module_id,
    std::uint64_t generation)
{
    auto predecessor = configured_module_graph(revision, module_id);
    ASSERT_TRUE(predecessor);
    // Module order: source -> delayed, (source, delayed) -> probe.in,
    // delayed -> history_reader.in.
    auto const successor =
        move_sample_connection_source(predecessor, 3, 2, 1, 0);
    constexpr std::int64_t block = 64;
    run_across_graph_edit(
        jit, definitions, resources, predecessor, successor, generation,
        16 * block, 116 * block,
        [](iv::CompiledGraph const& compiled,
           iv::NodeStorage const& storage,
           std::int64_t) {
            auto const* probe =
                find_unique_state<StereoAlignmentProbeStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(probe, nullptr);
            EXPECT_GT(probe->calls, 0u);
            EXPECT_EQ(probe->mismatches, 0u);
        });
}

// Activates the successor of a feedback graph at index 64 and checks that its
// converted node reads exactly what it reads in an uninterrupted run of the
// predecessor. Every predecessor node survives under its identity.
void expect_converted_feedback_unaffected_by_edit(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor,
    std::shared_ptr<iv::ConfiguredGraph const> const& successor,
    std::uint64_t generation)
{
    ASSERT_TRUE(predecessor);
    ASSERT_TRUE(successor);
    ASSERT_TRUE(std::ranges::includes(
        stable_node_identities(*successor),
        stable_node_identities(*predecessor)))
        << "every predecessor node should survive under its identity";
    constexpr std::int64_t block = 64;
    constexpr std::int64_t cutover = 64;
    constexpr std::int64_t end = 128;

    // The uninterrupted reference keeps every composition on
    // compose_samples(). The edited execution uses the fixture GraphJit and
    // therefore exercises generated compositions in eligible steady regions.
    iv::GraphJit reference_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .specialize_regions = false,
    });
    auto reference = reference_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = generation + 2,
        .graph = predecessor,
        .definitions = definitions,
    });
    ASSERT_TRUE(reference.succeeded())
        << (reference.diagnostics.empty()
                ? ""
                : reference.diagnostics.front().message);
    auto reference_storage =
        reference.compiled_graph->node_layout.create_storage(resources);
    reference_storage.initialize();
    for (std::int64_t begin = 0; begin < end; begin += block) {
        reference.compiled_graph->root_operations.tick_block(
            reference_storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            block);
    }
    auto const* expected = converted_sample_feedback_state(reference_storage);
    ASSERT_NE(expected, nullptr);

    ConvertedSampleFeedbackStateMirror actual{};
    run_across_graph_edit(
        jit, definitions, resources, predecessor, successor, generation,
        cutover, end,
        [&](auto const&, auto const& storage, std::int64_t) {
            auto const* state = converted_sample_feedback_state(storage);
            ASSERT_NE(state, nullptr);
            actual = *state;
        });
    if (::testing::Test::HasFatalFailure()) return;

    ASSERT_EQ(actual.calls, expected->calls);
    auto const recorded = std::min<std::size_t>(
        static_cast<std::size_t>(actual.calls), actual.indices.size());
    for (std::size_t call = 0; call < recorded; ++call) {
        SCOPED_TRACE("call at " + std::to_string(expected->indices[call]));
        EXPECT_EQ(actual.indices[call], expected->indices[call]);
        EXPECT_EQ(actual.block_sizes[call], expected->block_sizes[call]);
        EXPECT_FLOAT_EQ(actual.first_left[call], expected->first_left[call]);
        EXPECT_FLOAT_EQ(actual.first_right[call], expected->first_right[call]);
        EXPECT_FLOAT_EQ(actual.last_left[call], expected->last_left[call]);
        EXPECT_FLOAT_EQ(actual.last_right[call], expected->last_right[call]);
    }
}

// Activates a recompiled, unchanged two-node feedback graph.
void expect_converted_feedback_unaffected_by_recompile(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    std::shared_ptr<iv::ConfiguredGraph const> const& graph,
    std::uint64_t generation)
{
    ASSERT_TRUE(graph);
    ASSERT_EQ(stable_node_identities(*graph).size(), 2u);
    expect_converted_feedback_unaffected_by_edit(
        jit, definitions, resources, graph, graph, generation);
}

// Runs a graph edit with its cutover at sequence_cutover around the one node
// whose State is an IndexedValuesProbeStateMirror. After every block, checks
// each value it filed for that block's positions against `expected(position)`.
template<class Expected>
void expect_indexed_probe_values_across_graph_edit(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    std::shared_ptr<iv::ConfiguredGraph const> const& predecessor,
    std::shared_ptr<iv::ConfiguredGraph const> const& successor,
    Expected const& expected,
    std::uint64_t generation)
{
    ASSERT_TRUE(predecessor);
    ASSERT_TRUE(successor);
    ASSERT_EQ(
        stable_node_identities(*successor), stable_node_identities(*predecessor));
    constexpr std::int64_t block = 64;
    std::size_t mismatches = 0;
    run_across_graph_edit(
        jit, definitions, resources, predecessor, successor, generation,
        sequence_cutover, sequence_cutover + 4 * block,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            auto const* probe = find_unique_state<IndexedValuesProbeStateMirror>(
                compiled.node_layout, storage);
            ASSERT_NE(probe, nullptr);
            for (std::int64_t offset = 0; offset < block; ++offset) {
                auto const position = begin + offset;
                auto const value = probe->values[
                    static_cast<std::size_t>(position) % probe->values.size()];
                auto const wanted = expected(position);
                if (value == wanted) continue;
                if (mismatches++ < 8) {
                    ADD_FAILURE() << "position " << position << " read " << value
                                  << ", expected " << wanted;
                }
            }
        });
    EXPECT_EQ(mismatches, 0u) << "positions that read an unexpected value";
}

// Runs latent_direct_event_module with latency `before`, then with latency
// `after` from a cutover at 64, and returns the consumer's State.
RetainedEventConsumerProbeStateMirror run_latent_direct_event_edit(
    iv::GraphJit& jit,
    std::shared_ptr<iv::NodeDefinitionsSnapshot const> const& definitions,
    iv::ResourceContext const& resources,
    iv::PackageRevision const& revision,
    std::size_t before,
    std::size_t after,
    std::uint64_t generation)
{
    std::array predecessor_arguments{iv::details::configuration_argument(before)};
    std::array successor_arguments{iv::details::configuration_argument(after)};
    auto const predecessor = configured_module_graph_with_arguments(
        revision, graph_jit_latent_direct_event_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        revision, graph_jit_latent_direct_event_module_id, successor_arguments);
    RetainedEventConsumerProbeStateMirror consumer{};
    run_across_graph_edit(
        jit, definitions, resources, predecessor, successor, generation, 64, 192,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t) {
            auto const* state =
                find_unique_state<RetainedEventConsumerProbeStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(state, nullptr);
            consumer = *state;
        });
    return consumer;
}

} // namespace

TEST_F(GraphJitRuntimeFixture,
       CompilerOwnedActivityStateMigratesAcrossCompatibleGenerations)
{
    auto predecessor = compile(
        graph_jit_activity_type_default_module_id, 256);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    auto* predecessor_state = static_cast<ActivityProbeStateMirror*>(
        predecessor_storage.state_ptr(1));
    ASSERT_NE(predecessor_state, nullptr);

    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    EXPECT_EQ(predecessor_state->tick_calls, 1u);
    EXPECT_EQ(predecessor_state->skip_calls, 0u);

    auto successor = compile(
        graph_jit_activity_type_default_module_id, 257);
    ASSERT_TRUE(successor.succeeded())
        << (successor.diagnostics.empty()
                ? ""
                : successor.diagnostics.front().message);
    auto successor_storage = successor.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto* successor_state = static_cast<ActivityProbeStateMirror*>(
        successor_storage.state_ptr(1));
    ASSERT_NE(successor_state, nullptr);

    successor.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);
    // ActivityProbe::State is moved implicitly, so the predecessor's tick
    // carries over. The compiler-owned activity region migrates independently,
    // so the successor immediately skips.
    EXPECT_EQ(successor_state->tick_calls, 1u);
    EXPECT_EQ(successor_state->skip_calls, 1u);
}

TEST_F(GraphJitRuntimeFixture, CutoverTransfersSampleOutputWindowIntoSteadyCarry)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_disconnected_sample_output_module_id);
    ASSERT_TRUE(graph);
    auto predecessor = compile_graph(graph, 242);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    EXPECT_EQ(
        std::ranges::count_if(
            predecessor.compiled_graph->realtime_port_state_requirements.states,
            [](auto const& requirement) {
                return requirement.direction
                        == iv::graph_jit::PortDirection::output
                    && requirement.stable_identity.has_value();
            }),
        1);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 243,
        .graph = graph,
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
    // The output's one window is the only user of its compact carry, so the
    // cutover writes the whole history 3 + latency 2 window into the steady
    // successor carry. No transition root or expiry horizon is needed.
    ASSERT_TRUE(specialized.compiled_graph->cutover);
    EXPECT_EQ(
        specialized.compiled_graph->root_operations.transition_tick_block,
        nullptr);
    EXPECT_TRUE(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.empty());
    ASSERT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_transfers.size(),
        1u);
    auto const& transfer = specialized.compiled_graph->cutover
        ->state_transition.sample_transfers.front();
    EXPECT_FALSE(transfer.target_transition_only);
    EXPECT_EQ(transfer.target_begin, -5);
    EXPECT_EQ(transfer.target_end, 0);
    ASSERT_EQ(transfer.source_storage.size(), 1u);
    EXPECT_EQ(
        transfer.source_storage.front().storage,
        iv::graph_jit::SampleRealtimePortStateStorage::compact_carry);
    EXPECT_EQ(transfer.source_storage.front().timeline_offset_frames, 0);
    EXPECT_EQ(
        transfer.target_storage.storage,
        iv::graph_jit::SampleRealtimePortStateStorage::compact_carry);
    EXPECT_EQ(transfer.target_storage.timeline_offset_frames, 0);

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);

    auto successor_storage = specialized.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        specialized.compiled_graph->cutover->state_transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(128));

    auto* state = static_cast<HistoryRampSourceStateMirror*>(
        successor_storage.state_ptr(0));
    ASSERT_NE(state, nullptr);
    specialized.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(128),
        64);
    EXPECT_FLOAT_EQ(state->previous_output, 127.0f);
    specialized.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(192),
        64);
    EXPECT_FLOAT_EQ(state->previous_output, 191.0f);
}

TEST_F(GraphJitRuntimeFixture, LatentSampleOutputWindowSurvivesCutoverInCompactCarry)
{
    expect_latent_sample_window_survives_cutover(
        *jit, *revision, definitions, resources,
        LatentSampleCutover{
            .module_id = graph_jit_latent_window_compact_carry_module_id,
            .source_storage =
                iv::graph_jit::SampleRealtimePortStateStorage::compact_carry,
            .source_history = 3,
            .source_latency = 2,
        },
        300);
}

TEST_F(GraphJitRuntimeFixture, LatentSampleOutputWindowSurvivesCutoverInRing)
{
    expect_latent_sample_window_survives_cutover(
        *jit, *revision, definitions, resources,
        LatentSampleCutover{
            .module_id = graph_jit_latent_window_ring_module_id,
            .source_storage = iv::graph_jit::SampleRealtimePortStateStorage::ring,
            .source_history = 4000,
            .source_latency = 1000,
        },
        302);
}

TEST_F(GraphJitRuntimeFixture, LatentSampleConsumerStaysFinalAcrossCutoverInCompactCarry)
{
    expect_latent_sample_window_survives_cutover(
        *jit, *revision, definitions, resources,
        LatentSampleCutover{
            .module_id = graph_jit_latent_window_consumer_compact_carry_module_id,
            .source_storage =
                iv::graph_jit::SampleRealtimePortStateStorage::compact_carry,
            .source_history = 3,
            .source_latency = 2,
            .has_consumer = true,
        },
        304);
}

TEST_F(GraphJitRuntimeFixture, LatentSampleConsumerStaysFinalAcrossCutoverInRing)
{
    expect_latent_sample_window_survives_cutover(
        *jit, *revision, definitions, resources,
        LatentSampleCutover{
            .module_id = graph_jit_latent_window_consumer_ring_module_id,
            .source_storage = iv::graph_jit::SampleRealtimePortStateStorage::ring,
            .source_history = 4000,
            .source_latency = 1000,
            .has_consumer = true,
        },
        306);
}

// The probe's ramp input and both of its connections are the same in both
// revisions. The edit connects another node's input to the same ramp source.
TEST_F(GraphJitRuntimeFixture, CompensatedInputUnaffectedBySecondReaderMove)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_compensated_ramp_second_reader_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: source -> delayed, delayed -> twice_delayed,
    // source -> probe.ramp, delayed -> probe.slower,
    // delayed -> second_reader.fast, twice_delayed -> second_reader.slow.
    auto const successor =
        move_sample_connection_source(predecessor, 6, 4, 3, 2);
    expect_compensated_ramp_probe_reads_positions_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 0, 7, 320);
}

// As above, but the input that moves to the ramp source keeps 5000 values of
// history, which stay its own across the edit.
TEST_F(GraphJitRuntimeFixture, CompensatedInputUnaffectedByHistoryReaderMove)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_compensated_ramp_history_reader_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: source -> delayed, source -> probe.ramp,
    // delayed -> probe.slower, delayed -> history_reader.in.
    auto const successor =
        move_sample_connection_source(predecessor, 4, 3, 2, 1);
    expect_compensated_ramp_probe_reads_positions_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 0, 7, 322);
}

// As above, and the probe's ramp input also keeps 4 values of history.
TEST_F(GraphJitRuntimeFixture, CompensatedHistoryInputUnaffectedByHistoryReaderMove)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_compensated_ramp_history_probe_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: source -> delayed, source -> probe.ramp,
    // delayed -> probe.slower, delayed -> history_reader.in.
    auto const successor =
        move_sample_connection_source(predecessor, 4, 3, 2, 1);
    expect_compensated_ramp_probe_reads_positions_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 4, 7, 324);
}

// As above, over a latent source with history of its own. The probe's ramp
// input reads it 9 values late: the source's latency 2 plus 7 of compensation.
TEST_F(GraphJitRuntimeFixture,
       CompensatedHistoryInputOfLatentSourceUnaffectedByHistoryReaderMove)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_compensated_latent_history_probe_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: source -> delayed, source -> probe.ramp,
    // delayed -> probe.slower, delayed -> history_reader.in.
    auto const successor =
        move_sample_connection_source(predecessor, 4, 3, 2, 1);
    expect_compensated_ramp_probe_reads_positions_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 4, 9, 326);
}

// The reverse of CompensatedInputUnaffectedByHistoryReaderMove: the history
// reader leaves the probe's ramp source for the delay node, so the source keeps
// fewer values than before.
TEST_F(GraphJitRuntimeFixture,
       CompensatedInputUnaffectedByHistoryReaderLeavingItsSource)
{
    auto const module = configured_module_graph(
        *revision, graph_jit_compensated_ramp_history_reader_module_id);
    ASSERT_TRUE(module);
    // Module order: source -> delayed, source -> probe.ramp,
    // delayed -> probe.slower, delayed -> history_reader.in.
    auto const predecessor = move_sample_connection_source(module, 4, 3, 2, 1);
    expect_compensated_ramp_probe_reads_positions_across_graph_edit(
        *jit, definitions, resources, predecessor, module, 0, 7, 328);
}

// As CompensatedInputUnaffectedByHistoryReaderMove, but the compensated input
// converts mono to interleaved mono. InterleavedLatencyCompensationProbe
// compares it with its slower input at every value.
TEST_F(GraphJitRuntimeFixture,
       ConvertedCompensatedInputUnaffectedByHistoryReaderMove)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_compensated_converted_history_reader_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: source -> delayed, source -> probe.fast,
    // delayed -> probe.slow, delayed -> history_reader.in.
    auto const successor =
        move_sample_connection_source(predecessor, 4, 3, 2, 1);
    constexpr std::int64_t block = 64;
    run_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 330,
        16 * block, 116 * block,
        [](iv::CompiledGraph const& compiled,
           iv::NodeStorage const& storage,
           std::int64_t) {
            auto const* probe =
                find_unique_state<LatencyCompensationProbeStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(probe, nullptr);
            EXPECT_EQ(probe->marker, 0x1a7e2e0u);
            EXPECT_EQ(probe->mismatches, 0u);
        });
}

// The probe's ramp input stays connected to its source. The edit moves the
// probe's slower input to a path 7 values slower, so the ramp input's
// compensation grows from 7 to 14. It reads its 7 in-flight values, then 7
// fresh values, then the ramp continues: no value twice, none lost.
TEST_F(GraphJitRuntimeFixture,
       GrowingCompensationAddsFreshValuesAfterInFlightValues)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_compensated_sequence_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: source -> delayed, delayed -> twice_delayed,
    // source -> probe.ramp, delayed -> probe.slower.
    auto const successor =
        connect_sample_connection_to_target_output(predecessor, 4, 3, 1);
    expect_probe_sequence_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 7, 14,
        [](std::int64_t position) {
            if (position < sequence_cutover + 7) return ramp_value(position - 7);
            if (position < sequence_cutover + 14) return 0.0f;
            return ramp_value(position - 14);
        },
        332);
}

// The reverse edit: the ramp input's compensation shrinks from 14 to 7. It keeps
// its oldest 7 in-flight values, drops the newest 7, then the ramp continues.
TEST_F(GraphJitRuntimeFixture,
       ShrinkingCompensationDropsTheNewestInFlightValues)
{
    auto const module = configured_module_graph(
        *revision, graph_jit_compensated_sequence_module_id);
    ASSERT_TRUE(module);
    // Module order: source -> delayed, delayed -> twice_delayed,
    // source -> probe.ramp, delayed -> probe.slower.
    auto const predecessor =
        connect_sample_connection_to_target_output(module, 4, 3, 1);
    expect_probe_sequence_across_graph_edit(
        *jit, definitions, resources, predecessor, module, 14, 7,
        [](std::int64_t position) {
            if (position < sequence_cutover + 7) return ramp_value(position - 14);
            return ramp_value(position - 7);
        },
        334);
}

// The edit rewires the probe's ramp input to the offset ramp, which has the same
// latency, so its compensation stays 7. The input first reads its 7 in-flight
// values from the old ramp, then the offset ramp's values from the cutover on.
TEST_F(GraphJitRuntimeFixture,
       RewiredCompensatedInputReadsItsInFlightValuesFirst)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_compensated_sequence_rewire_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: source -> delayed, offset_source -> offset_delayed,
    // source -> probe.ramp, delayed -> probe.slower.
    auto const successor =
        move_sample_connection_source(predecessor, 4, 2, 0, 1);
    expect_probe_sequence_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 7, 7,
        [](std::int64_t position) {
            if (position < sequence_cutover + 7) return ramp_value(position - 7);
            return static_cast<float>(position - 7 + 100000);
        },
        336);
}

// The probe's planar stereo input composes the ramp, delayed by 7 to align it,
// with the delay node's output. The edit moves the history reader from the
// delay node to the ramp. The two channels must stay equal at every value.
TEST_F(GraphJitRuntimeFixture,
       ComposedCompensatedChannelUnaffectedByHistoryReaderMove)
{
    expect_composed_channels_aligned_across_reader_move(
        *jit, definitions, resources, *revision,
        graph_jit_compensated_planar_composition_module_id, 338);
}

// As above, with an interleaved stereo input.
TEST_F(GraphJitRuntimeFixture,
       InterleavedComposedCompensatedChannelUnaffectedByHistoryReaderMove)
{
    expect_composed_channels_aligned_across_reader_move(
        *jit, definitions, resources, *revision,
        graph_jit_compensated_interleaved_composition_module_id, 340);
}

// The counter reads its own output through a detach of latency 5, so it outputs
// floor(position / 5) + 1. The edit moves the history reader from the ramp to the
// counter's output. The counter's output must continue as if there were no edit.
TEST_F(GraphJitRuntimeFixture, FeedbackLoopUnaffectedByHistoryReaderMove)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_feedback_counter_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: counter -> observer.in, source -> history_reader.in,
    // counter -> counter.in (detached).
    auto const successor =
        move_sample_connection_source(predecessor, 3, 1, std::nullopt, 0);
    expect_probe_sequence_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 0, 0,
        [](std::int64_t position) {
            return static_cast<float>(position / 5 + 1);
        },
        342);
}

// The probe's ramp input reads its latent source 1000 values late, so its
// in-flight values span 16 blocks. The source's window and the probe's history
// share the source's storage, so the cutover runs a transition root whose own
// records expire after a few values. The edit moves the second reader's fast
// input to the probe's source.
TEST_F(GraphJitRuntimeFixture,
       LongCompensationInFlightValuesOutlastTheTransitionRoot)
{
    auto predecessor = configured_module_graph(
        *revision, graph_jit_long_compensation_module_id);
    ASSERT_TRUE(predecessor);
    // Module order: slow_source -> slow_delayed, source -> probe.ramp,
    // slow_source -> probe.slower, slow_source -> second_reader.fast,
    // slow_delayed -> second_reader.slow.
    auto const successor =
        move_sample_connection_source(predecessor, 5, 3, 2, 1);
    expect_compensated_ramp_probe_reads_positions_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 4, 1000, 344);
}

// The successor adds a node that nothing reads before the module's other
// nodes. That renumbers their bundles but changes nothing they read.
TEST_F(GraphJitRuntimeFixture, CompensatedInputUnaffectedByAddedLeadingNode)
{
    bool without_leading = false;
    bool with_leading = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(without_leading)};
    std::array successor_arguments{
        iv::details::configuration_argument(with_leading)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_leading_node_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_leading_node_module_id, successor_arguments);
    auto const predecessor_nodes = stable_node_identities(*predecessor);
    auto const successor_nodes = stable_node_identities(*successor);
    ASSERT_EQ(successor_nodes.size(), predecessor_nodes.size() + 1);
    ASSERT_TRUE(std::ranges::includes(successor_nodes, predecessor_nodes))
        << "every predecessor node should survive under its identity";
    expect_compensated_ramp_probe_reads_positions_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 0, 7, 346);
}

// The successor gives the probe's ramp source latency 3. The probe's slower
// input reads that source through the delay node, so the ramp input's
// compensation stays 7 while it now reads the ramp 10 values late. It reads its
// 7 in-flight values, then 3 fresh values, then the ramp continues.
TEST_F(GraphJitRuntimeFixture,
       GrowingSourceLatencyAddsFreshValuesAfterInFlightValues)
{
    std::size_t predecessor_latency = 0;
    std::size_t successor_latency = 3;
    std::array predecessor_arguments{
        iv::details::configuration_argument(predecessor_latency)};
    std::array successor_arguments{
        iv::details::configuration_argument(successor_latency)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_source_latency_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_source_latency_module_id, successor_arguments);
    ASSERT_EQ(
        stable_node_identities(*predecessor),
        stable_node_identities(*successor))
        << "every node should survive under its identity";
    expect_probe_sequence_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 7, 10,
        [](std::int64_t position) {
            if (position < sequence_cutover + 7) return ramp_value(position - 7);
            if (position < sequence_cutover + 10) return 0.0f;
            return ramp_value(position - 10);
        },
        348);
}

// Each node of a module graph is a stable concrete node, so a node that
// authors move() continues its own State in the next compiled generation,
// including when the graph holds several nodes of its type.
TEST_F(GraphJitRuntimeFixture, AuthoredStateMovesToTheSameNodeInTheNextGeneration)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_moving_state_module_id);
    ASSERT_TRUE(graph);
    auto predecessor = compile_graph(graph, 330);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    auto successor = compile_graph(graph, 331);
    ASSERT_TRUE(successor.succeeded())
        << (successor.diagnostics.empty()
                ? ""
                : successor.diagnostics.front().message);

    // Probe State keyed by the node's migration identity.
    auto probes = [](iv::CompiledGraph const& compiled,
                     iv::NodeStorage const& storage) {
        std::map<std::string, MovingStateProbeStateMirror*> result;
        auto const& nodes = compiled.node_layout.nodes;
        for (std::size_t node = 0; node < nodes.size(); ++node) {
            if (nodes[node].state_size != sizeof(MovingStateProbeStateMirror)) {
                continue;
            }
            EXPECT_FALSE(nodes[node].migration_identity.empty());
            auto const inserted = result.emplace(
                nodes[node].migration_identity,
                static_cast<MovingStateProbeStateMirror*>(
                    storage.state_ptr(node))).second;
            EXPECT_TRUE(inserted) << "two probes share a migration identity";
        }
        return result;
    };

    constexpr std::size_t block = 64;
    constexpr std::uint64_t predecessor_blocks = 4;
    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    for (std::uint64_t call = 0; call < predecessor_blocks; ++call) {
        predecessor.compiled_graph->root_operations.tick_block(
            predecessor_storage.buffer().data(),
            &empty_tick_invocation,
            index(static_cast<std::int64_t>(call * block)),
            block);
    }
    auto const before = probes(*predecessor.compiled_graph, predecessor_storage);
    ASSERT_EQ(before.size(), 2u);
    std::uint64_t marker = 0;
    for (auto const& [identity, state] : before) {
        EXPECT_EQ(state->ticks, predecessor_blocks);
        EXPECT_EQ(state->moves, 0u);
        state->marker = ++marker;
    }

    auto successor_storage = successor.compiled_graph
        ->node_layout.create_storage(resources);
    auto migration = successor_storage.prepare_migration_from(
        predecessor_storage);
    migration.commit();
    successor.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(static_cast<std::int64_t>(predecessor_blocks * block)),
        block);

    auto const after = probes(*successor.compiled_graph, successor_storage);
    ASSERT_EQ(after.size(), 2u);
    for (auto const& [identity, state] : after) {
        SCOPED_TRACE(identity);
        auto const previous = before.find(identity);
        ASSERT_NE(previous, before.end());
        EXPECT_EQ(state->marker, previous->second->marker);
        EXPECT_EQ(state->moves, 1u);
        EXPECT_EQ(state->ticks, predecessor_blocks + 1);
    }
}

// The converted history consumer keeps its history in its source's storage,
// which pairs with the predecessor's by producer, so the cutover transfers the
// history there and needs no transition root.
TEST_F(GraphJitRuntimeFixture, CutoverTransfersMaterializedSampleHistoryThroughItsSource)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_stereo_history_conversion_module_id);
    ASSERT_TRUE(graph);
    auto predecessor = compile_graph(graph, 244);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 245,
        .graph = graph,
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
    ASSERT_TRUE(specialized.compiled_graph->cutover);
    EXPECT_EQ(
        specialized.compiled_graph->root_operations.transition_tick_block,
        nullptr);
    EXPECT_TRUE(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.empty());

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(0),
        64);
    predecessor.compiled_graph->root_operations.tick_block(
        predecessor_storage.buffer().data(),
        &empty_tick_invocation,
        index(64),
        64);

    auto successor_storage = specialized.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        specialized.compiled_graph->cutover->state_transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(128));
    specialized.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(128),
        64);

    auto reference_storage = steady_successor.compiled_graph
        ->node_layout.create_storage(resources);
    reference_storage.initialize(&predecessor_storage);
    steady_successor.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(),
        &empty_tick_invocation,
        index(128),
        64);

    HistoryConsumerStateMirror* successor_state = nullptr;
    for (std::size_t node = 0;
         node < specialized.compiled_graph->node_layout.nodes.size(); ++node) {
        if (specialized.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(HistoryConsumerStateMirror)) {
            successor_state = static_cast<HistoryConsumerStateMirror*>(
                successor_storage.state_ptr(node));
        }
    }
    HistoryConsumerStateMirror* reference_state = nullptr;
    for (std::size_t node = 0;
         node < steady_successor.compiled_graph->node_layout.nodes.size(); ++node) {
        if (steady_successor.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(HistoryConsumerStateMirror)) {
            reference_state = static_cast<HistoryConsumerStateMirror*>(
                reference_storage.state_ptr(node));
        }
    }
    ASSERT_NE(successor_state, nullptr);
    ASSERT_NE(reference_state, nullptr);
    EXPECT_FLOAT_EQ(successor_state->current, reference_state->current);
    EXPECT_FLOAT_EQ(successor_state->history_1, reference_state->history_1);
    EXPECT_FLOAT_EQ(successor_state->history_5, reference_state->history_5);
}

TEST_F(GraphJitRuntimeFixture, MultipleSampleDetachBranchesMigrateSharedHomeAndFallbackCopy)
{
    // Its node needs an identity for its State and storage to migrate.
    auto feedback_graph = with_node_identities(
        configured_multi_branch_sample_feedback_graph(*revision));
    ASSERT_TRUE(feedback_graph);

    auto current = compile_graph(feedback_graph, 136);
    ASSERT_TRUE(current.succeeded())
        << (current.diagnostics.empty()
                ? ""
                : current.diagnostics.front().message);
    auto storage = current.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    current.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 13);

    // The two zero-initialized detach branches share one producer-home ring,
    // while the non-zero branch owns a second branch-local ring. Snapshot both
    // retained timelines before migration so we prove they migrate
    // independently rather than merely checking the post-migration waveform.
    struct RetainedRegionSnapshot {
        std::string identity;
        std::vector<std::byte> bytes;
    };
    auto retained_regions = [](iv::NodeStorage const& node_storage) {
        std::vector<RetainedRegionSnapshot> result;
        if (!node_storage.layout) return result;
        for (std::size_t i = 0; i < node_storage.layout->regions.size(); ++i) {
            auto const& region = node_storage.layout->regions[i];
            if (region.kind != iv::NodeLayout::Region::Kind::raw
                || !(region.migration_identity.starts_with("graphjit.sample:")
                    || region.migration_identity.starts_with(
                        "graphjit.sample.feedback:"))) {
                continue;
            }
            iv::NodeLayout::RegionHandle handle{.index = i};
            auto const bytes = node_storage.region_bytes(handle);
            result.push_back(RetainedRegionSnapshot{
                .identity = region.migration_identity,
                .bytes = std::vector<std::byte>(bytes.begin(), bytes.end()),
            });
        }
        std::ranges::sort(
            result, {}, &RetainedRegionSnapshot::identity);
        return result;
    };

    auto const before = retained_regions(storage);
    ASSERT_EQ(before.size(), 2u);
    EXPECT_EQ(
        std::ranges::count_if(before, [](auto const& region) {
            return region.identity.starts_with("graphjit.sample:");
        }),
        1u);
    EXPECT_EQ(
        std::ranges::count_if(before, [](auto const& region) {
            return region.identity.starts_with("graphjit.sample.feedback:");
        }),
        1u);

    auto const calls_before = static_cast<MultiBranchSampleFeedbackStateMirror*>(
        storage.state_ptr(0))->calls;
    ASSERT_LE(calls_before + 2u, 24u);

    auto migrated = compile_graph(feedback_graph, 137);
    ASSERT_TRUE(migrated.succeeded())
        << (migrated.diagnostics.empty()
                ? ""
                : migrated.diagnostics.front().message);
    auto migrated_storage =
        migrated.compiled_graph->node_layout.create_storage(resources);
    migrated_storage.initialize(&storage);

    auto const after = retained_regions(migrated_storage);
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(after[i].identity, before[i].identity);
        EXPECT_EQ(after[i].bytes, before[i].bytes);
    }

    auto* state = static_cast<MultiBranchSampleFeedbackStateMirror*>(
        migrated_storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    // Resume at an awkward absolute index. The shared producer-home timeline
    // must continue both zero-initialized branches, while the independently
    // migrated initialized timeline must continue without replaying its -2 pre-roll.
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(13), 6);

    // The node keeps its State: the two calls follow its earlier ones.
    ASSERT_EQ(state->calls, calls_before + 2u);
    EXPECT_EQ(state->scc_feedback_latency, 4u);
    EXPECT_EQ(state->marker, 0x6d756c74u);

    EXPECT_EQ(state->indices[calls_before], 13u);
    EXPECT_EQ(state->block_sizes[calls_before], 4u);
    EXPECT_FLOAT_EQ(state->first_fast[calls_before], 3.0f);
    EXPECT_FLOAT_EQ(state->last_fast[calls_before], 4.0f);
    EXPECT_FLOAT_EQ(state->first_slow[calls_before], 2.0f);
    EXPECT_FLOAT_EQ(state->last_slow[calls_before], 3.0f);
    EXPECT_FLOAT_EQ(state->first_initialized[calls_before], 2.0f);
    EXPECT_FLOAT_EQ(state->last_initialized[calls_before], 3.0f);

    EXPECT_EQ(state->indices[calls_before + 1], 17u);
    EXPECT_EQ(state->block_sizes[calls_before + 1], 2u);
    EXPECT_FLOAT_EQ(state->first_fast[calls_before + 1], 4.0f);
    EXPECT_FLOAT_EQ(state->last_fast[calls_before + 1], 4.0f);
    EXPECT_FLOAT_EQ(state->first_slow[calls_before + 1], 3.0f);
    EXPECT_FLOAT_EQ(state->last_slow[calls_before + 1], 3.0f);
    EXPECT_FLOAT_EQ(state->first_initialized[calls_before + 1], 3.0f);
    EXPECT_FLOAT_EQ(state->last_initialized[calls_before + 1], 4.0f);
}

TEST_F(GraphJitRuntimeFixture, UnequalLatencySampleFeedbackMigratesAlignmentPrehistory)
{
    // Its nodes need identities for their storage to migrate.
    auto feedback_graph = with_node_identities(
        configured_unequal_latency_projected_sample_feedback_graph(*revision));
    ASSERT_TRUE(feedback_graph);

    auto reference = compile_graph(feedback_graph, 129);
    ASSERT_TRUE(reference.succeeded())
        << (reference.diagnostics.empty()
                ? ""
                : reference.diagnostics.front().message);
    auto current = compile_graph(feedback_graph, 130);
    ASSERT_TRUE(current.succeeded())
        << (current.diagnostics.empty()
                ? ""
                : current.diagnostics.front().message);

    auto reference_storage =
        reference.compiled_graph->node_layout.create_storage(resources);
    reference_storage.initialize();
    auto storage = current.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    // After one frame, the detached routed contribution's alignment ring
    // contains one produced frame plus initialized prehistory for the older
    // aligned read. The complete ring must migrate; there is deliberately no
    // separate validity/warmup scalar.
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(0), 1);
    current.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 1);

    auto const reference_regions =
        sample_feedback_alignment_regions(reference_storage);
    auto const current_regions = sample_feedback_alignment_regions(storage);
    ASSERT_EQ(reference_regions.size(), 1u);
    ASSERT_EQ(current_regions.size(), 1u);
    auto const staged_snapshot =
        snapshot_sample_feedback_alignment_regions(storage);

    auto migrated = compile_graph(feedback_graph, 131);
    ASSERT_TRUE(migrated.succeeded())
        << (migrated.diagnostics.empty()
                ? ""
                : migrated.diagnostics.front().message);
    auto migrated_storage =
        migrated.compiled_graph->node_layout.create_storage(resources);
    migrated_storage.initialize(&storage);

    expect_sample_feedback_alignment_regions_equal(
        staged_snapshot, migrated_storage);

    auto* reference_state_before =
        converted_sample_feedback_state(reference_storage);
    ASSERT_NE(reference_state_before, nullptr);
    auto const reference_prefix_calls =
        static_cast<std::size_t>(reference_state_before->calls);

    // Root tick_block() accepts only power-of-two block sizes. Continue the
    // same logical 11-frame suffix using legal calls on both generations.
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(1), 8);
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(9), 2);
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(11), 1);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(1), 8);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(9), 2);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(11), 1);

    auto* reference_state = converted_sample_feedback_state(reference_storage);
    auto* migrated_state = converted_sample_feedback_state(migrated_storage);
    ASSERT_NE(reference_state, nullptr);
    ASSERT_NE(migrated_state, nullptr);
    expect_converted_feedback_suffix_equal(
        *reference_state, reference_prefix_calls, *migrated_state);
}

TEST_F(GraphJitRuntimeFixture, UnequalLatencySampleFeedbackMigratesPopulatedAlignmentRing)
{
    // Its nodes need identities for their storage to migrate.
    auto feedback_graph = with_node_identities(
        configured_unequal_latency_projected_sample_feedback_graph(*revision));
    ASSERT_TRUE(feedback_graph);

    auto reference = compile_graph(feedback_graph, 132);
    ASSERT_TRUE(reference.succeeded())
        << (reference.diagnostics.empty()
                ? ""
                : reference.diagnostics.front().message);
    auto current = compile_graph(feedback_graph, 133);
    ASSERT_TRUE(current.succeeded())
        << (current.diagnostics.empty()
                ? ""
                : current.diagnostics.front().message);

    auto reference_storage =
        reference.compiled_graph->node_layout.create_storage(resources);
    reference_storage.initialize();
    auto storage = current.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();

    // After three frames, the detached routed contribution's alignment ring
    // contains produced samples alongside its still-valid initialized
    // prehistory. Migration must preserve the complete ring rather than
    // reinitializing either part.
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(0), 2);
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(2), 1);
    current.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 2);
    current.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(2), 1);

    auto const current_regions = sample_feedback_alignment_regions(storage);
    ASSERT_EQ(current_regions.size(), 1u);
    auto const staged_snapshot =
        snapshot_sample_feedback_alignment_regions(storage);

    auto migrated = compile_graph(feedback_graph, 134);
    ASSERT_TRUE(migrated.succeeded())
        << (migrated.diagnostics.empty()
                ? ""
                : migrated.diagnostics.front().message);
    auto migrated_storage =
        migrated.compiled_graph->node_layout.create_storage(resources);
    auto migration = migrated_storage.prepare_migration_from(storage);
    migration.commit();

    // Raw compiler-owned state is copied when the prepared migration commits
    // at the activation boundary, before the new generation can observe it.
    expect_sample_feedback_alignment_regions_equal(
        staged_snapshot, migrated_storage);

    auto* reference_state_before =
        converted_sample_feedback_state(reference_storage);
    ASSERT_NE(reference_state_before, nullptr);
    auto const reference_prefix_calls =
        static_cast<std::size_t>(reference_state_before->calls);

    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(3), 8);
    reference.compiled_graph->root_operations.tick_block(
        reference_storage.buffer().data(), &empty_tick_invocation, index(11), 2);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(3), 8);
    migrated.compiled_graph->root_operations.tick_block(
        migrated_storage.buffer().data(), &empty_tick_invocation, index(11), 2);

    auto* reference_state = converted_sample_feedback_state(reference_storage);
    auto* migrated_state = converted_sample_feedback_state(migrated_storage);
    ASSERT_NE(reference_state, nullptr);
    ASSERT_NE(migrated_state, nullptr);
    expect_converted_feedback_suffix_equal(
        *reference_state, reference_prefix_calls, *migrated_state);
}

TEST_F(GraphJitRuntimeFixture, RoutedSampleDetachFeedbackUnaffectedByRecompile)
{
    auto const graph = configured_module_graph(
        *revision, graph_jit_routed_feedback_module_id);
    auto specialized = compile_graph(graph, 359);
    ASSERT_TRUE(specialized.succeeded())
        << (specialized.diagnostics.empty()
                ? ""
                : specialized.diagnostics.front().message);
    ASSERT_EQ(specialized.timings.size(), 1u);
    EXPECT_GT(
        specialized.timings.front().region_specializations_compiled
            + specialized.timings.front().region_specializations_reused,
        0u);
    EXPECT_EQ(
        specialized.timings.front().region_specialization_failures,
        0u)
        << specialized.timings.front().region_specialization_failure;
    expect_converted_feedback_unaffected_by_recompile(
        *jit, definitions, resources, graph, 360);
}

// The edit lengthens the counter's detach() from 5 to 7. Its input reads its 5
// in-flight values, then 2 positions that nothing filled, which read its neutral
// value 100 like the loop's first positions, then the longer loop continues.
TEST_F(GraphJitRuntimeFixture,
       GrowingDetachLatencyReadsNeutralValuesAfterInFlightValues)
{
    std::size_t predecessor_latency = 5;
    std::size_t successor_latency = 7;
    std::array predecessor_arguments{
        iv::details::configuration_argument(predecessor_latency)};
    std::array successor_arguments{
        iv::details::configuration_argument(successor_latency)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_neutral_feedback_counter_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_neutral_feedback_counter_module_id,
        successor_arguments);
    ASSERT_EQ(
        stable_node_identities(*successor), stable_node_identities(*predecessor));

    // The observer reads the counter's output: one more than its input.
    constexpr float neutral = 100.0f;
    std::vector<float> output(
        static_cast<std::size_t>(sequence_cutover + 16 * 64));
    for (std::int64_t position = 0;
         position < static_cast<std::int64_t>(output.size()); ++position) {
        auto const at = [&](std::int64_t earlier) {
            return output[static_cast<std::size_t>(earlier)];
        };
        float input = 0.0f;
        if (position < 5) {
            input = neutral;
        } else if (position < sequence_cutover + 5) {
            input = at(position - 5);
        } else if (position < sequence_cutover + 7) {
            input = neutral;
        } else {
            input = at(position - 7);
        }
        output[static_cast<std::size_t>(position)] = input + 1.0f;
    }
    expect_probe_sequence_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 0, 0,
        [&](std::int64_t position) {
            return output[static_cast<std::size_t>(position)];
        },
        366);
}

// The probe's stereo input keeps 3 values of history. Its left channel feeds
// the probe's output back through detach(); the edit rewires its right channel
// from the ramp to the offset ramp. From the cutover on, the right channel reads
// the offset ramp's values.
TEST_F(GraphJitRuntimeFixture, RewiredChannelBesideDetachedChannelReadsItsNewSource)
{
    bool original = false;
    bool rewired = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(original)};
    std::array successor_arguments{
        iv::details::configuration_argument(rewired)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_routed_rewire_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_routed_rewire_module_id, successor_arguments);
    ASSERT_EQ(
        stable_node_identities(*successor), stable_node_identities(*predecessor));

    constexpr std::int64_t block = 64;
    std::size_t mismatches = 0;
    run_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 370,
        sequence_cutover, sequence_cutover + 4 * block,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            auto const* probe = find_unique_state<RoutedHistoryProbeStateMirror>(
                compiled.node_layout, storage);
            ASSERT_NE(probe, nullptr);
            for (std::int64_t offset = 0; offset < block; ++offset) {
                auto const position = begin + offset;
                auto const value = probe->right[
                    static_cast<std::size_t>(position) % probe->right.size()];
                auto const wanted = position < sequence_cutover
                    ? static_cast<float>(position)
                    : static_cast<float>(position + 100000);
                if (value == wanted) continue;
                if (mismatches++ < 8) {
                    ADD_FAILURE() << "position " << position << " read " << value
                                  << ", expected " << wanted;
                }
            }
        });
    EXPECT_EQ(mismatches, 0u) << "positions that read an unexpected value";
}

// The probe's interleaved stereo input tiles the ramp on its left channel,
// compensated by 7 for the delay node on its right channel. The left channel
// aliases the ramp's storage, so its record is direct. The edit rewires the
// left channel to the offset ramp, whose latency is the same. The left channel
// first reads its 7 in-flight values from the ramp, then the offset ramp's
// values from the cutover on.
TEST_F(GraphJitRuntimeFixture, RewiredTiledChannelReadsItsInFlightValuesFirst)
{
    bool original = false;
    bool rewired = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(original)};
    std::array successor_arguments{
        iv::details::configuration_argument(rewired)};
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources,
        configured_module_graph_with_arguments(
            *revision, graph_jit_tiled_rewire_module_id,
            predecessor_arguments),
        configured_module_graph_with_arguments(
            *revision, graph_jit_tiled_rewire_module_id,
            successor_arguments),
        [](std::int64_t position) {
            if (position < sequence_cutover + 7) return ramp_value(position - 7);
            return static_cast<float>(position - 7 + 100000);
        },
        380);
}

// The probe's interleaved mono input reads the ramp through a layout
// conversion that aliases the ramp's storage, so its record is direct. It is
// compensated by 7 for its slower input. The edit rewires it to the offset
// ramp, whose latency is the same. It first reads its 7 in-flight values from
// the ramp, then the offset ramp's values from the cutover on.
TEST_F(GraphJitRuntimeFixture, RewiredInterleavedMonoInputReadsItsInFlightValuesFirst)
{
    bool original = false;
    bool rewired = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(original)};
    std::array successor_arguments{
        iv::details::configuration_argument(rewired)};
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources,
        configured_module_graph_with_arguments(
            *revision, graph_jit_interleaved_mono_rewire_module_id,
            predecessor_arguments),
        configured_module_graph_with_arguments(
            *revision, graph_jit_interleaved_mono_rewire_module_id,
            successor_arguments),
        [](std::int64_t position) {
            if (position < sequence_cutover + 7) return ramp_value(position - 7);
            return static_cast<float>(position - 7 + 100000);
        },
        382);
}

// The probe's input, whose neutral value is 100, reads a ramp with no latency.
// Before the edit nothing retains the ramp's values: the probe's input reads
// them only while its callback runs, so its record is callback_only. The edit
// gives the ramp latency 3, so the probe now reads it 3 values late. Nothing is
// in flight before the edit, so the probe reads 3 fresh positions, which hold
// its neutral value, then the ramp continues.
TEST_F(GraphJitRuntimeFixture, GrowingSourceLatencyExposesNeutralValues)
{
    std::size_t predecessor_latency = 0;
    std::size_t successor_latency = 3;
    std::array predecessor_arguments{
        iv::details::configuration_argument(predecessor_latency)};
    std::array successor_arguments{
        iv::details::configuration_argument(successor_latency)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_neutral_latency_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_neutral_latency_module_id, successor_arguments);
    ASSERT_EQ(
        stable_node_identities(*successor), stable_node_identities(*predecessor));
    expect_sequential_input_access(
        *jit, definitions, predecessor, 0, 0,
        iv::graph_jit::SampleRealtimePortStateAccess::callback_only, 420);
    if (HasFatalFailure()) return;
    expect_probe_sequence_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 0, 3,
        [](std::int64_t position) {
            if (position < sequence_cutover) return ramp_value(position);
            if (position < sequence_cutover + 3) return 100.0f;
            return ramp_value(position - 3);
        },
        384);
}

// The probe's interleaved stereo input tiles the ramp on its left channel and
// a second ramp on its right, so neither is compensated. Each channel aliases
// its source's storage, so its record is direct. The edit moves the right
// channel to the delay node, which compensates the left channel by 7. A
// history reader keeps the ramp's recent values. The left channel reads 7
// fresh positions, which hold its neutral value 0, then the ramp continues.
TEST_F(GraphJitRuntimeFixture, GrowingCompensationOfTiledChannelExposesNeutralValues)
{
    bool undelayed = false;
    bool delayed = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(undelayed)};
    std::array successor_arguments{
        iv::details::configuration_argument(delayed)};
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources,
        configured_module_graph_with_arguments(
            *revision, graph_jit_tiled_compensation_module_id,
            predecessor_arguments),
        configured_module_graph_with_arguments(
            *revision, graph_jit_tiled_compensation_module_id,
            successor_arguments),
        [](std::int64_t position) {
            if (position < sequence_cutover) return ramp_value(position);
            if (position < sequence_cutover + 7) return 0.0f;
            return ramp_value(position - 7);
        },
        390);
}

// The probe's interleaved mono input downmixes the stereo ramp into transient
// storage through a SampleMaterializationPlan, so its record is materialized.
// It is compensated by 7 for its slower input. The edit rewires it to the
// offset stereo ramp. It first reads its 7 in-flight values downmixed from the
// stereo ramp, then the offset stereo ramp's from the cutover on.
TEST_F(GraphJitRuntimeFixture, RewiredDownmixedInputReadsItsInFlightValuesFirst)
{
    bool original = false;
    bool rewired = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(original)};
    std::array successor_arguments{
        iv::details::configuration_argument(rewired)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_rewire_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_rewire_module_id, successor_arguments);
    expect_sequential_input_access(
        *jit, definitions, predecessor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::materialized, 402);
    expect_sequential_input_access(
        *jit, definitions, successor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::materialized, 403);
    if (HasFatalFailure()) return;
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources, predecessor, successor,
        [](std::int64_t position) {
            // The stereo ramp downmixes to its position + 500, the offset
            // stereo ramp to its position + 100500.
            auto const read = position - 7;
            if (read < 0) return 0.0f;
            if (position < sequence_cutover + 7) {
                return static_cast<float>(read + 500);
            }
            return static_cast<float>(read + 100500);
        },
        400);
}

// The probe's interleaved mono input downmixes a stereo tile of the ramp and a
// second ramp through a SampleCompositionPlan into transient storage, so its
// record is composed. It is compensated by 7 for its slower input. The edit
// rewires the tile's right channel to the offset ramp. The input first reads
// its 7 in-flight values, then the new downmix from the cutover on.
TEST_F(GraphJitRuntimeFixture, RewiredDownmixedTileReadsItsInFlightValuesFirst)
{
    bool original = false;
    bool rewired = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(original)};
    std::array successor_arguments{
        iv::details::configuration_argument(rewired)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_tile_rewire_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_tile_rewire_module_id,
        successor_arguments);
    expect_sequential_input_access(
        *jit, definitions, predecessor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 406);
    expect_sequential_input_access(
        *jit, definitions, successor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 407);
    if (HasFatalFailure()) return;
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources, predecessor, successor,
        [](std::int64_t position) {
            // Two ramps downmix to the ramp; the ramp and the offset ramp to
            // the ramp + 50000.
            if (position < sequence_cutover + 7) return ramp_value(position - 7);
            return static_cast<float>(position - 7 + 50000);
        },
        404);
}

// The probe's interleaved mono input downmixes the stereo ramp, so its record
// is materialized. The edit moves its slower input to the delay node, so its
// compensation grows from 0 to 7. A history reader keeps the stereo ramp's
// recent values. The input reads 7 fresh positions, which hold its neutral
// value 0, then the downmix continues 7 values late.
TEST_F(GraphJitRuntimeFixture, GrowingCompensationOfDownmixedInputExposesNeutralValues)
{
    bool undelayed = false;
    bool delayed = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(undelayed)};
    std::array successor_arguments{
        iv::details::configuration_argument(delayed)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_compensation_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_compensation_module_id,
        successor_arguments);
    expect_sequential_input_access(
        *jit, definitions, successor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::materialized, 410);
    if (HasFatalFailure()) return;
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources, predecessor, successor,
        [](std::int64_t position) {
            // The stereo ramp downmixes to its position + 500.
            if (position < sequence_cutover) {
                return static_cast<float>(position + 500);
            }
            if (position < sequence_cutover + 7) return 0.0f;
            return static_cast<float>(position - 7 + 500);
        },
        408);
}

// The probe's interleaved mono input downmixes a stereo tile of two ramps, so
// its record is composed. The edit moves its slower input to the delay node, so
// its compensation grows from 0 to 7. History readers keep both ramps' recent
// values. The input reads 7 fresh positions, which hold its neutral value 0,
// then the downmix continues 7 values late.
TEST_F(GraphJitRuntimeFixture, GrowingCompensationOfDownmixedTileExposesNeutralValues)
{
    bool undelayed = false;
    bool delayed = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(undelayed)};
    std::array successor_arguments{
        iv::details::configuration_argument(delayed)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_tile_compensation_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_tile_compensation_module_id,
        successor_arguments);
    expect_sequential_input_access(
        *jit, definitions, successor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 414);
    if (HasFatalFailure()) return;
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources, predecessor, successor,
        [](std::int64_t position) {
            if (position < sequence_cutover) return ramp_value(position);
            if (position < sequence_cutover + 7) return 0.0f;
            return ramp_value(position - 7);
        },
        412);
}

// GrowingSourceLatencyExposesNeutralValues with a history reader on the ramp,
// so the probe's input reads the ramp's retained storage before the edit and
// its record is direct.
TEST_F(GraphJitRuntimeFixture, GrowingLatencyOfRetainedSourceExposesNeutralValues)
{
    std::size_t predecessor_latency = 0;
    std::size_t successor_latency = 3;
    std::array predecessor_arguments{
        iv::details::configuration_argument(predecessor_latency)};
    std::array successor_arguments{
        iv::details::configuration_argument(successor_latency)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_retained_neutral_latency_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_retained_neutral_latency_module_id,
        successor_arguments);
    ASSERT_EQ(
        stable_node_identities(*successor), stable_node_identities(*predecessor));
    expect_sequential_input_access(
        *jit, definitions, predecessor, 0, 0,
        iv::graph_jit::SampleRealtimePortStateAccess::direct, 418);
    if (HasFatalFailure()) return;
    expect_probe_sequence_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 0, 3,
        [](std::int64_t position) {
            if (position < sequence_cutover) return ramp_value(position);
            if (position < sequence_cutover + 3) return 100.0f;
            return ramp_value(position - 3);
        },
        416);
}

// The edit changes the Constant from 0.25 to 0.75. The reader is identified in
// both graphs, so its input keeps the values it already holds: after the
// cutover it reads 0.75 at the current position and 0.25 at positions before
// the cutover until they leave its 5-value history.
TEST_F(GraphJitRuntimeFixture, ChangedConstantKeepsItsOldValuesInReaderHistory)
{
    bool predecessor_raised = false;
    bool successor_raised = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(predecessor_raised)};
    std::array successor_arguments{
        iv::details::configuration_argument(successor_raised)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_constant_history_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_constant_history_module_id, successor_arguments);
    ASSERT_EQ(
        stable_node_identities(*successor), stable_node_identities(*predecessor));
    auto const value = [](std::int64_t position) {
        return position < sequence_cutover ? 0.25f : 0.75f;
    };
    run_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 442,
        sequence_cutover, sequence_cutover + 4 * 64,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            auto const* reader = find_unique_state<HistoryConsumerStateMirror>(
                compiled.node_layout, storage);
            ASSERT_NE(reader, nullptr);
            EXPECT_FLOAT_EQ(reader->current, value(begin));
            EXPECT_FLOAT_EQ(reader->history_1, value(begin - 1));
            EXPECT_FLOAT_EQ(reader->history_5, value(begin - 5));
        });
}

// The probe's mono input downmixes a stereo tile of a ramp of latency 3 and the
// offset ramp, so each contributes half of its value. The edit raises the
// latency of its slower input's source from 7 to 10, so the input's read delay
// grows by 3 for both sources: the latent ramp's compensation grows from 4 to 7
// and the offset ramp's from 7 to 10. Each source keeps its own in-flight
// values, exposes 3 fresh positions after them, which contribute half of the
// input's neutral value 0, and continues after those: the latent ramp from
// position 7, the offset ramp from position 10.
TEST_F(GraphJitRuntimeFixture, GrowingCompensationOfComposedSourcesWithUnequalLatencies)
{
    std::size_t predecessor_latency = 7;
    std::size_t successor_latency = 10;
    std::array predecessor_arguments{
        iv::details::configuration_argument(predecessor_latency)};
    std::array successor_arguments{
        iv::details::configuration_argument(successor_latency)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_unequal_latency_composition_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_unequal_latency_composition_module_id,
        successor_arguments);
    expect_sequential_input_access(
        *jit, definitions, predecessor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 426);
    if (HasFatalFailure()) return;
    expect_sequential_input_access(
        *jit, definitions, successor, 0, 10,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 427);
    if (HasFatalFailure()) return;
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources, predecessor, successor,
        [](std::int64_t position) {
            auto const latent = [](std::int64_t source_position) {
                return 0.5f * ramp_value(source_position);
            };
            auto const offset = [](std::int64_t source_position) {
                return source_position < 0
                    ? 0.0f
                    : 0.5f * static_cast<float>(source_position + 100000);
            };
            if (position < sequence_cutover + 4) {
                return latent(position - 7) + offset(position - 7);
            }
            if (position < sequence_cutover + 7) return offset(position - 7);
            if (position < sequence_cutover + 10) return latent(position - 10);
            return latent(position - 10) + offset(position - 10);
        },
        424);
}

// The probe's stereo input reads a ramp on each channel and keeps 3 values of
// history. The edit feeds its left channel from the probe's own output through
// detach(4) instead, which routes the connection; the right channel keeps its
// ramp. The right channel continues its ramp. The left channel reads 4 fresh
// positions, which hold the detach's initial value, the input's neutral value
// 0, then the probe's output 4 values late. The probe outputs its left channel
// plus one, which the observer reads.
TEST_F(GraphJitRuntimeFixture, AddedFeedbackChannelReadsItsDetachValuesBesideItsContinuingChannel)
{
    bool ramps = false;
    bool feedback = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(ramps)};
    std::array successor_arguments{
        iv::details::configuration_argument(feedback)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_added_feedback_channel_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_added_feedback_channel_module_id,
        successor_arguments);
    ASSERT_EQ(
        stable_node_identities(*successor), stable_node_identities(*predecessor));
    // Both channels of the routed connection are composed.
    expect_sequential_input_access(
        *jit, definitions, successor, 3, 4,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 430);
    if (HasFatalFailure()) return;
    expect_sequential_input_access(
        *jit, definitions, successor, 3, 0,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 431);
    if (HasFatalFailure()) return;

    constexpr std::int64_t block = 64;
    std::size_t mismatches = 0;
    auto const expect_value = [&](std::string_view what,
                                  std::int64_t position,
                                  float value,
                                  float wanted) {
        if (value == wanted) return;
        if (mismatches++ < 8) {
            ADD_FAILURE() << what << " at position " << position << " read "
                          << value << ", expected " << wanted;
        }
    };
    run_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 428,
        sequence_cutover, sequence_cutover + 4 * block,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            auto const* probe = find_unique_state<RoutedHistoryProbeStateMirror>(
                compiled.node_layout, storage);
            auto const* observer = find_unique_state<SequenceProbeStateMirror>(
                compiled.node_layout, storage);
            ASSERT_NE(probe, nullptr);
            ASSERT_NE(observer, nullptr);
            ASSERT_EQ(observer->first_index, begin);
            ASSERT_EQ(observer->recorded, static_cast<std::uint64_t>(block));
            for (std::int64_t offset = 0; offset < block; ++offset) {
                auto const position = begin + offset;
                expect_value(
                    "right channel",
                    position,
                    probe->right[
                        static_cast<std::size_t>(position) % probe->right.size()],
                    static_cast<float>(position));
                expect_value(
                    "probe output",
                    position,
                    observer->values[static_cast<std::size_t>(offset)],
                    position < sequence_cutover
                        ? static_cast<float>(position + 1)
                        : static_cast<float>(
                            1 + (position - sequence_cutover) / 4));
            }
        });
    EXPECT_EQ(mismatches, 0u) << "positions that read an unexpected value";
}

// ProjectGraph's manual-control recompile compiles the current generation's
// graph without a predecessor and stages the result through
// GraphExecutor::stage_realtime_code(). Here the current generation was
// compiled as a cutover with a transition root, so its NodeLayout has
// transition-only regions after its steady prefix. Dynamic and constant code for
// one structural realization share its canonical NodeLayout, and a replacement
// changes only the steady Tick operations
// (node_presentation_and_manual_controls_direction.md). It is accepted, and
// takes over once the transition phase has ended.
TEST_F(GraphJitRuntimeFixture, CompatibleCodeReplacesSteadyCodeOfGenerationWithTransitionRoot)
{
    bool original = false;
    bool rewired = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(original)};
    std::array successor_arguments{
        iv::details::configuration_argument(rewired)};
    auto const predecessor_graph = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_rewire_module_id, predecessor_arguments);
    auto const successor_graph = configured_module_graph_with_arguments(
        *revision, graph_jit_downmixed_rewire_module_id, successor_arguments);

    auto predecessor = in_event_source([&] { return jit->handle_project_graph_transaction(
        iv::ProjectGraphGraphJitRequest{
            .successor = {
                .project_generation = 432,
                .graph = predecessor_graph,
                .definitions = definitions,
            },
        }); });
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    iv::GraphExecutor executor(resources);
    ASSERT_EQ(
        executor.stage(predecessor.compiled_graph),
        iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    executor.tick_block(index(0), 64);

    iv::GraphJitCompileRequest const successor_request{
        .project_generation = 433,
        .graph = successor_graph,
        .definitions = definitions,
    };
    auto cutover = in_event_source([&] { return jit->handle_project_graph_transaction(
        iv::ProjectGraphGraphJitRequest{
            .successor = successor_request,
            .predecessor = predecessor.compiled_graph,
        }); });
    ASSERT_TRUE(cutover.succeeded())
        << (cutover.diagnostics.empty()
                ? ""
                : cutover.diagnostics.front().message);
    ASSERT_TRUE(cutover.compiled_graph->cutover);
    ASSERT_NE(
        cutover.compiled_graph->root_operations.transition_tick_block, nullptr);
    ASSERT_GT(
        cutover.compiled_graph->node_layout.storage_size,
        cutover.compiled_graph->node_layout.steady_storage_size);
    ASSERT_EQ(
        executor.stage(cutover.compiled_graph),
        iv::GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(64)));

    auto replacement = in_event_source([&] { return jit->handle_project_graph_transaction(
        iv::ProjectGraphGraphJitRequest{.successor = successor_request}); });
    ASSERT_TRUE(replacement.succeeded())
        << (replacement.diagnostics.empty()
                ? ""
                : replacement.diagnostics.front().message);
    auto staged = iv::GraphExecutorStageResult::ignored_stale;
    EXPECT_NO_THROW(
        staged = executor.stage_realtime_code(replacement.compiled_graph));
    ASSERT_EQ(staged, iv::GraphExecutorStageResult::accepted);

    // The transition root expires after its 7 in-flight values.
    EXPECT_FALSE(executor.activate_pending(index(64)));
    executor.tick_block(index(64), 64);
    EXPECT_TRUE(executor.activate_pending(index(128)));
    executor.tick_block(index(128), 64);
}

// The probe's mono input downmixes a stereo tile of the revising source and the
// ramp, compensated by 7, so each contributes half of its value. The revising
// source's latency is 1: it revises its newest value one position later, to
// its final value, its position + 100. The edit rewires the ramp to the offset
// ramp. Each source keeps its own in-flight values: the revising source owns
// its 6 final ones and resumes at position 6 with the value it finalizes after
// the cutover; the ramp plays its 7 in-flight values first. The input reads
// only final values.
TEST_F(GraphJitRuntimeFixture, RewiredComposedInputReadsItsLatentSourceFinalValues)
{
    bool original = false;
    bool rewired = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(original)};
    std::array successor_arguments{
        iv::details::configuration_argument(rewired)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_revising_tile_rewire_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_revising_tile_rewire_module_id,
        successor_arguments);
    expect_sequential_input_access(
        *jit, definitions, predecessor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 442);
    expect_sequential_input_access(
        *jit, definitions, successor, 0, 7,
        iv::graph_jit::SampleRealtimePortStateAccess::composed, 443);
    if (HasFatalFailure()) return;
    expect_indexed_probe_values_across_graph_edit(
        *jit, definitions, resources, predecessor, successor,
        [](std::int64_t position) {
            auto const read = position - 7;
            if (read < 0) return 0.0f;
            auto const latent = static_cast<float>(read + 100);
            auto const right = position < sequence_cutover + 7
                ? static_cast<float>(read)
                : static_cast<float>(read + 100000);
            return 0.5f * latent + 0.5f * right;
        },
        440);
}

// A loop of the two event feedback nodes: each sends one trigger per 8-sample
// slice, and the second receives the first's triggers through detach(10), 10
// samples after they are sent. The edit lengthens the detach to 12 at index 64.
// The second node owns the triggers its detach has not yet delivered: the one
// sent at 57 still arrives at 67. Two fresh positions follow, and the triggers
// sent from the edit on arrive 12 samples after they are sent.
TEST_F(GraphJitRuntimeFixture, GrowingEventDetachDeliversItsInFlightEventsFirst)
{
    std::size_t predecessor_latency = 10;
    std::size_t successor_latency = 12;
    std::array predecessor_arguments{
        iv::details::configuration_argument(predecessor_latency)};
    std::array successor_arguments{
        iv::details::configuration_argument(successor_latency)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_event_feedback_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_event_feedback_module_id, successor_arguments);
    ASSERT_EQ(
        stable_node_identities(*successor), stable_node_identities(*predecessor));

    constexpr std::int64_t cutover = 64;
    constexpr std::int64_t end = 128;
    EventFeedbackBStateMirror receiver{};
    run_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 438,
        cutover, end,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t) {
            auto const* state = find_unique_state<EventFeedbackBStateMirror>(
                compiled.node_layout, storage);
            ASSERT_NE(state, nullptr);
            receiver = *state;
        });
    if (HasFatalFailure()) return;

    // Both nodes keep their State, so the receiver's slices are its calls.
    ASSERT_EQ(receiver.calls, static_cast<std::uint64_t>(end / 8));
    for (std::size_t slice = 1; slice < end / 8; ++slice) {
        auto const begin = static_cast<std::uint64_t>(slice * 8);
        auto const wanted = begin < static_cast<std::uint64_t>(cutover) + 8
            ? begin + 3
            : begin + 5;
        SCOPED_TRACE("slice at " + std::to_string(begin));
        EXPECT_EQ(receiver.indices[slice], begin);
        EXPECT_EQ(receiver.input_counts[slice], 1u);
        EXPECT_EQ(receiver.first_input_times[slice], wanted);
    }
}

// An edit adds a consumer of a MIDI source whose existing consumer survives.
// The added consumer is an input the predecessor does not have, so its whole
// window is fresh (sequential_port_storage_planning.md, "Port windows are
// semantically node-owned"): its history holds none of the events the source
// sent before the cutover, although the surviving consumer's history, which is
// kept, holds them. The added consumer reads the source's stream through a
// MIDI-to-trigger conversion.
TEST_F(GraphJitRuntimeFixture, AddedEventConsumerReadsNoEventsFromBeforeItsCutover)
{
    bool without = false;
    bool with = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(without)};
    std::array successor_arguments{
        iv::details::configuration_argument(with)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_added_event_consumer_module_id,
        predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_added_event_consumer_module_id,
        successor_arguments);

    // The survivor keeps its history in transition-only storage until its 160
    // positions have passed.
    constexpr std::int64_t cutover = 64;
    constexpr std::int64_t end = 256;
    RetainedMidiConsumerProbeStateMirror survivor{};
    PersistentEventFeedbackConsumerStateMirror added{};
    run_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 444,
        cutover, end,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            auto const* first =
                find_unique_state<RetainedMidiConsumerProbeStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(first, nullptr);
            survivor = *first;
            if (begin < cutover) return;
            auto const* second =
                find_unique_state<PersistentEventFeedbackConsumerStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(second, nullptr);
            added = *second;
        });
    if (HasFatalFailure()) return;

    // Each block the source sends note-ons at 5 and 17 and a note-off at 9.
    // The survivor keeps its State and its history: its window [-96, 128)
    // holds all six, and its window [32, 256) at the last call nine.
    ASSERT_EQ(survivor.calls, 4u);
    EXPECT_EQ(survivor.event_counts[1], 6u);
    EXPECT_EQ(survivor.first_times[1], 5u);
    EXPECT_EQ(survivor.last_times[1], 81u);
    EXPECT_EQ(survivor.event_counts[3], 9u);
    EXPECT_EQ(survivor.first_times[3], 69u);
    EXPECT_EQ(survivor.last_times[3], 209u);
    // The added consumer's window [-256, 128) holds the triggers of the
    // note-ons after the cutover only: 69 and 81; [-128, 256) at its last
    // call holds six.
    ASSERT_EQ(added.calls, 3u);
    EXPECT_EQ(added.event_counts[0], 2u);
    EXPECT_EQ(added.first_times[0], 69u);
    EXPECT_EQ(added.last_times[0], 81u);
    EXPECT_EQ(added.event_counts[2], 6u);
    EXPECT_EQ(added.first_times[2], 69u);
    EXPECT_EQ(added.last_times[2], 209u);
}

// The converted node's left channel mixes the temporal and converted outputs,
// whose latencies differ, through detach(6). The edit adds a node that nothing
// reads before the loop, which renumbers both loop nodes' bundles and changes
// nothing they read.
TEST_F(GraphJitRuntimeFixture, UnequalLatencyFeedbackUnaffectedByAddedLeadingNode)
{
    bool without_leading = false;
    bool with_leading = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(without_leading)};
    std::array successor_arguments{
        iv::details::configuration_argument(with_leading)};
    expect_converted_feedback_unaffected_by_edit(
        *jit, definitions, resources,
        project_unequal_latency_sample_feedback(
            configured_module_graph_with_arguments(
                *revision, graph_jit_detach_feedback_module_id,
                predecessor_arguments)),
        project_unequal_latency_sample_feedback(
            configured_module_graph_with_arguments(
                *revision, graph_jit_detach_feedback_module_id,
                successor_arguments)),
        392);
}

// The unequal-latency routed feedback loop, built without source identities:
// neither of its nodes belongs to a virtual node, so nothing they own survives
// an edit, and neither does their detach, whose producer and consumer ports
// have no identity. Activating a recompile of the loop at index 64 therefore
// behaves exactly like starting the same graph fresh at index 64.
TEST_F(GraphJitRuntimeFixture, AnonymousFeedbackLoopStartsFreshAtAnEdit)
{
    auto graph =
        configured_unequal_latency_projected_sample_feedback_graph(*revision);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(*graph, 64);
    ASSERT_TRUE(analysis.has_value()) << analysis.error();
    ASSERT_TRUE(std::ranges::none_of(
        analysis->nodes,
        [](auto const& node) { return node.stable_identity.has_value(); }));
    constexpr std::int64_t block = 64;
    constexpr std::int64_t cutover = 64;
    constexpr std::int64_t end = 192;

    auto reference = compile_graph(graph, 434);
    ASSERT_TRUE(reference.succeeded())
        << (reference.diagnostics.empty()
                ? ""
                : reference.diagnostics.front().message);
    auto reference_storage =
        reference.compiled_graph->node_layout.create_storage(resources);
    reference_storage.initialize();
    for (auto begin = cutover; begin < end; begin += block) {
        reference.compiled_graph->root_operations.tick_block(
            reference_storage.buffer().data(),
            &empty_tick_invocation,
            index(begin),
            block);
    }
    auto const* expected = converted_sample_feedback_state(reference_storage);
    ASSERT_NE(expected, nullptr);

    ConvertedSampleFeedbackStateMirror actual{};
    run_across_graph_edit(
        *jit, definitions, resources, graph, graph, 436, cutover, end,
        [&](auto const&, auto const& storage, std::int64_t) {
            auto const* state = converted_sample_feedback_state(storage);
            ASSERT_NE(state, nullptr);
            actual = *state;
        });
    if (HasFatalFailure()) return;

    ASSERT_EQ(actual.calls, expected->calls);
    auto const recorded = std::min<std::size_t>(
        static_cast<std::size_t>(actual.calls), actual.indices.size());
    for (std::size_t call = 0; call < recorded; ++call) {
        SCOPED_TRACE("call at " + std::to_string(expected->indices[call]));
        EXPECT_EQ(actual.indices[call], expected->indices[call]);
        EXPECT_EQ(actual.block_sizes[call], expected->block_sizes[call]);
        EXPECT_FLOAT_EQ(actual.first_left[call], expected->first_left[call]);
        EXPECT_FLOAT_EQ(actual.first_right[call], expected->first_right[call]);
        EXPECT_FLOAT_EQ(actual.last_left[call], expected->last_left[call]);
        EXPECT_FLOAT_EQ(actual.last_right[call], expected->last_right[call]);
    }
}

TEST_F(GraphJitRuntimeFixture, FeedForwardEventFanInCompactCarryMigrates)
{
    // Its nodes need identities for their State and storage to migrate.
    auto graph = with_node_identities(
        configured_merged_retained_event_graph(*revision, false));
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->event_producer_groups.size(), 1u);
    ASSERT_TRUE(analysis->event_producer_groups.front().storage_plan);
    EXPECT_EQ(
        analysis->event_producer_groups.front().storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    // Each callback output has its own retained canonical sequence. The
    // fan-in carry is a separate derived sequence for the consumer input.
    auto compiled = compile_graph(graph, 145);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const& state_requirements = compiled.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& state_realizations = compiled.compiled_graph
        ->realtime_port_state_realizations.event_states;
    ASSERT_EQ(state_requirements.size(), 3u);
    ASSERT_EQ(state_realizations.size(), 3u);
    std::size_t output_state_count = 0;
    std::size_t input_state_count = 0;
    for (auto const& realization : state_realizations) {
        ASSERT_LT(realization.requirement_index, state_requirements.size());
        auto const& requirement =
            state_requirements[realization.requirement_index];
        ASSERT_TRUE(realization.retained_storage);
        EXPECT_EQ(
            realization.retained_storage->storage,
            iv::graph_jit::EventRealtimePortStateStorage::compact_carry);
        EXPECT_FALSE(realization.retained_storage->has_source_indices);
        EXPECT_EQ(
            realization.retained_access,
            iv::graph_jit::EventRealtimePortStateAccess::direct);
        EXPECT_FALSE(realization.retained_source_index);
        EXPECT_FALSE(realization.materialization_conversion);
        if (requirement.direction
            == iv::graph_jit::PortDirection::output) {
            ++output_state_count;
        } else {
            ++input_state_count;
        }
    }
    EXPECT_EQ(output_state_count, 2u);
    EXPECT_EQ(input_state_count, 1u);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    RetainedEventConsumerProbeStateMirror* probe = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            probe = static_cast<RetainedEventConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(probe, nullptr);
    // Each source authors -5, 5 and 61, which the consumer reads at 3, 13
    // and 69 (latency 8). Its window [-8, 64) holds 3 and 13 of both.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(probe->event_counts[0], 4u);
    EXPECT_EQ(probe->first_times[0], 3u);
    EXPECT_EQ(probe->second_times[0], 3u);
    EXPECT_EQ(probe->last_times[0], 13u);

    auto recompiled = compile_graph(graph, 146);
    ASSERT_TRUE(recompiled.succeeded())
        << (recompiled.diagnostics.empty()
                ? ""
                : recompiled.diagnostics.front().message);
    auto migrated =
        recompiled.compiled_graph->node_layout.create_storage(resources);
    migrated.initialize(&storage);
    RetainedEventConsumerProbeStateMirror* migrated_probe = nullptr;
    for (std::size_t i = 0;
         i < recompiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (recompiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(RetainedEventConsumerProbeStateMirror)) {
            migrated_probe = static_cast<RetainedEventConsumerProbeStateMirror*>(
                migrated.state_ptr(i));
        }
    }
    ASSERT_NE(migrated_probe, nullptr);
    recompiled.compiled_graph->root_operations.tick_block(
        migrated.buffer().data(), &empty_tick_invocation, index(64), 64);
    // The compiler-owned carry and the probe's State both migrate, so the
    // resumed observation is the probe's second. The carry brings both 69s
    // (authored at 61, not final before the edit); each source's 59, 69 and
    // 125 follow at 67, 77 and 133. The window [56, 128) holds six.
    EXPECT_EQ(migrated_probe->calls, 2u);
    EXPECT_EQ(migrated_probe->event_counts[1], 6u);
    EXPECT_EQ(migrated_probe->first_times[1], 67u);
    EXPECT_EQ(migrated_probe->second_times[1], 67u);
    EXPECT_EQ(migrated_probe->last_times[1], 77u);
}

// The unequal-latency fan-in, recompiled across a cutover with its nodes
// identified. Each source has its own in-flight window in the consumer: the
// latent source's events not yet final at the cutover belong to that output
// (its latency positions), the other source's final but unread events to the
// consumer (its compensation). The consumer reads every event once, as the
// steady graph does: 67, 69, 71, 75 and 77 in the call at 64.
TEST_F(GraphJitRuntimeFixture, RecompiledEventFanInOfUnequalLatencyReadsEachEventOnce)
{
    auto const graph = with_node_identities(
        configured_unequal_latency_event_fan_in_graph(*revision));
    ASSERT_TRUE(graph);

    constexpr std::int64_t cutover = 64;
    constexpr std::int64_t end = 192;
    RetainedEventConsumerProbeStateMirror consumer{};
    run_across_graph_edit(
        *jit, definitions, resources, graph, graph, 456, cutover, end,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t) {
            auto const* state =
                find_unique_state<RetainedEventConsumerProbeStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(state, nullptr);
            consumer = *state;
        });
    if (HasFatalFailure()) return;

    ASSERT_EQ(consumer.calls, 3u);
    EXPECT_EQ(consumer.event_counts[1], 5u);
    EXPECT_EQ(consumer.first_times[1], 67u);
    EXPECT_EQ(consumer.second_times[1], 69u);
    EXPECT_EQ(consumer.last_times[1], 77u);
    // Then 123, 133 and 189 (read at 131, 141, 197) and 131 and 191 (read at
    // 139 and 199): the window [120, 192) holds 131, 133, 135, 139, 141.
    EXPECT_EQ(consumer.event_counts[2], 5u);
    EXPECT_EQ(consumer.first_times[2], 131u);
    EXPECT_EQ(consumer.second_times[2], 133u);
    EXPECT_EQ(consumer.last_times[2], 141u);
}

// A consumer reads a latent event output's own retained stream. The output's
// events that are not final at a recompile's cutover belong to the output
// (its latency positions) and survive it, so the consumer reads them once
// they are final, as the steady graph does. The source sends 62 before the
// cutover, then 63, 69, 126, then 127, 133, 190; with latency 4 the consumer
// reads them at 66, 67, 73, 130, 131, 137.
TEST_F(GraphJitRuntimeFixture, LatentEventOutputReadDirectlyKeepsItsUnfinalEventsAcrossARecompile)
{
    auto const consumer = run_latent_direct_event_edit(
        *jit, definitions, resources, *revision, 4, 4, 458);
    if (HasFatalFailure()) return;
    ASSERT_EQ(consumer.calls, 3u);
    EXPECT_EQ(consumer.event_counts[0], 2u);
    EXPECT_EQ(consumer.first_times[0], 3u);
    EXPECT_EQ(consumer.last_times[0], 9u);
    EXPECT_EQ(consumer.event_counts[1], 3u);
    EXPECT_EQ(consumer.first_times[1], 66u);
    EXPECT_EQ(consumer.second_times[1], 67u);
    EXPECT_EQ(consumer.last_times[1], 73u);
    EXPECT_EQ(consumer.event_counts[2], 3u);
    EXPECT_EQ(consumer.first_times[2], 130u);
    EXPECT_EQ(consumer.second_times[2], 131u);
    EXPECT_EQ(consumer.last_times[2], 137u);
}

// The same edit growing the output's latency from 4 to 6. The consumer reads
// the source's values from its old finalization frontier, 60, at the new
// delay, after 2 fresh positions: the unfinal 62 at 68, then 63 at 69 and
// 69 at 75; then 126, 127, 133 at 132, 133, 139.
TEST_F(GraphJitRuntimeFixture, LatentEventOutputReadDirectlyKeepsItsUnfinalEventsAcrossALatencyChange)
{
    auto const consumer = run_latent_direct_event_edit(
        *jit, definitions, resources, *revision, 4, 6, 460);
    if (HasFatalFailure()) return;
    ASSERT_EQ(consumer.calls, 3u);
    EXPECT_EQ(consumer.event_counts[1], 3u);
    EXPECT_EQ(consumer.first_times[1], 68u);
    EXPECT_EQ(consumer.second_times[1], 69u);
    EXPECT_EQ(consumer.last_times[1], 75u);
    EXPECT_EQ(consumer.event_counts[2], 3u);
    EXPECT_EQ(consumer.first_times[2], 132u);
    EXPECT_EQ(consumer.second_times[2], 133u);
    EXPECT_EQ(consumer.last_times[2], 139u);
}

// A latent sample output whose history grows from 3 to 6 while a reader of
// history 4 reads its ring. The reader's window covers one of the output's
// newly exposed positions with a value it read before, so their claims on the
// ring disagree; the reader's claim is native and the grown output's is not,
// so the output moves to transition-only storage. The output's revisable
// values must still reach the ring: the reader reads every position's final
// value, before and after the cutover.
TEST_F(GraphJitRuntimeFixture, GrowingLatentSampleOutputBesideItsReaderKeepsItsRevisableValues)
{
    std::size_t before = 3;
    std::size_t after = 6;
    std::array predecessor_arguments{iv::details::configuration_argument(before)};
    std::array successor_arguments{iv::details::configuration_argument(after)};
    auto const predecessor = configured_module_graph_with_arguments(
        *revision, graph_jit_latent_window_growth_module_id, predecessor_arguments);
    auto const successor = configured_module_graph_with_arguments(
        *revision, graph_jit_latent_window_growth_module_id, successor_arguments);

    std::size_t checked_blocks = 0;
    run_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 464, 64, 320,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            auto const* consumer =
                find_unique_state<LatentWindowConsumerStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(consumer, nullptr);
            SCOPED_TRACE("block at " + std::to_string(begin));
            EXPECT_GT(consumer->checked, 0u);
            EXPECT_EQ(consumer->mismatches, 0u);
            ++checked_blocks;
        });
    if (HasFatalFailure()) return;
    EXPECT_EQ(checked_blocks, 5u);
}

// An edit adds an acyclic consumer of a staged fan-in, whose stream keeps a
// source index beside each event, and whose surviving reader in the feedback
// loop keeps its history. The added consumer's whole window is fresh: it reads
// none of the events sent before the cutover.
TEST_F(GraphJitRuntimeFixture, AddedConsumerOfIndexedEventFanInReadsNoEventsFromBeforeItsCutover)
{
    auto const predecessor = configured_staged_event_fan_in_graph(*revision);
    auto const successor = configured_staged_event_fan_in_graph(*revision, true);
    ASSERT_TRUE(predecessor);
    ASSERT_TRUE(successor);

    // Long enough for a transition root of the added consumer's 320 positions
    // to expire.
    constexpr std::int64_t cutover = 64;
    constexpr std::int64_t end = 448;
    PersistentEventFeedbackConsumerStateMirror added{};
    run_across_graph_edit(
        *jit, definitions, resources, predecessor, successor, 452,
        cutover, end,
        [&](iv::CompiledGraph const& compiled,
            iv::NodeStorage const& storage,
            std::int64_t begin) {
            if (begin < cutover) return;
            auto const* state =
                find_unique_state<PersistentEventFeedbackConsumerStateMirror>(
                    compiled.node_layout, storage);
            ASSERT_NE(state, nullptr);
            added = *state;
        });
    if (HasFatalFailure()) return;

    // The ingress sends 67 and 127 after the cutover; the loop's second node
    // sends one trigger 2 positions into each 8-position slice, 66 to 122.
    ASSERT_EQ(added.calls, 6u);
    EXPECT_EQ(added.event_counts[0], 10u);
    EXPECT_EQ(added.first_times[0], 66u);
    EXPECT_EQ(added.last_times[0], 127u);
}

TEST_F(GraphJitRuntimeFixture, SampleHistoryCarryAndMigration)
{
    auto history_graph = configured_module_graph(
        *revision, graph_jit_history_fanout_module_id);
    ASSERT_TRUE(history_graph);
    auto history_analysis = build_connection_analysis_plan(
        *history_graph, 64);
    ASSERT_TRUE(history_analysis.has_value())
        << (history_analysis ? std::string{} : history_analysis.error());
    ASSERT_EQ(history_analysis->sample_producer_groups.size(), 1u);
    auto const& history_group = history_analysis->sample_producer_groups[0];
    ASSERT_TRUE(history_group.storage_plan.has_value());
    EXPECT_EQ(
        history_group.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(history_group.storage_requirements.retained_frames, 7u);

    auto history_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *history_analysis, 64);
    ASSERT_TRUE(history_plan.has_value())
        << (history_plan ? std::string{} : history_plan.error());
    ASSERT_EQ(history_plan->carry_operations.size(), 1u);
    ASSERT_EQ(history_plan->persistent_allocations.size(), 1u);
    EXPECT_TRUE(history_plan->materializations.empty());
    EXPECT_EQ(
        history_plan->persistent_allocations[0].kind,
        iv::graph_jit::detail::SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(
        history_plan->persistent_allocations[0].size_bytes,
        7u * sizeof(iv::Sample));

    auto history = compile_graph(history_graph, 116);
    ASSERT_TRUE(history.succeeded())
        << (history.diagnostics.empty() ? "" : history.diagnostics.front().message);
    ASSERT_EQ(history.compiled_graph->node_layout.nodes.size(), 3u);
    ASSERT_EQ(count_raw_regions(history.compiled_graph->node_layout), 2u);
    auto history_persistent_region = std::ranges::find_if(
        history.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw
                && !region.migration_identity.empty();
        });
    ASSERT_NE(
        history_persistent_region,
        history.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(history_persistent_region->size, 7u * sizeof(iv::Sample));

    auto history_storage =
        history.compiled_graph->node_layout.create_storage(resources);
    history_storage.initialize();
    HistoryRampSourceStateMirror* history_source_state = nullptr;
    HistoryConsumerStateMirror* history_mono_state = nullptr;
    StereoHistoryConsumerStateMirror* history_stereo_state = nullptr;
    for (std::size_t i = 0; i < history.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size = history.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(HistoryRampSourceStateMirror)) {
            history_source_state = static_cast<HistoryRampSourceStateMirror*>(
                history_storage.state_ptr(i));
        } else if (state_size == sizeof(HistoryConsumerStateMirror)) {
            history_mono_state = static_cast<HistoryConsumerStateMirror*>(
                history_storage.state_ptr(i));
        } else if (state_size == sizeof(StereoHistoryConsumerStateMirror)) {
            history_stereo_state = static_cast<StereoHistoryConsumerStateMirror*>(
                history_storage.state_ptr(i));
        }
    }
    ASSERT_NE(history_source_state, nullptr);
    ASSERT_NE(history_mono_state, nullptr);
    ASSERT_NE(history_stereo_state, nullptr);

    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 0.0f);
    EXPECT_FLOAT_EQ(history_mono_state->current, 0.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 0.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->current_left, 0.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->current_right, 0.0f);

    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 63.0f);
    EXPECT_EQ(history_mono_state->last_index, 64u);
    EXPECT_FLOAT_EQ(history_mono_state->current, 62.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_1, 61.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 57.0f);
    EXPECT_EQ(history_stereo_state->last_index, 64u);
    EXPECT_FLOAT_EQ(history_stereo_state->current_left, 62.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->current_right, 62.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->history_5_left, 57.0f);
    EXPECT_FLOAT_EQ(history_stereo_state->history_5_right, 57.0f);

    // A block shorter than the retained tail must combine restored older
    // samples with newly produced samples when committing the next carry.
    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), &empty_tick_invocation, index(128), 4);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 127.0f);
    EXPECT_FLOAT_EQ(history_mono_state->current, 126.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 121.0f);
    history.compiled_graph->root_operations.tick_block(
        history_storage.buffer().data(), &empty_tick_invocation, index(132), 4);
    EXPECT_FLOAT_EQ(history_source_state->previous_output, 131.0f);
    EXPECT_FLOAT_EQ(history_mono_state->current, 130.0f);
    EXPECT_FLOAT_EQ(history_mono_state->history_5, 125.0f);

    // Compile a distinct generation and migrate into its canonical NodeStorage.
    // The connection history lives in compiler-owned raw storage, so this
    // proves it survives independently of primitive State migration.
    auto history_next = compile_graph(history_graph, 117);
    ASSERT_TRUE(history_next.succeeded())
        << (history_next.diagnostics.empty()
                ? ""
                : history_next.diagnostics.front().message);
    auto history_next_storage =
        history_next.compiled_graph->node_layout.create_storage(resources);
    history_next_storage.initialize(&history_storage);
    HistoryRampSourceStateMirror* migrated_source = nullptr;
    HistoryConsumerStateMirror* migrated_mono = nullptr;
    for (std::size_t i = 0;
         i < history_next.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size = history_next.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(HistoryRampSourceStateMirror)) {
            migrated_source = static_cast<HistoryRampSourceStateMirror*>(
                history_next_storage.state_ptr(i));
        } else if (state_size == sizeof(HistoryConsumerStateMirror)) {
            migrated_mono = static_cast<HistoryConsumerStateMirror*>(
                history_next_storage.state_ptr(i));
        }
    }
    ASSERT_NE(migrated_source, nullptr);
    ASSERT_NE(migrated_mono, nullptr);
    history_next.compiled_graph->root_operations.tick_block(
        history_next_storage.buffer().data(), &empty_tick_invocation, index(136), 4);
    EXPECT_FLOAT_EQ(migrated_source->previous_output, 135.0f);
    EXPECT_FLOAT_EQ(migrated_mono->current, 134.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_1, 133.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_5, 129.0f);

    // Tick remains Sequential across a playback seam. The callback observes
    // the selected global coordinate, while latency/history address the values
    // immediately preceding this callback in invocation order.
    history_next.compiled_graph->root_operations.tick_block(
        history_next_storage.buffer().data(), &empty_tick_invocation, index(1000), 4);
    EXPECT_EQ(migrated_mono->last_index, 1000u);
    EXPECT_FLOAT_EQ(migrated_source->previous_output, 139.0f);
    EXPECT_FLOAT_EQ(migrated_mono->current, 138.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_1, 137.0f);
    EXPECT_FLOAT_EQ(migrated_mono->history_5, 133.0f);

}

TEST_F(
    GraphJitRuntimeFixture,
    TransitionRootPreservesRewiredHistoryAndMirrorsNewProducerSamples)
{
    auto predecessor_graph = configured_module_graph(
        *revision, graph_jit_large_history_rewire_module_id);
    ASSERT_TRUE(predecessor_graph);
    auto successor_graph = rewired_large_history_graph(predecessor_graph);
    auto predecessor = compile_graph(predecessor_graph, 240);
    ASSERT_TRUE(predecessor.succeeded())
        << (predecessor.diagnostics.empty()
                ? ""
                : predecessor.diagnostics.front().message);
    EXPECT_EQ(
        std::ranges::count_if(
            predecessor.compiled_graph->realtime_port_state_requirements.states,
            [](auto const& requirement) {
                return requirement.role
                        == iv::graph_jit::RealtimePortStateRole::
                            sequential_input_history
                    && requirement.stable_identity.has_value();
            }),
        2);

    iv::GraphJitCompileRequest successor_request{
        .project_generation = 241,
        .graph = successor_graph,
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
    ASSERT_TRUE(specialized.compiled_graph->cutover);
    ASSERT_NE(
        specialized.compiled_graph->root_operations.transition_tick_block,
        nullptr);
    ASSERT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.size(),
        1u);
    EXPECT_EQ(
        specialized.compiled_graph->cutover->state_transition
            .sample_expiry_cursors.front().value_count,
        5000u);
    EXPECT_EQ(
        specialized.compiled_graph->node_layout.steady_storage_size,
        steady_successor.compiled_graph->node_layout.storage_size);
    EXPECT_GT(
        specialized.compiled_graph->node_layout.storage_size,
        specialized.compiled_graph->node_layout.steady_storage_size);

    auto predecessor_storage = predecessor.compiled_graph
        ->node_layout.create_storage(resources);
    predecessor_storage.initialize();
    for (std::uint64_t sample = 0; sample <= 5056; sample += 64) {
        predecessor.compiled_graph->root_operations.tick_block(
            predecessor_storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(sample),
            64);
    }

    auto successor_storage = specialized.compiled_graph
        ->node_layout.create_storage(resources);
    successor_storage.initialize(&predecessor_storage);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        specialized.compiled_graph->cutover->state_transition,
        predecessor_storage.buffer(),
        successor_storage.buffer());
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared,
        predecessor_storage.buffer(),
        successor_storage.buffer(),
        index(5120));

    std::vector<LargeHistoryConsumerStateMirror*> consumers;
    for (std::size_t node = 0;
         node < specialized.compiled_graph->node_layout.nodes.size(); ++node) {
        if (specialized.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(LargeHistoryConsumerStateMirror)) {
            consumers.push_back(
                static_cast<LargeHistoryConsumerStateMirror*>(
                    successor_storage.state_ptr(node)));
        }
    }
    ASSERT_EQ(consumers.size(), 2u);

    specialized.compiled_graph->root_operations.transition_tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(5120),
        64);
    for (auto const* consumer : consumers) {
        EXPECT_FLOAT_EQ(consumer->current, 5120.0f);
        EXPECT_FLOAT_EQ(consumer->history_1, 5119.0f);
        EXPECT_FLOAT_EQ(consumer->history_5000, 120.0f);
    }

    for (std::uint64_t sample = 5184; sample < 10112; sample += 64) {
        specialized.compiled_graph->root_operations.transition_tick_block(
            successor_storage.buffer().data(),
            &empty_tick_invocation,
            index(sample),
            64);
    }
    specialized.compiled_graph->root_operations.transition_tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(10112),
        8);
    specialized.compiled_graph->root_operations.tick_block(
        successor_storage.buffer().data(),
        &empty_tick_invocation,
        index(10120),
        64);
    for (auto const* consumer : consumers) {
        EXPECT_FLOAT_EQ(consumer->current, 10120.0f);
        EXPECT_FLOAT_EQ(consumer->history_1, 10119.0f);
        EXPECT_FLOAT_EQ(consumer->history_5000, 5120.0f);
    }
}

} // namespace iv::test::graph_jit_tests
