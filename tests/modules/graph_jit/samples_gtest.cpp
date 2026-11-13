#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_direct_sample_module_id[] = "iv.test.graph_jit.state_context.direct_sample_module";

constexpr char graph_jit_disconnected_sample_input_module_id[] = "iv.test.graph_jit.state_context.disconnected_sample_input_module";

constexpr char graph_jit_transient_sample_module_id[] = "iv.test.graph_jit.state_context.transient_sample_module";

constexpr char graph_jit_tick_fallback_sample_module_id[] = "iv.test.graph_jit.state_context.tick_fallback_sample_module";

constexpr char graph_jit_neutral_compensated_module_id[] =
    "iv.test.graph_jit.state_context.neutral_compensated_module";

constexpr char graph_jit_connected_input_module_id[] =
    "iv.test.graph_jit.state_context.connected_input_module";

constexpr char graph_jit_shared_neutral_compensated_module_id[] =
    "iv.test.graph_jit.state_context.shared_neutral_compensated_module";

constexpr char graph_jit_scc_compensation_module_id[] =
    "iv.test.graph_jit.state_context.scc_compensation_module";

struct StereoSampleConsumerProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    float first_left = 0.0f;
    float first_right = 0.0f;
    float last_left = 0.0f;
    float last_right = 0.0f;
    float sum_left = 0.0f;
    float sum_right = 0.0f;
};

struct DisconnectedSampleInputProbeStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t last_index = 0;
    std::uint64_t last_block_size = 0;
    float current_left = 0.0f;
    float current_right = 0.0f;
    float history_left = 0.0f;
    float history_right = 0.0f;
    float block_first = 0.0f;
    float block_last = 0.0f;
    float block_sum = 0.0f;
    std::uint32_t marker = 0;
};

struct AlignedPairPassthroughStateMirror {
    std::uint64_t calls = 0;
    std::uint64_t checked = 0;
    std::uint64_t mismatches = 0;
    float largest_difference = 0.0f;
};

SampleConsumerProbeStateMirror* find_consumer_state(
    iv::CompiledGraph const& graph,
    iv::NodeStorage& storage)
{
    for (std::size_t i = 0; i < graph.node_layout.nodes.size(); ++i) {
        if (graph.node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            return static_cast<SampleConsumerProbeStateMirror*>(storage.state_ptr(i));
        }
    }
    return nullptr;
}

// Like expect_probe_sequence_across_graph_edit, without checking the probe's
// read latency.
template<class Expected>
void expect_probe_values_across_graph_edit(
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
            auto const* probe = find_unique_state<SequenceProbeStateMirror>(
                compiled.node_layout, storage);
            ASSERT_NE(probe, nullptr);
            EXPECT_EQ(probe->first_index, begin);
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

// Runs scc_compensation_module for two blocks of 64 and returns the
// passthrough's and the probe's States.
std::pair<AlignedPairPassthroughStateMirror, LatencyCompensationProbeStateMirror>
run_scc_compensation(
    iv::ResourceContext const& resources,
    iv::GraphJitCompileResult const& compiled)
{
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    for (std::int64_t begin = 0; begin < 128; begin += 64) {
        compiled.compiled_graph->root_operations.tick_block(
            storage.buffer().data(), &empty_tick_invocation, index(begin), 64);
    }
    auto const* passthrough = find_unique_state<AlignedPairPassthroughStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    auto const* probe = find_unique_state<LatencyCompensationProbeStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    return {
        passthrough ? *passthrough : AlignedPairPassthroughStateMirror{},
        probe ? *probe : LatencyCompensationProbeStateMirror{},
    };
}

} // namespace

TEST_F(GraphJitRuntimeFixture, DisconnectedSampleInputUsesDeclaredDefault)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_disconnected_sample_input_module_id);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    EXPECT_TRUE(analysis->sample_connections.empty());
    EXPECT_TRUE(analysis->sample_producer_groups.empty());

    auto compiled = compile_graph(graph, 121);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 1u);

    auto const& requirements =
        compiled.compiled_graph->realtime_port_state_requirements.states;
    auto const& realizations =
        compiled.compiled_graph->realtime_port_state_realizations.sample_states;
    ASSERT_EQ(requirements.size(), 2u);
    ASSERT_EQ(realizations.size(), 2u);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        auto const found = std::ranges::find_if(
            realizations,
            [&](iv::graph_jit::SampleRealtimePortStateRealization const& view) {
                return view.requirement_index < requirements.size()
                    && requirements[view.requirement_index].channel == channel;
            });
        ASSERT_NE(found, realizations.end());
        EXPECT_EQ(
            found->retained_access,
            iv::graph_jit::SampleRealtimePortStateAccess::immutable_constant);
        EXPECT_EQ(
            found->authored_storage.storage,
            iv::graph_jit::SampleRealtimePortStateStorage::immutable_constant);
        EXPECT_EQ(found->authored_storage.representation_channel, channel);
        EXPECT_EQ(
            found->authored_storage.channel_layout.channel_type,
            iv::ChannelTypeId::stereo);
        EXPECT_EQ(
            found->authored_storage.channel_layout.sample_layout,
            iv::SampleStreamLayout::interleaved);
        EXPECT_EQ(found->authored_storage.timeline_offset_frames, 0);
        ASSERT_TRUE(found->authored_storage.constant_value);
        EXPECT_FLOAT_EQ(
            static_cast<float>(sample_value_from_bytes(
                *found->authored_storage.constant_value)),
            0.375f);
        EXPECT_FALSE(found->authored_storage.node_storage_offset);
        EXPECT_FALSE(found->authored_storage.callback_arena_offset);
        EXPECT_TRUE(found->retained_sources.empty());
        EXPECT_FALSE(found->materialization);
    }

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<DisconnectedSampleInputProbeStateMirror*>(
        storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(100), 64);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_EQ(state->last_index, 100u);
    EXPECT_EQ(state->last_block_size, 64u);
    EXPECT_FLOAT_EQ(state->current_left, 0.375f);
    EXPECT_FLOAT_EQ(state->current_right, 0.375f);
    EXPECT_FLOAT_EQ(state->history_left, 0.375f);
    EXPECT_FLOAT_EQ(state->history_right, 0.375f);
    EXPECT_FLOAT_EQ(state->block_first, 0.375f);
    EXPECT_FLOAT_EQ(state->block_last, 0.375f);
    EXPECT_FLOAT_EQ(state->block_sum, 24.0f);
    EXPECT_EQ(state->marker, 0xd15c0a11u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(164), 16);
    EXPECT_EQ(state->calls, 2u);
    EXPECT_EQ(state->last_index, 164u);
    EXPECT_EQ(state->last_block_size, 16u);
    EXPECT_FLOAT_EQ(state->history_left, 0.375f);
    EXPECT_FLOAT_EQ(state->history_right, 0.375f);
    EXPECT_FLOAT_EQ(state->block_sum, 6.0f);
}

