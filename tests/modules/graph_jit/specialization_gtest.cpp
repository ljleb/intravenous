#include "test_support.h"

namespace iv::test::graph_jit_tests {

namespace {

struct FeedbackGainStateMirror {
    std::uint64_t ticks = 0;
    std::uint64_t configured_id = 0;
    std::uint64_t retained_pointer_identity_matches = 0;
    float last_input = 0.0f;
    float last_gain = 0.0f;
    float last_output = 0.0f;
};

struct ImmutableHistoryReaderStateMirror {
    std::uint64_t ticks = 0;
    float last_input = 0.0f;
    float last_output = 0.0f;
};

} // namespace

TEST(GraphJit, SpecializationIsLatchedAtConstruction)
{
    iv::GraphJit jit(iv::GraphJitConfig{
        .sample_rate = 44100,
        .block_size = 512,
    });

    auto const& specialization = jit.specialization();
    EXPECT_EQ(specialization.sample_rate, 44100u);
    EXPECT_EQ(specialization.block_size, 512u);
    EXPECT_FALSE(specialization.target_triple.empty());
}

TEST_F(GraphJitRuntimeFixture, IndependentFeedbackRegionsRunSpecialized)
{
    auto compiled = compile(graph_jit_native_batch_feedback_module_id, 125);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.timings.size(), 1u);
    EXPECT_EQ(compiled.timings.front().region_specializations_compiled, 1u);
    EXPECT_EQ(compiled.timings.front().region_specializations_reused, 1u);
    EXPECT_GT(
        compiled.timings.front().region_specialization_retargeted_functions,
        0u);
    EXPECT_EQ(compiled.timings.front().region_specialization_failures, 0u)
        << compiled.timings.front().region_specialization_failure;
    ASSERT_TRUE(compiled.compiled_graph->root_operations.valid());
    ASSERT_EQ(compiled.compiled_graph->node_layout.nodes.size(), 4u);

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::array<NativeBatchFeedbackProbeStateMirror*, 4> states{};
    for (std::size_t node = 0; node < states.size(); ++node) {
        states[node] = static_cast<NativeBatchFeedbackProbeStateMirror*>(
            storage.state_ptr(node));
        ASSERT_NE(states[node], nullptr);
    }

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 4);
    for (auto const* state : states) {
        EXPECT_EQ(state->tick_batch_calls, 4u);
        // A region specialization calls the normalized scalar tick_block.
        // The batch-only authored callback therefore receives one lane.
        EXPECT_EQ(state->last_lane_count, 1u);
    }
    EXPECT_FLOAT_EQ(states[0]->last_output, 10.0f);
    EXPECT_FLOAT_EQ(states[1]->last_output, 12.0f);
    EXPECT_FLOAT_EQ(states[2]->last_output, 100.0f);
    EXPECT_FLOAT_EQ(states[3]->last_output, 120.0f);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(4), 4);
    for (auto const* state : states) {
        EXPECT_EQ(state->tick_batch_calls, 8u);
        EXPECT_EQ(state->last_lane_count, 1u);
    }
    EXPECT_FLOAT_EQ(states[0]->last_output, 22.0f);
    EXPECT_FLOAT_EQ(states[1]->last_output, 24.0f);
    EXPECT_FLOAT_EQ(states[2]->last_output, 220.0f);
    EXPECT_FLOAT_EQ(states[3]->last_output, 240.0f);
}

