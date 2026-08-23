#include <intravenous/graph/builder.h>
#include <intravenous/node/block_executor.h>

#include <gtest/gtest.h>

namespace {
struct CountingNode {
    int *ticks = nullptr;

    void tick_block(iv::TickBlockContext<CountingNode> const&) const
    {
        if (ticks) {
            *ticks += 1;
        }
    }
};

struct LifecycleTrackingNode {
    std::string id;
    int* initialized = nullptr;
    int* released = nullptr;

    struct State {
        int initialized_count = 0;
        int released_count = 0;
    };

    std::string identity() const
    {
        return id;
    }

    void initialize(iv::InitializationContext<LifecycleTrackingNode> const& ctx) const
    {
        ctx.state().initialized_count += 1;
        if (initialized) {
            *initialized += 1;
        }
    }

    void release(iv::ReleaseContext<LifecycleTrackingNode> const& ctx) const
    {
        ctx.state().released_count += 1;
        if (released) {
            *released += 1;
        }
    }

    void tick_block(iv::TickBlockContext<LifecycleTrackingNode> const&) const
    {}
};

struct MigratingLifecycleNode {
    std::string id;
    int* initialized = nullptr;
    int* moved = nullptr;

    struct State {
        int value = 0;

        State() = default;
        State(State const&) = delete;
        State(State&&) = delete;
        State& operator=(State const&) = delete;
        State& operator=(State&&) = delete;
    };

    std::string identity() const { return id; }

    void initialize(
        iv::InitializationContext<MigratingLifecycleNode> const& ctx) const
    {
        ctx.state().value = 41;
        if (initialized) ++*initialized;
    }

    void move(iv::MoveContext<MigratingLifecycleNode> const& ctx) const
    {
        ctx.state().value = ctx.previous_state().value + 1;
        if (moved) ++*moved;
    }

    void tick_block(iv::TickBlockContext<MigratingLifecycleNode> const&) const
    {}
};

struct IndependentInitializationNode {
    int* initialized = nullptr;

    void initialize(
        iv::InitializationContext<IndependentInitializationNode> const&) const
    {
        if (initialized) ++*initialized;
    }

    void tick_block(
        iv::TickBlockContext<IndependentInitializationNode> const&) const
    {}
};
}

TEST(BlockNodeExecutor, TicksGraphOncePerCall)
{
    int ticks = 0;
    iv::GraphBuilder builder;
    (void)builder.node<CountingNode>(&ticks);
    builder.outputs();

    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(builder.build_root_node().graph),
        8);

    executor.tick_block(0);
    executor.tick_block(8);

    EXPECT_EQ(executor.block_size(), 8u);
    EXPECT_EQ(ticks, 2);
}

TEST(BlockNodeExecutor, ReloadReplacesTheRootGraph)
{
    int ticks_a = 0;
    iv::GraphBuilder builder_a;
    (void)builder_a.node<CountingNode>(&ticks_a);
    builder_a.outputs();

    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(builder_a.build_root_node().graph),
        8);

    executor.tick_block(0);
    EXPECT_EQ(ticks_a, 1);

    int ticks_b = 0;
    iv::GraphBuilder builder_b;
    (void)builder_b.node<CountingNode>(&ticks_b);
    builder_b.outputs();

    executor.reload(iv::TypeErasedNode(builder_b.build_root_node().graph));
    executor.tick_block(8);

    EXPECT_EQ(ticks_a, 1);
    EXPECT_EQ(ticks_b, 1);
}

TEST(BlockNodeExecutor, ReloadReinitializesAndReleasesLifecycleState)
{
    int initialized_a = 0;
    int released_a = 0;
    iv::GraphBuilder builder_a;
    (void)builder_a.node<LifecycleTrackingNode>("node", &initialized_a, &released_a);
    builder_a.outputs();

    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(builder_a.build_root_node().graph),
        8);

    EXPECT_EQ(initialized_a, 1);
    EXPECT_EQ(released_a, 0);

    int initialized_b = 0;
    int released_b = 0;
    iv::GraphBuilder builder_b;
    (void)builder_b.node<LifecycleTrackingNode>("node", &initialized_b, &released_b);
    builder_b.outputs();

    executor.reload(iv::TypeErasedNode(builder_b.build_root_node().graph));

    EXPECT_EQ(initialized_a, 1);
    EXPECT_EQ(released_a, 1);
    EXPECT_EQ(initialized_b, 1);
    EXPECT_EQ(released_b, 0);
}

TEST(BlockNodeExecutor, PreparedReloadDefersOnlyStateMigrationToCommit)
{
    int old_initialized = 0;
    int old_moved = 0;
    iv::GraphBuilder old_builder;
    (void)old_builder.node<MigratingLifecycleNode>(
        "stable-node", &old_initialized, &old_moved);
    old_builder.outputs();
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(old_builder.build_root_node().graph), 8);

    int new_initialized = 0;
    int new_moved = 0;
    int independent_initialized = 0;
    iv::GraphBuilder new_builder;
    (void)new_builder.node<MigratingLifecycleNode>(
        "stable-node", &new_initialized, &new_moved);
    (void)new_builder.node<IndependentInitializationNode>(
        &independent_initialized);
    new_builder.outputs();

    auto prepared = executor.prepare_reload(
        iv::TypeErasedNode(new_builder.build_root_node().graph));

    EXPECT_EQ(old_initialized, 1);
    EXPECT_EQ(old_moved, 0);
    EXPECT_EQ(new_initialized, 0);
    EXPECT_EQ(new_moved, 0);
    EXPECT_EQ(independent_initialized, 1);

    auto retired = executor.commit_reload(std::move(prepared));

    EXPECT_EQ(new_initialized, 0);
    EXPECT_EQ(new_moved, 1);
    EXPECT_EQ(independent_initialized, 1);
}

TEST(NodeStorageMigration, CrossGenerationTypeNameRequiresExactRewrittenStructure)
{
    MigratingLifecycleNode node{.id = "stable-node"};
    iv::NodeLayoutBuilder old_builder(8);
    iv::do_declare(node, old_builder);
    auto old_layout = std::move(old_builder).build();
    iv::NodeLayoutBuilder new_builder(8);
    iv::do_declare(node, new_builder);
    auto new_layout = std::move(new_builder).build();

    auto const structure = iv::NodeStateStructure{
        .size_bits = sizeof(MigratingLifecycleNode::State) * 8,
        .alignment_bits = alignof(MigratingLifecycleNode::State) * 8,
        .fields = {iv::NodeStateFieldStructure{
            .name = "value",
            .type_name = "int",
            .bit_offset = 0,
            .size_bits = sizeof(int) * 8,
            .alignment_bits = alignof(int) * 8,
        }},
    };
    old_layout.nodes.front().node_state_structure = structure;
    new_layout.nodes.front().node_state_structure = structure;
    static int replacement_generation_type_token = 0;
    new_layout.nodes.front().node_type = &replacement_generation_type_token;

    iv::ResourceContext resources;
    auto old_storage = old_layout.create_storage(resources);
    auto new_storage = new_layout.create_storage(resources);
    EXPECT_TRUE(new_storage.can_move_from(old_storage, 0, 0));

    new_layout.nodes.front().node_state_structure->fields.front().name =
        "renamed_value";
    EXPECT_FALSE(new_storage.can_move_from(old_storage, 0, 0));
}