TEST_F(GraphJitRuntimeFixture, DisconnectedSampleOutputKeepsDeclaredHistory)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_disconnected_sample_output_module_id);
    ASSERT_TRUE(graph);
    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    EXPECT_TRUE(analysis->sample_connections.empty());
    EXPECT_TRUE(analysis->sample_producer_groups.empty());

    auto compiled = compile_graph(graph, 122);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 2u);
    auto const raw = std::ranges::find_if(
        compiled.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(raw, compiled.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(raw->size, 5u * sizeof(iv::Sample));

    auto const& requirements =
        compiled.compiled_graph->realtime_port_state_requirements.states;
    auto const& realizations =
        compiled.compiled_graph->realtime_port_state_realizations.sample_states;
    // The output owns one window: its last history 3 + latency 2 values, each
    // filed at the index it was authored for.
    ASSERT_EQ(requirements.size(), 1u);
    ASSERT_EQ(realizations.size(), 1u);
    for (auto const& view : realizations) {
        ASSERT_LT(view.requirement_index, requirements.size());
        auto const& requirement = requirements[view.requirement_index];
        EXPECT_EQ(requirement.direction, iv::graph_jit::PortDirection::output);
        EXPECT_EQ(
            requirement.role,
            iv::graph_jit::RealtimePortStateRole::tick_output_history);
        EXPECT_EQ(requirement.extent_samples, 5u);
        EXPECT_EQ(
            view.authored_storage.storage,
            iv::graph_jit::SampleRealtimePortStateStorage::callback_transient);
        EXPECT_EQ(
            view.retained_access,
            iv::graph_jit::SampleRealtimePortStateAccess::direct);
        ASSERT_EQ(view.retained_sources.size(), 1u);
        auto const& retained = view.retained_sources.front();
        EXPECT_EQ(
            retained.storage,
            iv::graph_jit::SampleRealtimePortStateStorage::compact_carry);
        EXPECT_EQ(view.authored_storage.representation_channel, 0u);
        EXPECT_EQ(view.authored_storage.timeline_offset_frames, 0);
        ASSERT_TRUE(view.authored_storage.callback_arena_offset);
        EXPECT_FALSE(view.authored_storage.node_storage_offset);
        EXPECT_EQ(retained.representation_channel, 0u);
        EXPECT_EQ(retained.timeline_offset_frames, 0);
        EXPECT_EQ(retained.storage_frame_count, 5u);
        EXPECT_EQ(retained.carry_future_frames, 0u);
        EXPECT_EQ(retained.storage_size_bytes, 5u * sizeof(iv::Sample));
        ASSERT_TRUE(retained.node_storage_offset);
        EXPECT_EQ(*retained.node_storage_offset, raw->storage_offset);
        EXPECT_FALSE(retained.callback_arena_offset);
        EXPECT_FALSE(retained.constant_value);
        EXPECT_FALSE(view.materialization);
    }

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto* state = static_cast<HistoryRampSourceStateMirror*>(storage.state_ptr(0));
    ASSERT_NE(state, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(state->calls, 1u);
    EXPECT_FLOAT_EQ(state->previous_output, 0.0f);
    EXPECT_EQ(state->marker, 0x91a2b3c4u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(state->calls, 2u);
    EXPECT_FLOAT_EQ(state->previous_output, 63.0f);
}

TEST_F(GraphJitRuntimeFixture, DirectSampleStorage)
{
    auto direct_graph = configured_module_graph(
        *revision, graph_jit_direct_sample_module_id);
    ASSERT_TRUE(direct_graph);
    auto direct_analysis = build_connection_analysis_plan(
        *direct_graph, 64);
    ASSERT_TRUE(direct_analysis.has_value())
        << (direct_analysis ? std::string{} : direct_analysis.error());
    ASSERT_EQ(direct_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(direct_analysis->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_EQ(
        direct_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    auto direct_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *direct_analysis, 64);
    ASSERT_TRUE(direct_plan.has_value())
        << (direct_plan ? std::string{} : direct_plan.error());
    ASSERT_EQ(direct_plan->producer_groups.size(), 1u);
    ASSERT_TRUE(direct_plan->producer_groups[0].has_value());
    ASSERT_EQ(direct_plan->connection_representations.size(), 1u);
    ASSERT_TRUE(direct_plan->connection_representations[0].has_value());
    EXPECT_EQ(
        *direct_plan->connection_representations[0],
        direct_plan->producer_groups[0]->canonical_representation);

    auto direct = compile_graph(direct_graph, 109);
    ASSERT_TRUE(direct.succeeded())
        << (direct.diagnostics.empty() ? "" : direct.diagnostics.front().message);
    ASSERT_EQ(direct.compiled_graph->node_layout.nodes.size(), 2u);
    ASSERT_EQ(count_raw_regions(direct.compiled_graph->node_layout), 1u);

    auto direct_storage = direct.compiled_graph->node_layout.create_storage(resources);
    direct_storage.initialize();
    auto* direct_state = find_consumer_state(*direct.compiled_graph, direct_storage);
    ASSERT_NE(direct_state, nullptr);
    direct.compiled_graph->root_operations.tick_block(
        direct_storage.buffer().data(), &empty_tick_invocation, index(100), 64);
    EXPECT_EQ(direct_state->calls, 1u);
    EXPECT_EQ(direct_state->last_index, 100u);
    EXPECT_EQ(direct_state->last_block_size, 64u);
    EXPECT_FLOAT_EQ(direct_state->first, 100.0f);
    EXPECT_FLOAT_EQ(direct_state->last, 163.0f);
    EXPECT_FLOAT_EQ(direct_state->sum, 8416.0f);
    direct.compiled_graph->root_operations.tick_block(
        direct_storage.buffer().data(), &empty_tick_invocation, index(200), 16);
    EXPECT_EQ(direct_state->calls, 2u);
    EXPECT_EQ(direct_state->last_index, 200u);
    EXPECT_EQ(direct_state->last_block_size, 16u);
    EXPECT_FLOAT_EQ(direct_state->first, 200.0f);
    EXPECT_FLOAT_EQ(direct_state->last, 215.0f);
    EXPECT_FLOAT_EQ(direct_state->sum, 3320.0f);

}

TEST_F(GraphJitRuntimeFixture, TransientSampleStorage)
{
    auto transient_graph = configured_module_graph(
        *revision, graph_jit_transient_sample_module_id);
    ASSERT_TRUE(transient_graph);
    auto transient_analysis = build_connection_analysis_plan(
        *transient_graph, 64);
    ASSERT_TRUE(transient_analysis.has_value())
        << (transient_analysis ? std::string{} : transient_analysis.error());
    ASSERT_EQ(transient_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(
        transient_analysis->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_EQ(
        transient_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    auto transient_plan =
        iv::graph_jit::detail::build_sample_storage_plan(
            *transient_analysis, 64);
    ASSERT_TRUE(transient_plan.has_value())
        << (transient_plan
                ? std::string{}
                : transient_plan.error());
    ASSERT_EQ(transient_plan->producer_groups.size(), 1u);
    ASSERT_TRUE(transient_plan->producer_groups[0].has_value());
    ASSERT_EQ(transient_plan->connection_representations.size(), 1u);
    ASSERT_TRUE(transient_plan->connection_representations[0].has_value());
    EXPECT_EQ(
        *transient_plan->connection_representations[0],
        transient_plan->producer_groups[0]->canonical_representation);

    auto transient = compile_graph(transient_graph, 110);
    ASSERT_TRUE(transient.succeeded())
        << (transient.diagnostics.empty()
                ? ""
                : transient.diagnostics.front().message);
    ASSERT_EQ(transient.compiled_graph->node_layout.nodes.size(), 2u);
    ASSERT_EQ(count_raw_regions(transient.compiled_graph->node_layout), 1u);
    auto transient_storage =
        transient.compiled_graph->node_layout.create_storage(resources);
    transient_storage.initialize();
    auto* transient_state = find_consumer_state(
        *transient.compiled_graph, transient_storage);
    ASSERT_NE(transient_state, nullptr);

    // The limited producer executes as four 16-frame slices. Each imported
    // invocation reconstructs an ephemeral façade at its absolute sample index,
    // while all slices target the same planned current-block storage.
    // The 64-frame consumer therefore observes one contiguous logical block.
    transient.compiled_graph->root_operations.tick_block(
        transient_storage.buffer().data(), &empty_tick_invocation, index(300), 64);
    EXPECT_EQ(transient_state->calls, 1u);
    EXPECT_EQ(transient_state->last_index, 300u);
    EXPECT_EQ(transient_state->last_block_size, 64u);
    EXPECT_FLOAT_EQ(transient_state->first, 300.0f);
    EXPECT_FLOAT_EQ(transient_state->last, 363.0f);
    EXPECT_FLOAT_EQ(transient_state->sum, 21216.0f);
    transient.compiled_graph->root_operations.tick_block(
        transient_storage.buffer().data(), &empty_tick_invocation, index(500), 32);
    EXPECT_EQ(transient_state->calls, 2u);
    EXPECT_EQ(transient_state->last_index, 500u);
    EXPECT_EQ(transient_state->last_block_size, 32u);
    EXPECT_FLOAT_EQ(transient_state->first, 500.0f);
    EXPECT_FLOAT_EQ(transient_state->last, 531.0f);
    EXPECT_FLOAT_EQ(transient_state->sum, 16496.0f);

}

TEST_F(GraphJitRuntimeFixture, TransientArenaReuse)
{
    auto reused_arena_graph = configured_module_graph(
        *revision, graph_jit_reused_sample_arena_module_id);
    ASSERT_TRUE(reused_arena_graph);
    auto reused_arena_analysis =
        build_connection_analysis_plan(
            *reused_arena_graph, 64);
    ASSERT_TRUE(reused_arena_analysis.has_value())
        << (reused_arena_analysis
                ? std::string{}
                : reused_arena_analysis.error());
    auto reused_arena_storage =
        iv::graph_jit::detail::build_sample_storage_plan(
            *reused_arena_analysis, 64);
    ASSERT_TRUE(reused_arena_storage.has_value())
        << (reused_arena_storage
                ? std::string{}
                : reused_arena_storage.error());
    ASSERT_EQ(reused_arena_storage->representations.size(), 2u);
    ASSERT_EQ(reused_arena_storage->transient_allocations.size(), 2u);
    auto const first_arena_allocation =
        reused_arena_storage->representations[0].transient_allocation;
    auto const second_arena_allocation =
        reused_arena_storage->representations[1].transient_allocation;
    ASSERT_LT(
        first_arena_allocation,
        reused_arena_storage->transient_allocations.size());
    ASSERT_LT(
        second_arena_allocation,
        reused_arena_storage->transient_allocations.size());
    EXPECT_EQ(
        reused_arena_storage->transient_allocations[first_arena_allocation]
            .region_relative_offset,
        reused_arena_storage->transient_allocations[second_arena_allocation]
            .region_relative_offset);
    EXPECT_EQ(
        reused_arena_storage->transient_arena_size,
        64u * sizeof(iv::Sample));

    auto reused_arena = compile_graph(reused_arena_graph, 111);
    ASSERT_TRUE(reused_arena.succeeded())
        << (reused_arena.diagnostics.empty()
                ? ""
                : reused_arena.diagnostics.front().message);
    ASSERT_EQ(reused_arena.compiled_graph->node_layout.nodes.size(), 4u);
    ASSERT_EQ(count_raw_regions(reused_arena.compiled_graph->node_layout), 1u);

    auto reused_storage =
        reused_arena.compiled_graph->node_layout.create_storage(resources);
    reused_storage.initialize();
    std::vector<SampleConsumerProbeStateMirror*> reused_consumer_states;
    for (std::size_t i = 0;
         i < reused_arena.compiled_graph->node_layout.nodes.size(); ++i) {
        if (reused_arena.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            reused_consumer_states.push_back(
                static_cast<SampleConsumerProbeStateMirror*>(
                    reused_storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(reused_consumer_states.size(), 2u);
    reused_arena.compiled_graph->root_operations.tick_block(
        reused_storage.buffer().data(), &empty_tick_invocation, index(700), 64);
    for (auto const* state : reused_consumer_states) {
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->calls, 1u);
        EXPECT_EQ(state->last_index, 700u);
        EXPECT_EQ(state->last_block_size, 64u);
        EXPECT_FLOAT_EQ(state->first, 700.0f);
        EXPECT_FLOAT_EQ(state->last, 763.0f);
        EXPECT_FLOAT_EQ(state->sum, 46816.0f);
    }

}

TEST_F(GraphJitRuntimeFixture, StackBudgetUsesPackedSampleLifetimes)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_reused_sample_arena_module_id);
    ASSERT_TRUE(graph);

    iv::GraphJit budgeted_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .realtime_storage_cost_model = iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 2u * 64u * sizeof(iv::Sample),
        },
    });
    auto compiled = budgeted_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 211,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    // The two producer buffers are each 64 samples, but their lifetimes do not
    // overlap, so they pack into one 64-sample range. The block consumers share
    // a second 64-sample contiguous-input scratch range. Both ranges fit the
    // budget, so neither producer is forced into NodeStorage.
    EXPECT_EQ(count_raw_regions(compiled.compiled_graph->node_layout), 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(700), 64);

    std::size_t observed = 0;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            != sizeof(SampleConsumerProbeStateMirror)) {
            continue;
        }
        auto const* state = static_cast<SampleConsumerProbeStateMirror const*>(
            storage.state_ptr(i));
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->calls, 1u);
        EXPECT_FLOAT_EQ(state->first, 700.0f);
        EXPECT_FLOAT_EQ(state->last, 763.0f);
        ++observed;
    }
    EXPECT_EQ(observed, 2u);
}

TEST_F(GraphJitRuntimeFixture, StackBudgetMovesOverlappingSampleBuffersToNodeStorage)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_composed_sample_revision_module_id);
    ASSERT_TRUE(graph);

    // Each one-sample-latency producer uses a 128-frame stack ring at the
    // default policy. The two producer lifetimes overlap until the stereo
    // consumer executes, so the packed stack is larger than one ring. A
    // 512-byte limit therefore requires at least one full NodeStorage buffer.
    iv::GraphJit budgeted_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .realtime_storage_cost_model = iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 128u * sizeof(iv::Sample),
        },
    });
    auto compiled = budgeted_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 212,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);

    std::size_t full_sample_buffers = 0;
    for (auto const& region : compiled.compiled_graph->node_layout.regions) {
        if (region.kind != iv::NodeLayout::Region::Kind::raw) continue;
        if (region.migration_identity.starts_with("graphjit.sample:")
            && region.size >= 128u * sizeof(iv::Sample)) {
            ++full_sample_buffers;
        }
    }
    EXPECT_GE(full_sample_buffers, 1u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    StereoSampleConsumerProbeStateMirror* consumer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(StereoSampleConsumerProbeStateMirror)) {
            consumer = static_cast<StereoSampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(consumer, nullptr);
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_FLOAT_EQ(consumer->first_left, 0.0f);
    EXPECT_FLOAT_EQ(consumer->first_right, 0.0f);
    EXPECT_FLOAT_EQ(consumer->last_left, 102.0f);
    EXPECT_FLOAT_EQ(consumer->last_right, 102.0f);
}