// A cyclic region of pointwise nodes reading a Constant compiles into one
// region specialization, which the realtime root runs in place of its slice
// loop. It produces the interpreter's values across root calls, with and
// without a step operation (the copied timeline of a detach with a non-zero
// initial value). A later compilation of the same region reuses the compiled
// code; a different Constant value is a different code input and compiles
// again.
TEST_F(GraphJitRuntimeFixture, CyclicRegionSpecializationMatchesInterpreterAndIsReused)
{
    iv::GraphJit interpreter_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .specialize_regions = false,
    });
    auto states = [](iv::CompiledGraph const& graph, iv::NodeStorage& storage) {
        // Configured order: the Constant, which has no State, then head and
        // tail.
        std::vector<FeedbackGainStateMirror*> result;
        for (std::size_t node = 0; node < graph.node_layout.nodes.size(); ++node) {
            if (graph.node_layout.nodes[node].state_size
                != sizeof(FeedbackGainStateMirror)) {
                continue;
            }
            result.push_back(
                static_cast<FeedbackGainStateMirror*>(storage.state_ptr(node)));
        }
        return result;
    };
    // Compiles the module with and without region specializations, runs three
    // root calls of both, and checks them against each other and against
    // head[n] = tail[n - 1] * gain + 1 and tail[n] = head[n] * gain + 2.
    auto compare = [&](float gain, float initial, std::uint64_t generation)
        -> std::optional<iv::GraphJitCompileTimings> {
        std::array arguments{
            iv::details::configuration_argument(gain),
            iv::details::configuration_argument(initial),
        };
        auto const graph = configured_module_graph_with_arguments(
            *revision, graph_jit_constant_gain_feedback_module_id, arguments);
        auto specialized = compile_graph(graph, generation);
        auto interpreted = interpreter_jit.compile(iv::GraphJitCompileRequest{
            .project_generation = generation,
            .graph = graph,
            .definitions = definitions,
        });
        EXPECT_TRUE(specialized.succeeded())
            << (specialized.diagnostics.empty()
                    ? ""
                    : specialized.diagnostics.front().message);
        EXPECT_TRUE(interpreted.succeeded())
            << (interpreted.diagnostics.empty()
                    ? ""
                    : interpreted.diagnostics.front().message);
        if (!specialized.succeeded() || !interpreted.succeeded()
            || specialized.timings.size() != 1 || interpreted.timings.size() != 1) {
            ADD_FAILURE() << "compilation did not produce one steady result";
            return std::nullopt;
        }
        EXPECT_EQ(interpreted.timings.front().region_specializations_compiled, 0u);
        EXPECT_EQ(interpreted.timings.front().region_specializations_reused, 0u);

        auto specialized_storage =
            specialized.compiled_graph->node_layout.create_storage(resources);
        specialized_storage.initialize();
        auto interpreted_storage =
            interpreted.compiled_graph->node_layout.create_storage(resources);
        interpreted_storage.initialize();
        auto const specialized_states =
            states(*specialized.compiled_graph, specialized_storage);
        auto const interpreted_states =
            states(*interpreted.compiled_graph, interpreted_storage);
        if (specialized_states.size() != 2 || interpreted_states.size() != 2) {
            ADD_FAILURE() << "the module does not have two feedback nodes";
            return std::nullopt;
        }

        float head = 0.0f;
        float tail = initial;
        for (std::int64_t block = 0; block < 3; ++block) {
            specialized.compiled_graph->root_operations.tick_block(
                specialized_storage.buffer().data(),
                &empty_tick_invocation,
                index(block * 64),
                64);
            interpreted.compiled_graph->root_operations.tick_block(
                interpreted_storage.buffer().data(),
                &empty_tick_invocation,
                index(block * 64),
                64);
            for (std::size_t sample = 0; sample < 64; ++sample) {
                head = tail * gain + 1.0f;
                tail = head * gain + 2.0f;
            }
            for (std::size_t node = 0; node < 2; ++node) {
                auto const& actual = *specialized_states[node];
                auto const& expected = *interpreted_states[node];
                EXPECT_EQ(actual.ticks, static_cast<std::uint64_t>((block + 1) * 64));
                EXPECT_EQ(actual.ticks, expected.ticks);
                EXPECT_EQ(actual.configured_id, node + 1);
                EXPECT_EQ(actual.retained_pointer_identity_matches, 1u);
                EXPECT_EQ(
                    actual.retained_pointer_identity_matches,
                    expected.retained_pointer_identity_matches);
                EXPECT_FLOAT_EQ(actual.last_gain, gain);
                EXPECT_FLOAT_EQ(actual.last_input, expected.last_input);
                EXPECT_FLOAT_EQ(actual.last_output, expected.last_output);
            }
            EXPECT_FLOAT_EQ(specialized_states[0]->last_output, head);
            EXPECT_FLOAT_EQ(specialized_states[1]->last_output, tail);
        }
        return specialized.timings.front();
    };

    auto const first = compare(0.5f, 0.0f, 120);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->region_specializations_compiled, 1u);
    EXPECT_EQ(first->region_specializations_reused, 0u);
    EXPECT_EQ(first->region_specializations_skipped, 0u)
        << first->region_specialization_skip;
    EXPECT_EQ(first->region_specialization_failures, 0u)
        << first->region_specialization_failure;
    // The inlined callbacks read the Constant gain and the context's sample
    // rate from the workspace; those reads become the fields' values.
    EXPECT_GT(first->region_specialization_substitutions, 0u);

    // compare() released its compiled graphs. The byte-bounded recent-use
    // cache still owns the specialization, so held reuses it. A second
    // compilation also reuses the same code while held owns it.
    float gain = 0.5f;
    float initial = 0.0f;
    std::array arguments{
        iv::details::configuration_argument(gain),
        iv::details::configuration_argument(initial),
    };
    auto const graph = configured_module_graph_with_arguments(
        *revision, graph_jit_constant_gain_feedback_module_id, arguments);
    auto held = compile_graph(graph, 121);
    ASSERT_TRUE(held.succeeded());
    ASSERT_EQ(held.timings.size(), 1u);
    EXPECT_EQ(held.timings.front().region_specializations_compiled, 0u);
    EXPECT_EQ(held.timings.front().region_specializations_reused, 1u);
    EXPECT_GT(held.timings.front().region_specialization_cache_bytes, 0u);
    EXPECT_LE(
        held.timings.front().region_specialization_cache_bytes,
        held.timings.front().region_specialization_cache_budget_bytes);
    auto reused = compile_graph(graph, 122);
    ASSERT_TRUE(reused.succeeded());
    ASSERT_EQ(reused.timings.size(), 1u);
    EXPECT_EQ(reused.timings.front().region_specializations_compiled, 0u);
    EXPECT_EQ(reused.timings.front().region_specializations_reused, 1u);

    auto const changed = compare(0.25f, 0.0f, 123);
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->region_specializations_compiled, 1u);
    EXPECT_EQ(changed->region_specializations_reused, 0u);

    auto const copied = compare(0.5f, 0.25f, 124);
    ASSERT_TRUE(copied);
    EXPECT_EQ(copied->region_specializations_compiled, 1u);
    EXPECT_EQ(copied->region_specializations_skipped, 0u)
        << copied->region_specialization_skip;
    EXPECT_EQ(copied->region_specialization_failures, 0u)
        << copied->region_specialization_failure;

    // A one-byte budget cannot retain generated code. The first compilation
    // reports its allocated code memory, which is whole pages. After its graph
    // is released, the second compilation must generate the specialization
    // again.
    iv::GraphJit one_byte_cache(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .region_specialization_cache_budget_bytes = 1,
    });
    {
        auto limited = one_byte_cache.compile(iv::GraphJitCompileRequest{
            .project_generation = 128,
            .graph = graph,
            .definitions = definitions,
        });
        ASSERT_TRUE(limited.succeeded())
            << (limited.diagnostics.empty()
                    ? ""
                    : limited.diagnostics.front().message);
        ASSERT_EQ(limited.timings.size(), 1u);
        EXPECT_EQ(limited.timings.front().region_specializations_compiled, 1u);
        auto const allocated =
            limited.timings.front().region_specialization_allocated_bytes;
        auto const page_size = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
        EXPECT_GE(allocated, page_size);
        EXPECT_EQ(allocated % page_size, 0u);
        EXPECT_EQ(limited.timings.front().region_specialization_cache_bytes, 0u);
        EXPECT_EQ(
            limited.timings.front().region_specialization_cache_budget_bytes,
            1u);
    }
    auto limited_again = one_byte_cache.compile(iv::GraphJitCompileRequest{
        .project_generation = 129,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(limited_again.succeeded())
        << (limited_again.diagnostics.empty()
                ? ""
                : limited_again.diagnostics.front().message);
    ASSERT_EQ(limited_again.timings.size(), 1u);
    EXPECT_EQ(
        limited_again.timings.front().region_specializations_compiled,
        1u);
    EXPECT_EQ(limited_again.timings.front().region_specializations_reused, 0u);
}

// The recent-use cache owns region specializations within its byte budget and
// evicts the least recently requested one first. Eviction removes only the
// cache's ownership: code that a compiled graph still runs keeps working, a
// later request reuses it, and the cache then owns it again. A specialization
// larger than the budget is reused while a compiled graph owns it and compiled
// again once nothing does.
TEST_F(GraphJitRuntimeFixture, RegionSpecializationCacheEvictsLeastRecentlyUsedCode)
{
    // Three gains of one cyclic region: three value specializations.
    std::array const gains{0.5f, 0.25f, 0.125f};
    // configuration_argument() keeps its argument's address, so each value
    // is a named object that outlives the graph construction.
    auto gain_graph = [&](float gain) {
        float initial = 0.0f;
        std::array arguments{
            iv::details::configuration_argument(gain),
            iv::details::configuration_argument(initial),
        };
        return configured_module_graph_with_arguments(
            *revision, graph_jit_constant_gain_feedback_module_id, arguments);
    };
    std::array const graphs{
        gain_graph(gains[0]),
        gain_graph(gains[1]),
        gain_graph(gains[2]),
    };
    auto make_jit = [](std::size_t budget) {
        return std::make_unique<iv::GraphJit>(iv::GraphJitConfig{
            .sample_rate = 88200,
            .block_size = 64,
            .region_specialization_cache_budget_bytes = budget,
        });
    };
    std::uint64_t generation = 140;
    auto compile_with = [&](iv::GraphJit& graph_jit, std::size_t graph) {
        auto result = graph_jit.compile(iv::GraphJitCompileRequest{
            .project_generation = generation++,
            .graph = graphs[graph],
            .definitions = definitions,
        });
        EXPECT_TRUE(result.succeeded())
            << (result.diagnostics.empty() ? "" : result.diagnostics.front().message);
        EXPECT_EQ(result.timings.size(), 1u);
        return result;
    };
    struct Counts {
        std::uint64_t compiled = 0;
        std::uint64_t reused = 0;
        std::uint64_t allocated = 0;
        std::uint64_t cache = 0;
    };
    auto counts = [](iv::GraphJitCompileResult const& result) {
        if (result.timings.size() != 1) return Counts{};
        auto const& timings = result.timings.front();
        return Counts{
            .compiled = timings.region_specializations_compiled,
            .reused = timings.region_specializations_reused,
            .allocated = timings.region_specialization_allocated_bytes,
            .cache = timings.region_specialization_cache_bytes,
        };
    };
    // Compiles one graph and releases the result at once, so only the cache
    // can keep the graph's specialization alive.
    auto released = [&](iv::GraphJit& graph_jit, std::size_t graph) {
        return counts(compile_with(graph_jit, graph));
    };

    // Each specialization's allocated code memory, from a GraphJit whose
    // budget holds all three. The code is the same in every GraphJit; each
    // compilation below checks that.
    std::array<std::uint64_t, 3> sizes{};
    {
        auto probe = make_jit(
            iv::GraphJitConfig{}.region_specialization_cache_budget_bytes);
        for (std::size_t graph = 0; graph < graphs.size(); ++graph) {
            auto const measured = released(*probe, graph);
            EXPECT_EQ(measured.compiled, 1u);
            sizes[graph] = measured.allocated;
        }
    }
    auto const [a, b, c] = sizes;
    ASSERT_GT(a, 1u);
    ASSERT_GT(b, 0u);
    ASSERT_GT(c, 0u);

    // A budget that holds A with B or with C, never all three.
    {
        auto lru = make_jit(std::max(a + b, a + c));
        auto step = released(*lru, 0);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.allocated, a);
        EXPECT_EQ(step.cache, a);
        step = released(*lru, 1);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.allocated, b);
        EXPECT_EQ(step.cache, a + b);
        // Requesting A makes B the least recently requested entry.
        step = released(*lru, 0);
        EXPECT_EQ(step.compiled, 0u);
        EXPECT_EQ(step.reused, 1u);
        EXPECT_EQ(step.cache, a + b);
        // C evicts B, not A.
        step = released(*lru, 2);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.allocated, c);
        EXPECT_EQ(step.cache, a + c);
        step = released(*lru, 0);
        EXPECT_EQ(step.compiled, 0u);
        EXPECT_EQ(step.reused, 1u);
        EXPECT_EQ(step.cache, a + c);
        // B was released when it was evicted. Owning it again evicts C, now
        // the least recently requested entry.
        step = released(*lru, 1);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.reused, 0u);
        EXPECT_EQ(step.cache, a + b);
        step = released(*lru, 2);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.reused, 0u);
    }

    // A budget that holds A or B, never both.
    {
        auto single = make_jit(std::max(a, b));
        auto held = compile_with(*single, 0);
        auto step = counts(held);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.cache, a);
        // B evicts A from the cache. held still owns A.
        step = released(*single, 1);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.cache, b);
        ASSERT_TRUE(held.succeeded());
        {
            // A's code still runs after its eviction.
            auto storage = held.compiled_graph->node_layout.create_storage(resources);
            storage.initialize();
            std::vector<FeedbackGainStateMirror const*> states;
            auto const& nodes = held.compiled_graph->node_layout.nodes;
            for (std::size_t node = 0; node < nodes.size(); ++node) {
                if (nodes[node].state_size != sizeof(FeedbackGainStateMirror)) continue;
                states.push_back(static_cast<FeedbackGainStateMirror const*>(
                    storage.state_ptr(node)));
            }
            ASSERT_EQ(states.size(), 2u);
            float head = 0.0f;
            float tail = 0.0f;
            for (std::int64_t block = 0; block < 3; ++block) {
                held.compiled_graph->root_operations.tick_block(
                    storage.buffer().data(),
                    &empty_tick_invocation,
                    index(block * 64),
                    64);
                for (std::size_t sample = 0; sample < 64; ++sample) {
                    head = tail * gains[0] + 1.0f;
                    tail = head * gains[0] + 2.0f;
                }
                EXPECT_EQ(states[0]->ticks, static_cast<std::uint64_t>((block + 1) * 64));
                EXPECT_FLOAT_EQ(states[0]->last_output, head);
                EXPECT_FLOAT_EQ(states[1]->last_output, tail);
            }
        }
        // A is reused through held, and the cache owns it again in place of B.
        step = released(*single, 0);
        EXPECT_EQ(step.compiled, 0u);
        EXPECT_EQ(step.reused, 1u);
        EXPECT_EQ(step.cache, a);
        held = {};
        step = released(*single, 0);
        EXPECT_EQ(step.compiled, 0u);
        EXPECT_EQ(step.reused, 1u);
        step = released(*single, 1);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.reused, 0u);
    }

    // A budget smaller than A.
    {
        auto small = make_jit(a - 1);
        auto held = compile_with(*small, 0);
        auto step = counts(held);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.allocated, a);
        EXPECT_EQ(step.cache, 0u);
        step = released(*small, 0);
        EXPECT_EQ(step.compiled, 0u);
        EXPECT_EQ(step.reused, 1u);
        EXPECT_EQ(step.cache, 0u);
        held = {};
        step = released(*small, 0);
        EXPECT_EQ(step.compiled, 1u);
        EXPECT_EQ(step.reused, 0u);
    }
}