TEST_F(GraphJitRuntimeFixture, StackBudgetRejectsMandatorySampleConversionBuffer)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_stereo_conversion_module_id);
    ASSERT_TRUE(graph);

    iv::GraphJit budgeted_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .realtime_storage_cost_model = iv::RealtimeStorageCostModel{
            .stack_budget_bytes = 32u * sizeof(iv::Sample),
        },
    });
    auto compiled = budgeted_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 213,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_FALSE(compiled.succeeded());
    ASSERT_FALSE(compiled.diagnostics.empty());
    EXPECT_NE(
        compiled.diagnostics.front().message.find(
            "packed realtime stack requires"),
        std::string::npos);
}

TEST_F(GraphJitRuntimeFixture, SampleFanoutConversion)
{
    auto fanout_graph = configured_module_graph(
        *revision, graph_jit_sample_fanout_conversion_module_id);
    ASSERT_TRUE(fanout_graph);
    auto fanout_analysis = build_connection_analysis_plan(
        *fanout_graph, 64);
    ASSERT_TRUE(fanout_analysis.has_value())
        << (fanout_analysis ? std::string{} : fanout_analysis.error());
    ASSERT_EQ(fanout_analysis->sample_producer_groups.size(), 1u);
    ASSERT_EQ(fanout_analysis->sample_producer_groups[0].connection_indices.size(), 4u);
    ASSERT_TRUE(fanout_analysis->sample_producer_groups[0].storage_plan.has_value());
    EXPECT_EQ(
        fanout_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);

    auto fanout_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *fanout_analysis, 64);
    ASSERT_TRUE(fanout_plan.has_value())
        << (fanout_plan ? std::string{} : fanout_plan.error());
    ASSERT_EQ(fanout_plan->producer_groups.size(), 1u);
    ASSERT_TRUE(fanout_plan->producer_groups[0].has_value());
    ASSERT_EQ(fanout_plan->representations.size(), 1u);
    EXPECT_TRUE(fanout_plan->materializations.empty());
    ASSERT_EQ(fanout_plan->connection_representations.size(), 4u);
    ASSERT_EQ(fanout_plan->connection_channel_bindings.size(), 4u);
    auto const fanout_canonical =
        fanout_plan->producer_groups[0]->canonical_representation;
    EXPECT_EQ(
        fanout_plan->producer_groups[0]
            ->storage_plan.candidate_costs.full_node_storage.copied_bytes,
        0u);
    EXPECT_EQ(
        fanout_plan->producer_groups[0]
            ->storage_plan.candidate_costs.full_node_storage
            .ring_addressed_bytes,
        0u);
    std::size_t direct_whole_port = 0;
    std::size_t channel_granular = 0;
    for (std::size_t connection_index = 0;
         connection_index < fanout_plan->connection_representations.size();
         ++connection_index) {
        if (fanout_plan->connection_representations[connection_index]) {
            EXPECT_EQ(
                *fanout_plan->connection_representations[connection_index],
                fanout_canonical);
            ++direct_whole_port;
            continue;
        }
        ASSERT_TRUE(
            fanout_plan->connection_channel_bindings[connection_index]
                .has_value());
        auto const& channels =
            *fanout_plan->connection_channel_bindings[connection_index];
        ASSERT_FALSE(channels.empty());
        for (auto const& channel : channels) {
            EXPECT_EQ(channel.representation, fanout_canonical);
            EXPECT_EQ(channel.representation_channel, 0u);
            EXPECT_EQ(channel.frame_delay, 0u);
        }
        ++channel_granular;
    }
    EXPECT_EQ(direct_whole_port, 1u);
    EXPECT_EQ(channel_granular, 3u);
    EXPECT_EQ(
        fanout_plan->transient_arena_size,
        64u * sizeof(iv::Sample));

    auto fanout = compile_graph(fanout_graph, 112);
    ASSERT_TRUE(fanout.succeeded())
        << (fanout.diagnostics.empty() ? "" : fanout.diagnostics.front().message);
    ASSERT_EQ(fanout.compiled_graph->node_layout.nodes.size(), 5u);
    ASSERT_EQ(count_raw_regions(fanout.compiled_graph->node_layout), 1u);

    auto fanout_storage =
        fanout.compiled_graph->node_layout.create_storage(resources);
    fanout_storage.initialize();
    std::vector<SampleConsumerProbeStateMirror*> fanout_mono_states;
    std::vector<StereoSampleConsumerProbeStateMirror*> fanout_stereo_states;
    for (std::size_t i = 0; i < fanout.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size = fanout.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleConsumerProbeStateMirror)) {
            fanout_mono_states.push_back(
                static_cast<SampleConsumerProbeStateMirror*>(
                    fanout_storage.state_ptr(i)));
        } else if (state_size == sizeof(StereoSampleConsumerProbeStateMirror)) {
            fanout_stereo_states.push_back(
                static_cast<StereoSampleConsumerProbeStateMirror*>(
                    fanout_storage.state_ptr(i)));
        }
    }
    ASSERT_EQ(fanout_mono_states.size(), 2u);
    ASSERT_EQ(fanout_stereo_states.size(), 2u);

    // 37..100 crosses the 64-frame storage ring boundary. Converted consumers
    // now read the canonical source channel directly, so absolute-index alias
    // addressing must remain correct across the wrap.
    fanout.compiled_graph->root_operations.tick_block(
        fanout_storage.buffer().data(), &empty_tick_invocation, index(37), 64);
    for (auto const* state : fanout_mono_states) {
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->calls, 1u);
        EXPECT_FLOAT_EQ(state->first, 37.0f);
        EXPECT_FLOAT_EQ(state->last, 100.0f);
        EXPECT_FLOAT_EQ(state->sum, 4384.0f);
    }
    for (auto const* state : fanout_stereo_states) {
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->calls, 1u);
        EXPECT_EQ(state->last_index, 37u);
        EXPECT_EQ(state->last_block_size, 64u);
        EXPECT_FLOAT_EQ(state->first_left, 37.0f);
        EXPECT_FLOAT_EQ(state->first_right, 37.0f);
        EXPECT_FLOAT_EQ(state->last_left, 100.0f);
        EXPECT_FLOAT_EQ(state->last_right, 100.0f);
        EXPECT_FLOAT_EQ(state->sum_left, 4384.0f);
        EXPECT_FLOAT_EQ(state->sum_right, 4384.0f);
    }

    fanout.compiled_graph->root_operations.tick_block(
        fanout_storage.buffer().data(), &empty_tick_invocation, index(205), 16);
    for (auto const* state : fanout_mono_states) {
        EXPECT_EQ(state->calls, 2u);
        EXPECT_FLOAT_EQ(state->first, 205.0f);
        EXPECT_FLOAT_EQ(state->last, 220.0f);
        EXPECT_FLOAT_EQ(state->sum, 3400.0f);
    }
    for (auto const* state : fanout_stereo_states) {
        EXPECT_EQ(state->calls, 2u);
        EXPECT_EQ(state->last_index, 205u);
        EXPECT_EQ(state->last_block_size, 16u);
        EXPECT_FLOAT_EQ(state->first_left, 205.0f);
        EXPECT_FLOAT_EQ(state->first_right, 205.0f);
        EXPECT_FLOAT_EQ(state->last_left, 220.0f);
        EXPECT_FLOAT_EQ(state->last_right, 220.0f);
        EXPECT_FLOAT_EQ(state->sum_left, 3400.0f);
        EXPECT_FLOAT_EQ(state->sum_right, 3400.0f);
    }

}