// A package rebuild supersedes the revision whose node code a cached
// specialization calls, and region_specialization_key() names each revision's
// retained-global addresses, so that code can never be reused. The first
// compilation with the successor's definitions stops owning it. The superseded
// PackageRevision is then released once nothing else uses it.
TEST_F(GraphJitRuntimeFixture, RegionSpecializationCacheReleasesSupersededPackageRevision)
{
    // configuration_argument() keeps its argument's address, so each value
    // is a named object that outlives the graph construction.
    auto gain_graph = [](iv::PackageRevision const& package) {
        float gain = 0.5f;
        float initial = 0.0f;
        std::array arguments{
            iv::details::configuration_argument(gain),
            iv::details::configuration_argument(initial),
        };
        return configured_module_graph_with_arguments(
            package, graph_jit_constant_gain_feedback_module_id, arguments);
    };
    auto first_graph = gain_graph(*revision);
    {
        auto first = compile_graph(first_graph, 170);
        ASSERT_TRUE(first.succeeded())
            << (first.diagnostics.empty() ? "" : first.diagnostics.front().message);
        ASSERT_EQ(first.timings.size(), 1u);
        EXPECT_EQ(first.timings.front().region_specializations_compiled, 1u);
        EXPECT_GT(first.timings.front().region_specialization_cache_bytes, 0u);
    }
    std::weak_ptr<iv::PackageRevision const> const superseded = revision;

    // Loading the package again stands in for a rebuild: the successor has
    // its own package code and retained-global addresses.
    std::shared_ptr<iv::PackageRevision const> successor;
    ASSERT_NO_THROW(successor = load_graph_jit_runtime_revision());
    ASSERT_TRUE(successor);
    ASSERT_NE(successor, revision);
    auto const successor_definitions = make_graph_jit_snapshot(successor, 92);
    auto second = jit->compile(iv::GraphJitCompileRequest{
        .project_generation = 171,
        .graph = gain_graph(*successor),
        .definitions = successor_definitions,
    });
    ASSERT_TRUE(second.succeeded())
        << (second.diagnostics.empty() ? "" : second.diagnostics.front().message);
    ASSERT_EQ(second.timings.size(), 1u);
    auto const& timings = second.timings.front();
    EXPECT_EQ(timings.region_specializations_compiled, 1u);
    EXPECT_EQ(timings.region_specializations_reused, 0u);
    // The cache owns only the successor's specialization.
    EXPECT_EQ(
        timings.region_specialization_cache_bytes,
        timings.region_specialization_allocated_bytes);

    first_graph.reset();
    definitions.reset();
    revision.reset();
    EXPECT_TRUE(superseded.expired());
}

// An acyclic node reading immutable inputs with history, through get() and
// block_extended(), runs as a region specialization. The interpreter's
// constant ring and contiguous scratch serve both reads. Two equal readers
// share one compiled function; the ramp, which reads no immutable input,
// stays on its native callback.
TEST_F(GraphJitRuntimeFixture, AcyclicRegionWithImmutableHistoryRunsSpecialized)
{
    iv::GraphJit interpreter_jit(iv::GraphJitConfig{
        .sample_rate = 88200,
        .block_size = 64,
        .specialize_regions = false,
    });
    auto states = [](iv::CompiledGraph const& graph, iv::NodeStorage& storage) {
        // Configured order: the ramp and the Constant, which have no State,
        // then the readers with ids 1 and 2.
        std::vector<ImmutableHistoryReaderStateMirror*> result;
        for (std::size_t node = 0; node < graph.node_layout.nodes.size(); ++node) {
            if (graph.node_layout.nodes[node].state_size
                != sizeof(ImmutableHistoryReaderStateMirror)) {
                continue;
            }
            result.push_back(static_cast<ImmutableHistoryReaderStateMirror*>(
                storage.state_ptr(node)));
        }
        return result;
    };
    float gain = 0.5f;
    std::array arguments{iv::details::configuration_argument(gain)};
    auto const graph = configured_module_graph_with_arguments(
        *revision, graph_jit_immutable_history_module_id, arguments);
    auto specialized = compile_graph(graph, 126);
    auto interpreted = interpreter_jit.compile(iv::GraphJitCompileRequest{
        .project_generation = 126,
        .graph = graph,
        .definitions = definitions,
    });
    ASSERT_TRUE(specialized.succeeded())
        << (specialized.diagnostics.empty()
                ? ""
                : specialized.diagnostics.front().message);
    ASSERT_TRUE(interpreted.succeeded())
        << (interpreted.diagnostics.empty()
                ? ""
                : interpreted.diagnostics.front().message);
    ASSERT_EQ(specialized.timings.size(), 1u);
    auto const& timings = specialized.timings.front();
    // One specialization for the ramp, which reads no immutable input, and
    // one that both readers share.
    EXPECT_EQ(timings.region_specializations_compiled, 2u);
    EXPECT_EQ(timings.region_specializations_reused, 1u);
    EXPECT_EQ(timings.region_specializations_skipped, 0u)
        << timings.region_specialization_skip;
    EXPECT_EQ(timings.region_specialization_failures, 0u)
        << timings.region_specialization_failure;
    EXPECT_GT(timings.region_specialization_substitutions, 0u);

    auto specialized_storage =
        specialized.compiled_graph->node_layout.create_storage(resources);
    specialized_storage.initialize();
    auto interpreted_storage =
        interpreted.compiled_graph->node_layout.create_storage(resources);
    interpreted_storage.initialize();
    auto const specialized_states =
        states(*specialized.compiled_graph, specialized_storage);
    auto const interpreted_states =
        states(*interpreted.compiled_graph, interpreted_storage);
    ASSERT_EQ(specialized_states.size(), 2u);
    ASSERT_EQ(interpreted_states.size(), 2u);

    for (std::int64_t block = 0; block < 3; ++block) {
        specialized.compiled_graph->root_operations.tick_block(
            specialized_storage.buffer().data(),
            &empty_tick_invocation,
            index(block * 64),
            64);
        interpreted.compiled_graph->root_operations.tick_block(
            interpreted_storage.buffer().data(),
            &empty_tick_invocation,
            index(block * 64),
            64);
        auto const last = static_cast<float>(block * 64 + 63);
        for (std::size_t node = 0; node < 2; ++node) {
            auto const& actual = *specialized_states[node];
            auto const& expected = *interpreted_states[node];
            EXPECT_EQ(actual.ticks, static_cast<std::uint64_t>((block + 1) * 64));
            EXPECT_EQ(actual.ticks, expected.ticks);
            EXPECT_FLOAT_EQ(actual.last_input, last);
            EXPECT_FLOAT_EQ(actual.last_input, expected.last_input);
            // in * gain + offset + (gain + gain) + id.
            EXPECT_FLOAT_EQ(
                actual.last_output,
                last * gain + 0.125f + 2.0f * gain + static_cast<float>(node + 1));
            EXPECT_FLOAT_EQ(actual.last_output, expected.last_output);
        }
    }
}