TEST_F(GraphJitRuntimeFixture, StereoSampleConversion)
{
    auto stereo_conversion_graph = configured_module_graph(
        *revision, graph_jit_stereo_conversion_module_id);
    ASSERT_TRUE(stereo_conversion_graph);
    auto stereo_conversion_analysis =
        build_connection_analysis_plan(
            *stereo_conversion_graph, 64);
    ASSERT_TRUE(stereo_conversion_analysis.has_value())
        << (stereo_conversion_analysis
                ? std::string{}
                : stereo_conversion_analysis.error());
    ASSERT_EQ(stereo_conversion_analysis->sample_producer_groups.size(), 1u);
    ASSERT_EQ(
        stereo_conversion_analysis->sample_producer_groups[0]
            .connection_indices.size(),
        2u);
    ASSERT_TRUE(
        stereo_conversion_analysis->sample_producer_groups[0]
            .storage_plan.has_value());
    EXPECT_EQ(
        stereo_conversion_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::transient_stack);
    auto stereo_conversion_plan =
        iv::graph_jit::detail::build_sample_storage_plan(
            *stereo_conversion_analysis, 64);
    ASSERT_TRUE(stereo_conversion_plan.has_value())
        << (stereo_conversion_plan
                ? std::string{}
                : stereo_conversion_plan.error());
    EXPECT_EQ(stereo_conversion_plan->representations.size(), 2u);
    EXPECT_EQ(stereo_conversion_plan->materializations.size(), 1u);
    ASSERT_TRUE(stereo_conversion_plan->producer_groups[0]);
    auto const& stereo_source_costs =
        stereo_conversion_plan->producer_groups[0]
            ->storage_plan.candidate_costs;
    EXPECT_EQ(
        stereo_source_costs.transient_stack.copied_bytes,
        64u * sizeof(iv::Sample));
    EXPECT_EQ(
        stereo_source_costs.full_node_storage.copied_bytes,
        64u * sizeof(iv::Sample));
    EXPECT_EQ(
        stereo_source_costs.full_node_storage.ring_addressed_bytes,
        128u * sizeof(iv::Sample));
    ASSERT_EQ(
        std::ranges::count_if(
            stereo_conversion_plan->connection_channel_bindings,
            [](auto const& binding) { return binding.has_value(); }),
        1);

    auto stereo_conversion = compile_graph(stereo_conversion_graph, 113);
    ASSERT_TRUE(stereo_conversion.succeeded())
        << (stereo_conversion.diagnostics.empty()
                ? ""
                : stereo_conversion.diagnostics.front().message);
    ASSERT_EQ(stereo_conversion.compiled_graph->node_layout.nodes.size(), 3u);
    ASSERT_EQ(
        count_raw_regions(stereo_conversion.compiled_graph->node_layout), 1u);

    auto stereo_conversion_storage =
        stereo_conversion.compiled_graph->node_layout.create_storage(resources);
    stereo_conversion_storage.initialize();
    SampleConsumerProbeStateMirror* stereo_to_mono_state = nullptr;
    StereoSampleConsumerProbeStateMirror* stereo_to_planar_state = nullptr;
    for (std::size_t i = 0;
         i < stereo_conversion.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            stereo_conversion.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleConsumerProbeStateMirror)) {
            stereo_to_mono_state = static_cast<SampleConsumerProbeStateMirror*>(
                stereo_conversion_storage.state_ptr(i));
        } else if (state_size == sizeof(StereoSampleConsumerProbeStateMirror)) {
            stereo_to_planar_state =
                static_cast<StereoSampleConsumerProbeStateMirror*>(
                    stereo_conversion_storage.state_ptr(i));
        }
    }
    ASSERT_NE(stereo_to_mono_state, nullptr);
    ASSERT_NE(stereo_to_planar_state, nullptr);

    stereo_conversion.compiled_graph->root_operations.tick_block(
        stereo_conversion_storage.buffer().data(), &empty_tick_invocation, index(51), 32);
    EXPECT_EQ(stereo_to_mono_state->calls, 1u);
    EXPECT_EQ(stereo_to_mono_state->last_index, 51u);
    EXPECT_EQ(stereo_to_mono_state->last_block_size, 32u);
    EXPECT_FLOAT_EQ(stereo_to_mono_state->first, 551.0f);
    EXPECT_FLOAT_EQ(stereo_to_mono_state->last, 582.0f);
    EXPECT_FLOAT_EQ(stereo_to_mono_state->sum, 18128.0f);

    EXPECT_EQ(stereo_to_planar_state->calls, 1u);
    EXPECT_EQ(stereo_to_planar_state->last_index, 51u);
    EXPECT_EQ(stereo_to_planar_state->last_block_size, 32u);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->first_left, 51.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->first_right, 1051.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->last_left, 82.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->last_right, 1082.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->sum_left, 2128.0f);
    EXPECT_FLOAT_EQ(stereo_to_planar_state->sum_right, 34128.0f);

}

TEST_F(GraphJitRuntimeFixture, SampleOutputUpdateRevisesUnpublishedFrames)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_sample_revision_module_id);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    auto const& producer = analysis->sample_producer_groups.front();
    EXPECT_EQ(producer.storage_requirements.retained_frames, 1u);
    ASSERT_TRUE(producer.storage_plan.has_value());
    EXPECT_EQ(
        producer.storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 1u);
    EXPECT_EQ(
        sample_plan->persistent_allocations.front().kind,
        iv::graph_jit::detail::SamplePersistentStorageKind::compact_carry);
    EXPECT_TRUE(sample_plan->materializations.empty());

    auto compiled = compile_graph(graph, 114);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 3u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    SampleConsumerProbeStateMirror* mono = nullptr;
    StereoSampleConsumerProbeStateMirror* stereo = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const state_size =
            compiled.compiled_graph->node_layout.nodes[i].state_size;
        if (state_size == sizeof(SampleConsumerProbeStateMirror)) {
            mono = static_cast<SampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        } else if (state_size == sizeof(StereoSampleConsumerProbeStateMirror)) {
            stereo = static_cast<StereoSampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(mono, nullptr);
    ASSERT_NE(stereo, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(mono->calls, 1u);
    EXPECT_FLOAT_EQ(mono->first, 0.0f);
    EXPECT_FLOAT_EQ(mono->last, 102.0f);
    EXPECT_FLOAT_EQ(mono->sum, 303.0f);
    EXPECT_EQ(stereo->calls, 1u);
    EXPECT_FLOAT_EQ(stereo->first_left, 0.0f);
    EXPECT_FLOAT_EQ(stereo->first_right, 0.0f);
    EXPECT_FLOAT_EQ(stereo->last_left, 102.0f);
    EXPECT_FLOAT_EQ(stereo->last_right, 102.0f);
    EXPECT_FLOAT_EQ(stereo->sum_left, 303.0f);
    EXPECT_FLOAT_EQ(stereo->sum_right, 303.0f);

    // Frame 3 was published in the previous root call but remained hidden by
    // the source's one-sample authored latency. The first update in this call
    // must revise that retained frame before either direct or converted fanout
    // observes it.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    EXPECT_EQ(mono->calls, 2u);
    EXPECT_FLOAT_EQ(mono->first, 103.0f);
    EXPECT_FLOAT_EQ(mono->last, 106.0f);
    EXPECT_FLOAT_EQ(mono->sum, 418.0f);
    EXPECT_EQ(stereo->calls, 2u);
    EXPECT_FLOAT_EQ(stereo->first_left, 103.0f);
    EXPECT_FLOAT_EQ(stereo->first_right, 103.0f);
    EXPECT_FLOAT_EQ(stereo->last_left, 106.0f);
    EXPECT_FLOAT_EQ(stereo->last_right, 106.0f);
    EXPECT_FLOAT_EQ(stereo->sum_left, 418.0f);
    EXPECT_FLOAT_EQ(stereo->sum_right, 418.0f);
}

TEST_F(GraphJitRuntimeFixture, SampleOutputUpdateSurvivesPersistentRingStorage)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_persistent_sample_revision_module_id);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_producer_groups.size(), 1u);
    auto const& producer = analysis->sample_producer_groups.front();
    EXPECT_EQ(producer.storage_requirements.retained_frames, 5002u);
    ASSERT_TRUE(producer.storage_plan.has_value());
    EXPECT_EQ(
        producer.storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    ASSERT_EQ(sample_plan->persistent_allocations.size(), 1u);
    EXPECT_EQ(
        sample_plan->persistent_allocations.front().kind,
        iv::graph_jit::detail::SamplePersistentStorageKind::ring);

    auto compiled = compile_graph(graph, 153);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    SampleConsumerProbeStateMirror* consumer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            consumer = static_cast<SampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(consumer, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_FLOAT_EQ(consumer->first, 0.0f);
    EXPECT_FLOAT_EQ(consumer->last, 101.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 201.0f);

    // The first update of this invocation revises frame 3 from the preceding
    // root call while the canonical producer storage is a persistent
    // ring rather than compact carry.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    EXPECT_EQ(consumer->calls, 2u);
    EXPECT_FLOAT_EQ(consumer->first, 102.0f);
    EXPECT_FLOAT_EQ(consumer->last, 105.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 414.0f);
}

TEST_F(GraphJitRuntimeFixture, SampleOutputUpdateFeedsComposedFanout)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_composed_sample_revision_module_id);
    ASSERT_TRUE(graph);

    auto analysis = build_connection_analysis_plan(
        *graph, 64);
    ASSERT_TRUE(analysis.has_value())
        << (analysis ? std::string{} : analysis.error());
    ASSERT_EQ(analysis->sample_producer_groups.size(), 2u);
    for (auto const& producer : analysis->sample_producer_groups) {
        EXPECT_EQ(producer.storage_requirements.retained_frames, 1u);
        ASSERT_TRUE(producer.storage_plan.has_value());
        EXPECT_EQ(
            producer.storage_plan->kind,
            iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    }

    auto sample_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *analysis, 64);
    ASSERT_TRUE(sample_plan.has_value())
        << (sample_plan ? std::string{} : sample_plan.error());
    EXPECT_TRUE(sample_plan->compositions.empty());
    EXPECT_EQ(
        std::ranges::count_if(
            sample_plan->connection_channel_bindings,
            [](auto const& binding) { return binding.has_value(); }),
        1u);
    EXPECT_TRUE(sample_plan->feedback_timelines.empty());

    auto compiled = compile_graph(graph, 154);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 3u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    StereoSampleConsumerProbeStateMirror* consumer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(StereoSampleConsumerProbeStateMirror)) {
            consumer = static_cast<StereoSampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(consumer, nullptr);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_FLOAT_EQ(consumer->first_left, 0.0f);
    EXPECT_FLOAT_EQ(consumer->first_right, 0.0f);
    EXPECT_FLOAT_EQ(consumer->last_left, 102.0f);
    EXPECT_FLOAT_EQ(consumer->last_right, 102.0f);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    EXPECT_EQ(consumer->calls, 2u);
    EXPECT_FLOAT_EQ(consumer->first_left, 103.0f);
    EXPECT_FLOAT_EQ(consumer->first_right, 103.0f);
    EXPECT_FLOAT_EQ(consumer->last_left, 106.0f);
    EXPECT_FLOAT_EQ(consumer->last_right, 106.0f);
}

TEST_F(GraphJitRuntimeFixture, TickOnlySampleNodePreservesContextAcrossPrimitiveSlices)
{
    auto graph = configured_module_graph(
        *revision, graph_jit_tick_fallback_sample_module_id);
    ASSERT_TRUE(graph);
    auto compiled = compile_graph(graph, 130);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 2u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    SampleConsumerProbeStateMirror* consumer = nullptr;
    for (std::size_t i = 0;
         i < compiled.compiled_graph->node_layout.nodes.size(); ++i) {
        if (compiled.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(SampleConsumerProbeStateMirror)) {
            consumer = static_cast<SampleConsumerProbeStateMirror*>(
                storage.state_ptr(i));
        }
    }
    ASSERT_NE(consumer, nullptr);

    // The source callback is sliced as [0,2), [2,4). Because it only
    // implements tick(), each slice is further executed one sample at a time.
    // update() at index 2 therefore revises a frame authored by the
    // preceding primitive callback invocation, while the one-sample source
    // latency keeps those frames unpublished until the consumer runs.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    EXPECT_EQ(consumer->calls, 1u);
    EXPECT_EQ(consumer->last_index, 0u);
    EXPECT_EQ(consumer->last_block_size, 4u);
    EXPECT_FLOAT_EQ(consumer->first, 0.0f);
    EXPECT_FLOAT_EQ(consumer->last, 102.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 303.0f);

    // The next root call begins by revising frame 3 from the previous root.
    // Later updates cross the [4,6) -> [6,8) primitive-slice boundary too.
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    EXPECT_EQ(consumer->calls, 2u);
    EXPECT_EQ(consumer->last_index, 4u);
    EXPECT_EQ(consumer->last_block_size, 4u);
    EXPECT_FLOAT_EQ(consumer->first, 103.0f);
    EXPECT_FLOAT_EQ(consumer->last, 106.0f);
    EXPECT_FLOAT_EQ(consumer->sum, 418.0f);
}