TEST_F(GraphJitRuntimeFixture, DistinctImmutableValuesShareOneStructureSpecialization)
{
    auto compiled = compile(graph_jit_distinct_immutable_history_module_id, 127);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty()
                ? ""
                : compiled.diagnostics.front().message);
    ASSERT_EQ(compiled.timings.size(), 1u);
    auto const& timings = compiled.timings.front();
    // The ramp's specialization, and one structure specialization that both
    // readers share.
    EXPECT_EQ(timings.region_specializations_compiled, 2u);
    EXPECT_EQ(timings.region_specializations_reused, 1u);
    EXPECT_EQ(timings.region_specialization_failures, 0u)
        << timings.region_specialization_failure;

    auto storage = compiled.compiled_graph->node_layout.create_storage(resources);
    storage.initialize();
    std::vector<ImmutableHistoryReaderStateMirror*> states;
    for (std::size_t node = 0;
         node < compiled.compiled_graph->node_layout.nodes.size(); ++node) {
        if (compiled.compiled_graph->node_layout.nodes[node].state_size
            == sizeof(ImmutableHistoryReaderStateMirror)) {
            states.push_back(static_cast<ImmutableHistoryReaderStateMirror*>(
                storage.state_ptr(node)));
        }
    }
    ASSERT_EQ(states.size(), 2u);

    compiled.compiled_graph->root_operations.tick_block(
        storage.buffer().data(), &empty_tick_invocation, index(0), 64);
    EXPECT_EQ(states[0]->ticks, 64u);
    EXPECT_EQ(states[1]->ticks, 64u);
    EXPECT_FLOAT_EQ(states[0]->last_output, 63.0f * 0.5f + 0.125f + 1.0f + 1.0f);
    EXPECT_FLOAT_EQ(states[1]->last_output, 63.0f * 0.75f + 0.125f + 1.5f + 2.0f);
}

// A Constant's static_sample_value makes its output immutable storage, so the
// root runs no callback for it. The history reader's input record is therefore
// an immutable constant rather than a direct view of a written ring.
TEST_F(GraphJitRuntimeFixture, ConstantProducerOutputIsImmutableStorage)
{
    bool raised = false;
    std::array arguments{iv::details::configuration_argument(raised)};
    auto const graph = configured_module_graph_with_arguments(
        *revision, graph_jit_constant_history_module_id, arguments);
    expect_sequential_input_access(
        *jit, definitions, graph, 5, 0,
        iv::graph_jit::SampleRealtimePortStateAccess::immutable_constant, 440);
}

} // namespace iv::test::graph_jit_tests