TEST_F(GraphJitRuntimeFixture, SampleLatencyCompensation)
{
    auto latency_compensation_graph = configured_module_graph(
        *revision, graph_jit_latency_compensation_module_id);
    ASSERT_TRUE(latency_compensation_graph);
    auto latency_compensation = compile_graph(latency_compensation_graph, 114);
    ASSERT_TRUE(latency_compensation.succeeded())
        << (latency_compensation.diagnostics.empty()
                ? ""
                : latency_compensation.diagnostics.front().message);
    ASSERT_EQ(latency_compensation.compiled_graph->node_layout.nodes.size(), 3u);

    auto latency_storage =
        latency_compensation.compiled_graph->node_layout.create_storage(resources);
    latency_storage.initialize();
    LatencyCompensationProbeStateMirror* latency_probe = nullptr;
    for (std::size_t i = 0;
         i < latency_compensation.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const& node = latency_compensation.compiled_graph->node_layout.nodes[i];
        if (node.state_size == sizeof(LatencyCompensationProbeStateMirror)) {
            ASSERT_EQ(latency_probe, nullptr);
            latency_probe = static_cast<LatencyCompensationProbeStateMirror*>(
                latency_storage.state_ptr(i));
        }
    }
    ASSERT_NE(latency_probe, nullptr);

    latency_compensation.compiled_graph->root_operations.tick_block(
        latency_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(latency_probe->calls, 1u);
    EXPECT_EQ(latency_probe->last_index, 0u);
    EXPECT_EQ(latency_probe->last_block_size, 64u);
    EXPECT_EQ(latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(latency_probe->fast_first, 0.0f);
    EXPECT_FLOAT_EQ(latency_probe->slow_first, 0.0f);
    EXPECT_FLOAT_EQ(latency_probe->fast_last, 56.0f);
    EXPECT_FLOAT_EQ(latency_probe->slow_last, 56.0f);
    EXPECT_FLOAT_EQ(latency_probe->max_abs_difference, 0.0f);
    EXPECT_EQ(latency_probe->marker, 0x71ac0deu);

    // The second kernel call requires the fast branch to restore the source's
    // compiler-owned seven-frame carry while the slow branch restores its own
    // two-frame output carry. Both must still address the same logical sample.
    latency_compensation.compiled_graph->root_operations.tick_block(
        latency_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(latency_probe->calls, 2u);
    EXPECT_EQ(latency_probe->last_index, 64u);
    EXPECT_EQ(latency_probe->last_block_size, 64u);
    EXPECT_EQ(latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(latency_probe->fast_first, 57.0f);
    EXPECT_FLOAT_EQ(latency_probe->slow_first, 57.0f);
    EXPECT_FLOAT_EQ(latency_probe->fast_last, 120.0f);
    EXPECT_FLOAT_EQ(latency_probe->slow_last, 120.0f);
    EXPECT_FLOAT_EQ(latency_probe->max_abs_difference, 0.0f);

}

TEST_F(GraphJitRuntimeFixture, ConvertedFanoutLatencyWindows)
{
    auto latency_conversion_fanout_graph = configured_module_graph(
        *revision, graph_jit_latency_conversion_fanout_module_id);
    ASSERT_TRUE(latency_conversion_fanout_graph);
    auto latency_conversion_analysis =
        build_connection_analysis_plan(
            *latency_conversion_fanout_graph, 64);
    ASSERT_TRUE(latency_conversion_analysis.has_value())
        << (latency_conversion_analysis
                ? std::string{}
                : latency_conversion_analysis.error());

    auto latency_source_group = std::ranges::find_if(
        latency_conversion_analysis->sample_producer_groups,
        [](auto const& group) { return group.connection_indices.size() == 3; });
    ASSERT_NE(
        latency_source_group,
        latency_conversion_analysis->sample_producer_groups.end());
    EXPECT_EQ(latency_source_group->storage_requirements.retained_frames, 7u);
    ASSERT_TRUE(latency_source_group->storage_plan.has_value());
    EXPECT_EQ(
        latency_source_group->storage_plan->kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);

    std::size_t converted_read_0 = 0;
    std::size_t converted_read_7 = 0;
    for (auto const connection_index : latency_source_group->connection_indices) {
        auto const& connection =
            latency_conversion_analysis->sample_connections[connection_index];
        if (!connection.requires_conversion) continue;
        if (connection.read_latency == 0) ++converted_read_0;
        if (connection.read_latency == 7) ++converted_read_7;
    }
    EXPECT_EQ(converted_read_0, 1u);
    EXPECT_EQ(converted_read_7, 1u);

    auto latency_conversion_plan =
        iv::graph_jit::detail::build_sample_storage_plan(
            *latency_conversion_analysis, 64);
    ASSERT_TRUE(latency_conversion_plan.has_value())
        << (latency_conversion_plan
                ? std::string{}
                : latency_conversion_plan.error());
    auto const latency_source_group_index = static_cast<std::size_t>(
        latency_source_group
        - latency_conversion_analysis->sample_producer_groups.begin());
    ASSERT_LT(
        latency_source_group_index,
        latency_conversion_plan->producer_groups.size());
    ASSERT_TRUE(
        latency_conversion_plan->producer_groups[latency_source_group_index]
            .has_value());
    auto const latency_source_representation =
        latency_conversion_plan->producer_groups[latency_source_group_index]
            ->canonical_representation;
    EXPECT_TRUE(latency_conversion_plan->materializations.empty());
    std::size_t aliased_converted_connections = 0;
    for (auto const connection_index : latency_source_group->connection_indices) {
        auto const& connection =
            latency_conversion_analysis->sample_connections[connection_index];
        if (!connection.requires_conversion) continue;
        ASSERT_LT(
            connection_index,
            latency_conversion_plan->connection_channel_bindings.size());
        auto const& bindings =
            latency_conversion_plan->connection_channel_bindings[connection_index];
        ASSERT_TRUE(bindings.has_value());
        ASSERT_EQ(bindings->size(), 1u);
        EXPECT_EQ(bindings->front().representation, latency_source_representation);
        EXPECT_EQ(bindings->front().representation_channel, 0u);
        EXPECT_EQ(bindings->front().frame_delay, 0u);
        ++aliased_converted_connections;
    }
    EXPECT_EQ(aliased_converted_connections, 2u);

    auto latency_conversion_fanout =
        compile_graph(latency_conversion_fanout_graph, 115);
    ASSERT_TRUE(latency_conversion_fanout.succeeded())
        << (latency_conversion_fanout.diagnostics.empty()
                ? ""
                : latency_conversion_fanout.diagnostics.front().message);
    ASSERT_EQ(
        latency_conversion_fanout.compiled_graph->node_layout.nodes.size(), 4u);

    auto latency_conversion_storage =
        latency_conversion_fanout.compiled_graph->node_layout.create_storage(
            resources);
    latency_conversion_storage.initialize();
    LatencyCompensationProbeStateMirror* interleaved_latency_probe = nullptr;
    SampleConsumerProbeStateMirror* current_observer = nullptr;
    for (std::size_t i = 0;
         i < latency_conversion_fanout.compiled_graph->node_layout.nodes.size();
         ++i) {
        auto const& node =
            latency_conversion_fanout.compiled_graph->node_layout.nodes[i];
        if (node.state_size == sizeof(LatencyCompensationProbeStateMirror)) {
            ASSERT_EQ(interleaved_latency_probe, nullptr);
            interleaved_latency_probe =
                static_cast<LatencyCompensationProbeStateMirror*>(
                    latency_conversion_storage.state_ptr(i));
        } else if (node.state_size == sizeof(SampleConsumerProbeStateMirror)) {
            ASSERT_EQ(current_observer, nullptr);
            current_observer = static_cast<SampleConsumerProbeStateMirror*>(
                latency_conversion_storage.state_ptr(i));
        }
    }
    ASSERT_NE(interleaved_latency_probe, nullptr);
    ASSERT_NE(current_observer, nullptr);

    latency_conversion_fanout.compiled_graph->root_operations.tick_block(
        latency_conversion_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(interleaved_latency_probe->calls, 1u);
    EXPECT_EQ(interleaved_latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_first, 0.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_first, 0.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_last, 56.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_last, 56.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->max_abs_difference, 0.0f);
    EXPECT_EQ(interleaved_latency_probe->marker, 0x1a7e2e0u);

    // This observer shares the same converted interleaved storage as the
    // compensated fast branch but reads it at zero latency. The materializer
    // must therefore cover both [-7, +56] and [0, +63], not just the older
    // compensated window.
    EXPECT_EQ(current_observer->calls, 1u);
    EXPECT_EQ(current_observer->last_index, 0u);
    EXPECT_EQ(current_observer->last_block_size, 64u);
    EXPECT_FLOAT_EQ(current_observer->first, 0.0f);
    EXPECT_FLOAT_EQ(current_observer->last, 63.0f);
    EXPECT_FLOAT_EQ(current_observer->sum, 2016.0f);

    latency_conversion_fanout.compiled_graph->root_operations.tick_block(
        latency_conversion_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(interleaved_latency_probe->calls, 2u);
    EXPECT_EQ(interleaved_latency_probe->mismatches, 0u);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_first, 57.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_first, 57.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->fast_last, 120.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->slow_last, 120.0f);
    EXPECT_FLOAT_EQ(interleaved_latency_probe->max_abs_difference, 0.0f);
    EXPECT_EQ(current_observer->calls, 2u);
    EXPECT_EQ(current_observer->last_index, 64u);
    EXPECT_FLOAT_EQ(current_observer->first, 64.0f);
    EXPECT_FLOAT_EQ(current_observer->last, 127.0f);
    EXPECT_FLOAT_EQ(current_observer->sum, 6112.0f);

}

TEST_F(GraphJitRuntimeFixture, ComposedSampleLatency)
{
    auto composed_latency_graph = configured_module_graph(
        *revision, graph_jit_composed_latency_module_id);
    ASSERT_TRUE(composed_latency_graph);
    auto composed_latency_analysis =
        build_connection_analysis_plan(
            *composed_latency_graph, 64);
    ASSERT_TRUE(composed_latency_analysis.has_value())
        << (composed_latency_analysis
                ? std::string{}
                : composed_latency_analysis.error());
    auto composed_connection = std::ranges::find_if(
        composed_latency_analysis->sample_connections,
        [](auto const& connection) {
            return !connection.canonical_source_port
                && connection.source_channel_timings.size() == 2;
        });
    ASSERT_NE(
        composed_connection,
        composed_latency_analysis->sample_connections.end());
    ASSERT_EQ(composed_connection->source_channel_timings.size(), 2u);
    ASSERT_EQ(composed_connection->projection_contributions.size(), 1u);
    EXPECT_EQ(
        composed_connection->projection_contributions[0].source_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        composed_connection->projection_contributions[0].target_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        composed_connection->projection_contributions[0].source_channel_indices,
        (std::vector<std::size_t>{0u, 1u}));
    EXPECT_EQ(
        composed_connection->projection_contributions[0].target_channels,
        (std::vector<std::size_t>{0u, 1u}));
    EXPECT_EQ(composed_connection->source_channel_timings[0].read_latency, 7u);
    EXPECT_EQ(composed_connection->source_channel_timings[1].read_latency, 2u);

    auto composed_latency = compile_graph(composed_latency_graph, 120);
    ASSERT_TRUE(composed_latency.succeeded())
        << (composed_latency.diagnostics.empty()
                ? ""
                : composed_latency.diagnostics.front().message);
    ASSERT_EQ(composed_latency.compiled_graph->node_layout.nodes.size(), 3u);

    auto composed_latency_storage =
        composed_latency.compiled_graph->node_layout.create_storage(resources);
    composed_latency_storage.initialize();
    StereoSampleConsumerProbeStateMirror* composed_probe = nullptr;
    for (std::size_t i = 0;
         i < composed_latency.compiled_graph->node_layout.nodes.size(); ++i) {
        auto const& node = composed_latency.compiled_graph->node_layout.nodes[i];
        if (node.state_size == sizeof(StereoSampleConsumerProbeStateMirror)) {
            ASSERT_EQ(composed_probe, nullptr);
            composed_probe = static_cast<StereoSampleConsumerProbeStateMirror*>(
                composed_latency_storage.state_ptr(i));
        }
    }
    ASSERT_NE(composed_probe, nullptr);

    composed_latency.compiled_graph->root_operations.tick_block(
        composed_latency_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(composed_probe->calls, 1u);
    EXPECT_EQ(composed_probe->last_index, 0u);
    EXPECT_EQ(composed_probe->last_block_size, 64u);
    EXPECT_FLOAT_EQ(composed_probe->first_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_probe->first_right, 0.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_left, 56.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_right, 56.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_left, 1596.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_right, 1596.0f);

    // The second call proves the channel-granular input reads each producer's
    // independently restored history (7 frames from the direct source, 2 from
    // the delayed source) without a gathered stereo storage.
    composed_latency.compiled_graph->root_operations.tick_block(
        composed_latency_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(composed_probe->calls, 2u);
    EXPECT_EQ(composed_probe->last_index, 64u);
    EXPECT_EQ(composed_probe->last_block_size, 64u);
    EXPECT_FLOAT_EQ(composed_probe->first_left, 57.0f);
    EXPECT_FLOAT_EQ(composed_probe->first_right, 57.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_left, 120.0f);
    EXPECT_FLOAT_EQ(composed_probe->last_right, 120.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_left, 5664.0f);
    EXPECT_FLOAT_EQ(composed_probe->sum_right, 5664.0f);

    // A composed input with history must extend each aliased producer's
    // retention by the target history. The direct channel therefore needs
    // 5 + 7 frames while the delayed channel needs 5 + 2; the logical stereo
    // input remains timestamp-aligned at port latency 0.
}

TEST_F(GraphJitRuntimeFixture, ComposedSampleHistory)
{
    auto composed_history_graph = configured_module_graph(
        *revision, graph_jit_composed_history_module_id);
    ASSERT_TRUE(composed_history_graph);
    auto composed_history_analysis =
        build_connection_analysis_plan(
            *composed_history_graph, 64);
    ASSERT_TRUE(composed_history_analysis.has_value())
        << (composed_history_analysis
                ? std::string{}
                : composed_history_analysis.error());

    auto composed_history_connection = std::ranges::find_if(
        composed_history_analysis->sample_connections,
        [](auto const& connection) {
            return !connection.canonical_source_port
                && connection.source_channel_timings.size() == 2;
        });
    ASSERT_NE(
        composed_history_connection,
        composed_history_analysis->sample_connections.end());
    ASSERT_EQ(composed_history_connection->source_channel_timings.size(), 2u);
    EXPECT_EQ(composed_history_connection->target_history, 5u);
    EXPECT_EQ(
        composed_history_connection->source_channel_timings[0].read_latency,
        7u);
    EXPECT_EQ(
        composed_history_connection->source_channel_timings[1].read_latency,
        2u);

    auto producer_group_for = [&](auto const& channel) {
        auto const source_port = iv::NodeBundlePortId{
            channel.source.bundle,
            iv::PortKind::sample,
            channel.source.port,
        };
        return std::ranges::find_if(
            composed_history_analysis->sample_producer_groups,
            [&](auto const& group) {
                return group.source_port && *group.source_port == source_port;
            });
    };
    auto direct_history_group = producer_group_for(
        composed_history_connection->source_channel_timings[0]);
    auto delayed_history_group = producer_group_for(
        composed_history_connection->source_channel_timings[1]);
    ASSERT_NE(
        direct_history_group,
        composed_history_analysis->sample_producer_groups.end());
    ASSERT_NE(
        delayed_history_group,
        composed_history_analysis->sample_producer_groups.end());
    EXPECT_EQ(direct_history_group->storage_requirements.retained_frames, 12u);
    EXPECT_EQ(delayed_history_group->storage_requirements.retained_frames, 7u);

    auto composed_history_plan =
        iv::graph_jit::detail::build_sample_storage_plan(
            *composed_history_analysis, 64);
    ASSERT_TRUE(composed_history_plan.has_value())
        << (composed_history_plan
                ? std::string{}
                : composed_history_plan.error());
    EXPECT_TRUE(composed_history_plan->compositions.empty());
    auto const composed_history_connection_index = static_cast<std::size_t>(
        std::distance(
            composed_history_analysis->sample_connections.begin(),
            composed_history_connection));
    ASSERT_LT(
        composed_history_connection_index,
        composed_history_plan->connection_channel_bindings.size());
    ASSERT_TRUE(
        composed_history_plan
            ->connection_channel_bindings[composed_history_connection_index]
            .has_value());
    auto const& composed_history_aliases =
        *composed_history_plan
            ->connection_channel_bindings[composed_history_connection_index];
    ASSERT_EQ(composed_history_aliases.size(), 2u);
    EXPECT_EQ(composed_history_aliases[0].frame_delay, 7u);
    EXPECT_EQ(composed_history_aliases[1].frame_delay, 2u);
    EXPECT_NE(
        composed_history_aliases[0].representation,
        composed_history_aliases[1].representation);

    auto composed_history = compile_graph(composed_history_graph, 121);
    ASSERT_TRUE(composed_history.succeeded())
        << (composed_history.diagnostics.empty()
                ? ""
                : composed_history.diagnostics.front().message);
    ASSERT_EQ(composed_history.compiled_graph->node_layout.nodes.size(), 3u);

    auto composed_history_storage =
        composed_history.compiled_graph->node_layout.create_storage(resources);
    composed_history_storage.initialize();
    StereoHistoryConsumerStateMirror* composed_history_probe = nullptr;
    for (std::size_t i = 0;
         i < composed_history.compiled_graph->node_layout.nodes.size(); ++i) {
        if (composed_history.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(StereoHistoryConsumerStateMirror)) {
            ASSERT_EQ(composed_history_probe, nullptr);
            composed_history_probe =
                static_cast<StereoHistoryConsumerStateMirror*>(
                    composed_history_storage.state_ptr(i));
        }
    }
    ASSERT_NE(composed_history_probe, nullptr);

    composed_history.compiled_graph->root_operations.tick_block(
        composed_history_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(composed_history_probe->calls, 1u);
    EXPECT_EQ(composed_history_probe->last_index, 0u);
    EXPECT_FLOAT_EQ(composed_history_probe->current_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->current_right, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_left, 0.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_right, 0.0f);

    // At absolute index 64 both channels represent source frame 57 after path
    // equalization, and get(5) must reconstruct source frame 52 on each side.
    composed_history.compiled_graph->root_operations.tick_block(
        composed_history_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(composed_history_probe->calls, 2u);
    EXPECT_EQ(composed_history_probe->last_index, 64u);
    EXPECT_FLOAT_EQ(composed_history_probe->current_left, 57.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->current_right, 57.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_left, 52.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_right, 52.0f);
    EXPECT_EQ(composed_history_probe->marker, 0xa1b2c3d4e5f60718ull);

    // A short third block catches composition code that accidentally assumes
    // the kernel's configured block size while reconstructing target history.
    composed_history.compiled_graph->root_operations.tick_block(
        composed_history_storage.buffer().data(), &empty_tick_invocation, index(128), 4);
    EXPECT_EQ(composed_history_probe->calls, 3u);
    EXPECT_EQ(composed_history_probe->last_index, 128u);
    EXPECT_FLOAT_EQ(composed_history_probe->current_left, 121.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->current_right, 121.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_left, 116.0f);
    EXPECT_FLOAT_EQ(composed_history_probe->history_5_right, 116.0f);

    // Split a valid whole-port projected composition into two configured mono
    // target-channel contributions. Connection analysis must normalize them
    // back into one full stereo logical input, preserving both the source
    // permutation (stereo right -> target left) and independent path latency.
}

TEST_F(GraphJitRuntimeFixture, ProjectedSampleComposition)
{
    auto projected_base_graph = configured_module_graph(
        *revision, graph_jit_projected_composition_module_id);
    ASSERT_TRUE(projected_base_graph);
    auto projected_graph = std::make_shared<iv::ConfiguredGraph>(*projected_base_graph);
    std::vector<iv::ConfiguredSampleConnection> projected_connections;
    for (auto const& connection :
         projected_base_graph->connections.configured_sample_connections()) {
        if (connection.source_type == iv::ChannelTypeId::stereo
            && connection.target_type == iv::ChannelTypeId::stereo
            && connection.source_channels.size() == 2
            && connection.target_channels.size() == 2) {
            // The left target is deliberately expressed as stereo->mono using
            // semantic channels from two distinct producer ports. The
            // conversion must read those sources directly without gathering a
            // contiguous stereo input. The right target remains a direct alias,
            // so one logical input mixes computed and aliased channel bindings.
            projected_connections.push_back(iv::ConfiguredSampleConnection{
                .source_type = iv::ChannelTypeId::stereo,
                .source_channels = {
                    connection.source_channels[0],
                    connection.source_channels[1],
                },
                .target_type = iv::ChannelTypeId::mono,
                .target_channels = {connection.target_channels[0]},
            });
            projected_connections.push_back(iv::ConfiguredSampleConnection{
                .source_type = iv::ChannelTypeId::mono,
                .source_channels = {connection.source_channels[1]},
                .target_type = iv::ChannelTypeId::mono,
                .target_channels = {connection.target_channels[1]},
            });
        } else {
            projected_connections.push_back(connection);
        }
    }
    ASSERT_EQ(projected_connections.size(), 3u);
    projected_graph->connections = iv::GraphBuilderConnections::from_configured_connections(
        projected_connections,
        projected_base_graph->connections.configured_event_connections());

    auto projected_analysis = build_connection_analysis_plan(
        *projected_graph, 64);
    ASSERT_TRUE(projected_analysis.has_value())
        << (projected_analysis ? std::string{} : projected_analysis.error());
    // The two target-channel contributions normalize into one logical sink
    // connection, alongside the mono connection feeding FiveSampleDelay.
    ASSERT_EQ(projected_analysis->sample_connections.size(), 2u);
    auto projected_connection = std::ranges::find_if(
        projected_analysis->sample_connections,
        [](auto const& connection) {
            return connection.target_type == iv::ChannelTypeId::stereo;
        });
    ASSERT_NE(projected_connection, projected_analysis->sample_connections.end());
    EXPECT_FALSE(projected_connection->canonical_source_port.has_value());
    ASSERT_GE(projected_connection->source_channel_timings.size(), 2u);
    ASSERT_EQ(projected_connection->target_channels.size(), 2u);
    ASSERT_EQ(projected_connection->projection_contributions.size(), 2u);
    auto const mixed = std::ranges::find_if(
        projected_connection->projection_contributions,
        [](auto const& contribution) {
            return contribution.source_type == iv::ChannelTypeId::stereo
                && contribution.target_type == iv::ChannelTypeId::mono;
        });
    ASSERT_NE(mixed, projected_connection->projection_contributions.end());
    ASSERT_EQ(mixed->source_channel_indices.size(), 2u);
    ASSERT_EQ(mixed->target_channels, (std::vector<std::size_t>{0u}));
    auto const aliased = std::ranges::find_if(
        projected_connection->projection_contributions,
        [](auto const& contribution) {
            return contribution.source_type == iv::ChannelTypeId::mono
                && contribution.target_type == iv::ChannelTypeId::mono;
        });
    ASSERT_NE(aliased, projected_connection->projection_contributions.end());
    ASSERT_EQ(aliased->target_channels, (std::vector<std::size_t>{1u}));
    EXPECT_EQ(projected_connection->target_channels[0].channel, 0u);
    EXPECT_EQ(projected_connection->target_channels[1].channel, 1u);

    auto projected_plan = iv::graph_jit::detail::build_sample_storage_plan(
        *projected_analysis, 64);
    ASSERT_TRUE(projected_plan.has_value())
        << (projected_plan ? std::string{} : projected_plan.error());
    ASSERT_EQ(projected_plan->compositions.size(), 1u);
    auto const projected_connection_index = static_cast<std::size_t>(
        std::distance(
            projected_analysis->sample_connections.begin(),
            projected_connection));
    ASSERT_LT(
        projected_connection_index,
        projected_plan->connection_channel_bindings.size());
    ASSERT_TRUE(
        projected_plan
            ->connection_channel_bindings[projected_connection_index]
            .has_value());
    auto const& projected_bindings =
        *projected_plan
            ->connection_channel_bindings[projected_connection_index];
    ASSERT_EQ(projected_bindings.size(), 2u);

    auto const& computed = projected_plan->compositions.front();
    ASSERT_EQ(computed.connection_index, projected_connection_index);
    ASSERT_EQ(computed.contributions.size(), 1u);
    EXPECT_EQ(
        computed.contributions.front().source_layout.channel_type,
        iv::ChannelTypeId::stereo);
    EXPECT_EQ(
        computed.contributions.front().converted_layout.channel_type,
        iv::ChannelTypeId::mono);
    ASSERT_EQ(computed.contributions.front().sources.size(), 2u);
    EXPECT_NE(
        computed.contributions.front().sources[0].source_representation,
        computed.contributions.front().sources[1].source_representation);
    EXPECT_EQ(projected_bindings[0].representation, computed.target_representation);
    EXPECT_EQ(projected_bindings[0].representation_channel, 0u);
    EXPECT_EQ(projected_bindings[0].frame_delay, 0u);

    EXPECT_NE(projected_bindings[1].representation, computed.target_representation);
    EXPECT_EQ(projected_bindings[1].representation_channel, 0u);
    EXPECT_EQ(projected_bindings[1].frame_delay, 2u);

    auto projected = compile_graph(projected_graph, 122);
    ASSERT_TRUE(projected.succeeded())
        << (projected.diagnostics.empty()
                ? ""
                : projected.diagnostics.front().message);
    auto projected_storage =
        projected.compiled_graph->node_layout.create_storage(resources);
    projected_storage.initialize();
    StereoSampleConsumerProbeStateMirror* projected_probe = nullptr;
    for (std::size_t i = 0;
         i < projected.compiled_graph->node_layout.nodes.size(); ++i) {
        if (projected.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(StereoSampleConsumerProbeStateMirror)) {
            ASSERT_EQ(projected_probe, nullptr);
            projected_probe = static_cast<StereoSampleConsumerProbeStateMirror*>(
                projected_storage.state_ptr(i));
        }
    }
    ASSERT_NE(projected_probe, nullptr);

    projected.compiled_graph->root_operations.tick_block(
        projected_storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(projected_probe->calls, 1u);
    EXPECT_FLOAT_EQ(projected_probe->first_left, 0.0f);
    EXPECT_FLOAT_EQ(projected_probe->first_right, 0.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_left, 556.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_right, 56.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_left, 30096.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_right, 1596.0f);

    projected.compiled_graph->root_operations.tick_block(
        projected_storage.buffer().data(), &empty_tick_invocation, index(64), 64);
    EXPECT_EQ(projected_probe->calls, 2u);
    EXPECT_FLOAT_EQ(projected_probe->first_left, 557.0f);
    EXPECT_FLOAT_EQ(projected_probe->first_right, 57.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_left, 620.0f);
    EXPECT_FLOAT_EQ(projected_probe->last_right, 120.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_left, 37664.0f);
    EXPECT_FLOAT_EQ(projected_probe->sum_right, 5664.0f);

}

// The probe's ramp input, whose neutral value is 100, reads the ramp
// compensated by 7. Its first 7 positions come before the ramp's first value,
// so they hold its neutral value.
TEST_F(GraphJitRuntimeFixture,
       CompensatedInputReadsItsNeutralValueBeforeItsSourceStarts)
{
    auto compiled = compile(graph_jit_neutral_compensated_module_id, 386);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);

    auto const* probe = find_unique_state<SequenceProbeStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe->read_latency, 7u);
    ASSERT_EQ(probe->recorded, 64u);
    for (std::size_t offset = 0; offset < 64; ++offset) {
        auto const wanted = offset < 7
            ? 100.0f
            : static_cast<float>(offset - 7);
        EXPECT_EQ(probe->values[offset], wanted) << "position " << offset;
    }
}

// The probe's ramp input, whose neutral value is 100, reads the ramp
// compensated by 7. A history reader, whose neutral value is 0, reads the same
// ramp. Before the ramp's first value, each input reads its own neutral value.
TEST_F(GraphJitRuntimeFixture,
       CompensatedInputBesideHistoryReaderReadsItsNeutralValueBeforeItsSourceStarts)
{
    // The executor's first generation, started as
    // GraphExecutor::ExecutionGeneration::initialize_first_generation() does.
    auto graph = configured_module_graph(
        *revision, graph_jit_shared_neutral_compensated_module_id);
    ASSERT_TRUE(graph);
    auto compiled = in_event_source([&] { return jit->handle_project_graph_transaction(
        iv::ProjectGraphGraphJitRequest{
            .successor = {
                .project_generation = 422,
                .graph = graph,
                .definitions = definitions,
            },
            .initial_generation = true,
        }); });
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    auto transition = false;
    if (auto const& cutover = compiled.compiled_graph->cutover) {
        ASSERT_TRUE(cutover->from_empty_graph);
        auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
            cutover->state_transition, {}, storage.buffer());
        ASSERT_TRUE(prepared.has_value())
            << (prepared ? std::string{} : prepared.error());
        iv::graph_jit::execute_realtime_port_state_transfers(
            *prepared, {}, storage.buffer(), index(0));
        transition = !cutover->state_transition.sample_expiry_cursors.empty();
    }
    auto const operation = transition
        ? compiled.compiled_graph->root_operations.transition_tick_block
        : compiled.compiled_graph->root_operations.tick_block;
    ASSERT_NE(operation, nullptr);
    operation(storage.buffer().data(), &empty_tick_invocation, index(0), 64);

    auto const* probe = find_unique_state<SequenceProbeStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe->read_latency, 7u);
    ASSERT_EQ(probe->recorded, 64u);
    for (std::size_t offset = 0; offset < 64; ++offset) {
        auto const wanted = offset < 7
            ? 100.0f
            : static_cast<float>(offset - 7);
        EXPECT_EQ(probe->values[offset], wanted) << "position " << offset;
    }
    auto const* history = find_unique_state<LargeHistoryConsumerStateMirror>(
        compiled.compiled_graph->node_layout, storage);
    ASSERT_NE(history, nullptr);
    EXPECT_EQ(history->calls, 1u);
    EXPECT_FLOAT_EQ(history->history_5000, 0.0f);
}

// Before the edit, the probe's ramp input is not connected and reads its default
// value 0. The edit connects it to the ramp, compensated by 7. A history reader
// keeps the ramp's recent values. The input reads 7 fresh positions, which hold
// its neutral value 0, then the ramp's values from the cutover on.
TEST_F(GraphJitRuntimeFixture, ConnectedInputReadsNeutralValuesBeforeItsNewSource)
{
    bool disconnected = false;
    bool connected = true;
    std::array predecessor_arguments{
        iv::details::configuration_argument(disconnected)};
    std::array successor_arguments{
        iv::details::configuration_argument(connected)};
    expect_probe_values_across_graph_edit(
        *jit, definitions, resources,
        configured_module_graph_with_arguments(
            *revision, graph_jit_connected_input_module_id,
            predecessor_arguments),
        configured_module_graph_with_arguments(
            *revision, graph_jit_connected_input_module_id,
            successor_arguments),
        [](std::int64_t position) {
            if (position < sequence_cutover + 7) return 0.0f;
            return ramp_value(position - 7);
        },
        388);
}

// Inside a feedback loop, a node reads a ramp of latency 7 and, over an edge
// that is not detached, the loop's own ramp. The loop's edges that are not
// detached form an acyclic order, so path-latency compensation aligns this
// node's inputs as it would outside a loop: it reads the loop ramp 7 positions
// late, and both inputs hold the same value at every position. (Only the
// detached edge keeps its authored delay.)
TEST_F(GraphJitRuntimeFixture, SccNodeAlignsItsInputsOfUnequalPathLatency)
{
    auto compiled = compile(graph_jit_scc_compensation_module_id, 466);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const [passthrough, probe] = run_scc_compensation(resources, compiled);
    (void)probe;
    // Called once per slice of the loop; every position is checked once.
    EXPECT_GE(passthrough.calls, 2u);
    EXPECT_EQ(passthrough.checked, 128u);
    EXPECT_EQ(passthrough.mismatches, 0u);
    EXPECT_EQ(passthrough.largest_difference, 0.0f);
}

// The passthrough's output carries its ramp input, 7 positions late. A probe
// after the loop compares it with a plain ramp; compensation delays the plain
// ramp by the loop node's output path latency, 7, so the probe reads both
// inputs equal.
TEST_F(GraphJitRuntimeFixture, NodeAfterAnSccIsCompensatedForTheSccsPathLatency)
{
    auto compiled = compile(graph_jit_scc_compensation_module_id, 468);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    auto const [passthrough, probe] = run_scc_compensation(resources, compiled);
    (void)passthrough;
    EXPECT_EQ(probe.calls, 2u);
    EXPECT_EQ(probe.mismatches, 0u);
    EXPECT_EQ(probe.max_abs_difference, 0.0f);
}

TEST_F(GraphJitRuntimeFixture, PersistentSampleHistory)
{
    auto persistent_history_graph = configured_module_graph(
        *revision, graph_jit_persistent_history_module_id);
    ASSERT_TRUE(persistent_history_graph);
    auto persistent_history_analysis =
        build_connection_analysis_plan(
            *persistent_history_graph, 64);
    ASSERT_TRUE(persistent_history_analysis.has_value())
        << (persistent_history_analysis
                ? std::string{}
                : persistent_history_analysis.error());
    ASSERT_EQ(persistent_history_analysis->sample_producer_groups.size(), 1u);
    ASSERT_TRUE(
        persistent_history_analysis->sample_producer_groups[0]
            .storage_plan.has_value());
    EXPECT_EQ(
        persistent_history_analysis->sample_producer_groups[0].storage_plan->kind,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(
        persistent_history_analysis->sample_producer_groups[0]
            .storage_requirements.retained_frames,
        5000u);

    auto persistent_history = compile_graph(persistent_history_graph, 118);
    ASSERT_TRUE(persistent_history.succeeded())
        << (persistent_history.diagnostics.empty()
                ? ""
                : persistent_history.diagnostics.front().message);
    ASSERT_EQ(count_raw_regions(persistent_history.compiled_graph->node_layout), 2u);
    auto const persistent_ring_region = std::ranges::find_if(
        persistent_history.compiled_graph->node_layout.regions,
        [](iv::NodeLayout::Region const& region) {
            return region.kind == iv::NodeLayout::Region::Kind::raw;
        });
    ASSERT_NE(
        persistent_ring_region,
        persistent_history.compiled_graph->node_layout.regions.end());
    EXPECT_EQ(
        persistent_ring_region->size,
        8192u * sizeof(iv::Sample));
    EXPECT_FALSE(persistent_ring_region->migration_identity.empty());

    auto const& requirements = persistent_history.compiled_graph
        ->realtime_port_state_requirements.states;
    auto const& realizations = persistent_history.compiled_graph
        ->realtime_port_state_realizations.sample_states;
    ASSERT_EQ(requirements.size(), 1u);
    ASSERT_EQ(realizations.size(), 1u);
    ASSERT_LT(realizations[0].requirement_index, requirements.size());
    EXPECT_EQ(
        requirements[realizations[0].requirement_index].role,
        iv::graph_jit::RealtimePortStateRole::sequential_input_history);
    EXPECT_EQ(
        realizations[0].authored_storage.storage,
        iv::graph_jit::SampleRealtimePortStateStorage::ring);
    EXPECT_EQ(
        realizations[0].retained_access,
        iv::graph_jit::SampleRealtimePortStateAccess::direct);
    ASSERT_EQ(realizations[0].retained_sources.size(), 1u);
    auto const& retained_ring = realizations[0].retained_sources.front();
    EXPECT_EQ(
        retained_ring.storage,
        iv::graph_jit::SampleRealtimePortStateStorage::ring);
    EXPECT_EQ(
        realizations[0].authored_storage.working_frame_capacity,
        8192u);
    EXPECT_EQ(retained_ring.working_frame_capacity, 8192u);
    EXPECT_EQ(retained_ring.storage_frame_count, 8192u);
    EXPECT_EQ(
        retained_ring.storage_size_bytes,
        8192u * sizeof(iv::Sample));
    ASSERT_TRUE(retained_ring.node_storage_offset);
    EXPECT_EQ(
        *retained_ring.node_storage_offset,
        persistent_ring_region->storage_offset);
    EXPECT_FALSE(retained_ring.callback_arena_offset);
    EXPECT_FALSE(realizations[0].materialization);

    auto persistent_history_storage =
        persistent_history.compiled_graph->node_layout.create_storage(resources);
    persistent_history_storage.initialize();
    LargeHistoryConsumerStateMirror* large_history_state = nullptr;
    for (std::size_t i = 0;
         i < persistent_history.compiled_graph->node_layout.nodes.size(); ++i) {
        if (persistent_history.compiled_graph->node_layout.nodes[i].state_size
            == sizeof(LargeHistoryConsumerStateMirror)) {
            large_history_state = static_cast<LargeHistoryConsumerStateMirror*>(
                persistent_history_storage.state_ptr(i));
        }
    }
    ASSERT_NE(large_history_state, nullptr);
    for (std::uint64_t index = 0; index <= 5056; index += 64) {
        persistent_history.compiled_graph->root_operations.tick_block(
            persistent_history_storage.buffer().data(),
            &empty_tick_invocation,
            iv::GlobalIndex::from_bits(index),
            64);
    }
    EXPECT_EQ(large_history_state->calls, 80u);
    EXPECT_EQ(large_history_state->last_index, 5056u);
    EXPECT_FLOAT_EQ(large_history_state->current, 5056.0f);
    EXPECT_FLOAT_EQ(large_history_state->history_1, 5055.0f);
    EXPECT_FLOAT_EQ(large_history_state->history_5000, 56.0f);

}

} // namespace iv::test::graph_jit_tests
