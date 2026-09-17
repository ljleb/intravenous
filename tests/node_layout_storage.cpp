#include <intravenous/node/lifecycle.h>
#include <intravenous/node/layout.h>
#include "module_test_utils.h"
#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/node.h>
#include <intravenous/graph/node_wrapper.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    inline iv::ResourceContext make_resources()
    {
        return iv::ResourceContext {};
    }

    struct Producer {
        struct State {
            std::span<float> values;
            int initialized = 0;
            int released = 0;
        };

        void declare(iv::DeclarationContext<Producer> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.values, 4);
            ctx.export_array("values", state.values);
        }

        void initialize(iv::InitializationContext<Producer> const& ctx) const
        {
            auto& state = ctx.state();
            state.initialized += 1;
            for (size_t i = 0; i < state.values.size(); ++i) {
                state.values[i] = float(i + 1);
            }
        }

        void release(iv::ReleaseContext<Producer> const& ctx) const
        {
            auto& state = ctx.state();
            state.released += 1;
        }
    };

    struct Consumer {
        std::vector<int>* order = nullptr;

        struct State {
            std::span<float> imported;
            int initialized = 0;
            int released = 0;
            float observed_sum = 0.0f;
        };

        void declare(iv::DeclarationContext<Consumer> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.import_array("values", state.imported);
        }

        void initialize(iv::InitializationContext<Consumer> const& ctx) const
        {
            auto& state = ctx.state();
            state.initialized += 1;
            state.observed_sum = 0.0f;
            for (float value : state.imported) {
                state.observed_sum += value;
            }
            if (order) {
                order->push_back(2);
            }
        }

        void release(iv::ReleaseContext<Consumer> const& ctx) const
        {
            auto& state = ctx.state();
            state.released += 1;
            if (order) {
                order->push_back(3);
            }
        }
    };

    struct LocalOnly {
        struct State {
            std::span<int> scratch;
            std::uint32_t guard = 0;
        };

        void declare(iv::DeclarationContext<LocalOnly> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.scratch, 3);
        }

        void initialize(iv::InitializationContext<LocalOnly> const& ctx) const
        {
            auto& state = ctx.state();
            state.guard = 0x1234abcd;
            for (size_t i = 0; i < state.scratch.size(); ++i) {
                state.scratch[i] = int(i) * 10;
            }
        }
    };

    struct Movable {
        struct State {
            std::span<int> scratch;
            int initialized = 0;
            int moved = 0;
            int released = 0;
        };

        void declare(iv::DeclarationContext<Movable> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.scratch, 2);
        }

        void initialize(iv::InitializationContext<Movable> const& ctx) const
        {
            auto& state = ctx.state();
            state.initialized += 1;
            state.scratch[0] = 7;
            state.scratch[1] = 9;
        }

        void move(iv::MoveContext<Movable> const& ctx) const
        {
            auto& state = ctx.state();
            auto const& previous = ctx.previous_state();
            state.moved = previous.moved + 1;
            state.initialized = previous.initialized;
            state.released = previous.released;
            state.scratch[0] = previous.scratch[0];
            state.scratch[1] = previous.scratch[1];
        }

        void release(iv::ReleaseContext<Movable> const& ctx) const
        {
            auto& state = ctx.state();
            state.released += 1;
        }
    };

    struct AutoMovable {
        struct State {
            std::span<int> scratch;
            std::unique_ptr<int> value;
            int initialized = 0;
            int released = 0;
        };

        void declare(iv::DeclarationContext<AutoMovable> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.scratch, 2);
        }

        void initialize(iv::InitializationContext<AutoMovable> const& ctx) const
        {
            auto& state = ctx.state();
            state.initialized += 1;
            if (!state.value) {
                state.value = std::make_unique<int>(41);
            }
            state.scratch[0] = *state.value;
            state.scratch[1] = state.initialized;
        }

        void release(iv::ReleaseContext<AutoMovable> const& ctx) const
        {
            auto& state = ctx.state();
            state.released += 1;
        }
    };

    struct NestedLeaf {
        struct State {
            int marker = 17;
        };

        struct CompiledState {
            int marker = 23;
        };
    };

    struct IdentifiedMovable {
        std::string id;

        struct State {
            std::span<int> scratch;
            int initialized = 0;
            int moved = 0;
            int released = 0;
        };

        void declare(iv::DeclarationContext<IdentifiedMovable> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.scratch, 2);
        }

        std::string identity() const
        {
            return id;
        }

        void initialize(iv::InitializationContext<IdentifiedMovable> const& ctx) const
        {
            auto& state = ctx.state();
            state.initialized += 1;
            state.scratch[0] = 11;
            state.scratch[1] = 13;
        }

        void move(iv::MoveContext<IdentifiedMovable> const& ctx) const
        {
            auto& state = ctx.state();
            auto const& previous = ctx.previous_state();
            state.initialized = previous.initialized;
            state.moved = previous.moved + 1;
            state.released = previous.released;
            state.scratch[0] = previous.scratch[0];
            state.scratch[1] = previous.scratch[1];
        }

        void release(iv::ReleaseContext<IdentifiedMovable> const& ctx) const
        {
            auto& state = ctx.state();
            state.released += 1;
        }
    };

    struct IdentifiedAutoMovable {
        std::string id;

        struct State {
            std::unique_ptr<int> value;
            int initialized = 0;
            int released = 0;
        };

        std::string identity() const
        {
            return id;
        }

        void initialize(iv::InitializationContext<IdentifiedAutoMovable> const& ctx) const
        {
            auto& state = ctx.state();
            state.initialized += 1;
            if (!state.value) {
                state.value = std::make_unique<int>(29);
            }
        }

        void release(iv::ReleaseContext<IdentifiedAutoMovable> const& ctx) const
        {
            auto& state = ctx.state();
            state.released += 1;
        }
    };

    struct DifferentTypeSameIdentity {
        std::string id;

        struct State {
            int initialized = 0;
            int released = 0;
        };

        std::string identity() const
        {
            return id;
        }

        void initialize(iv::InitializationContext<DifferentTypeSameIdentity> const& ctx) const
        {
            ctx.state().initialized += 1;
        }

        void release(iv::ReleaseContext<DifferentTypeSameIdentity> const& ctx) const
        {
            ctx.state().released += 1;
        }
    };

    struct NestedParent {
        struct State {
            std::span<std::span<std::byte>> nested;
            std::span<std::span<std::byte>> nested_compiled;
        };

        void declare(iv::DeclarationContext<NestedParent> const& ctx) const
        {
            auto const& state = ctx.state();
            NestedLeaf a;
            NestedLeaf b;
            do_declare(a, ctx);
            do_declare(b, ctx);
            ctx.nested_node_states(state.nested);
            ctx.nested_node_compiled_states(state.nested_compiled);
        }
    };

    struct CompiledLifecycleNode {
        std::string id;

        struct CompiledState {
            static inline int live_instances = 0;

            int initialized = 0;
            int moved = 0;
            int released = 0;
            int value = 0;

            CompiledState()
            {
                ++live_instances;
            }

            ~CompiledState()
            {
                --live_instances;
            }
        };

        std::string identity() const
        {
            return id;
        }

        void initialize(
            iv::InitializationContext<CompiledLifecycleNode> const& ctx) const
        {
            auto& state = ctx.compiled_state();
            ++state.initialized;
            state.value = 17;
        }

        void move(iv::MoveContext<CompiledLifecycleNode> const& ctx) const
        {
            auto& state = ctx.compiled_state();
            auto const& previous = ctx.previous_compiled_state();
            state.initialized = previous.initialized;
            state.moved = previous.moved + 1;
            state.released = previous.released;
            state.value = previous.value;
        }

        void release(
            iv::ReleaseContext<CompiledLifecycleNode> const& ctx) const
        {
            ++ctx.compiled_state().released;
        }
    };

    struct StatefulTickingNode {
        struct State {
            int initialized = 0;
            int ticked = 0;
        };

        struct CompiledState {
            int initialized = 0;
            int ticked = 0;
        };

        void initialize(iv::InitializationContext<StatefulTickingNode> const& ctx) const
        {
            ctx.state().initialized += 1;
            ctx.compiled_state().initialized += 1;
        }

        void tick_block(iv::TickBlockContext<StatefulTickingNode> const& ctx) const
        {
            ctx.state().ticked += static_cast<int>(ctx.block_size);
            ctx.compiled_state().ticked += static_cast<int>(ctx.block_size);
        }
    };

    struct CompiledStorageProducer {
        struct CompiledState {
            std::span<int> values;
        };

        void declare(iv::DeclarationContext<CompiledStorageProducer> const& ctx) const
        {
            auto const& state = ctx.compiled_state();
            ctx.local_array(state.values, 3);
            ctx.export_array("compiled-values", state.values);
        }

        void initialize(
            iv::InitializationContext<CompiledStorageProducer> const& ctx) const
        {
            auto& state = ctx.compiled_state();
            state.values[0] = 5;
            state.values[1] = 7;
            state.values[2] = 11;
        }
    };

    struct CompiledStorageConsumer {
        struct CompiledState {
            std::span<int> imported;
            int observed_sum = 0;
        };

        void declare(iv::DeclarationContext<CompiledStorageConsumer> const& ctx) const
        {
            auto const& state = ctx.compiled_state();
            ctx.import_array("compiled-values", state.imported);
        }

        void initialize(
            iv::InitializationContext<CompiledStorageConsumer> const& ctx) const
        {
            auto& state = ctx.compiled_state();
            for (auto const value : state.imported) {
                state.observed_sum += value;
            }
        }
    };
}

int main()
{
    iv::test::install_crash_handlers();

    {
        iv::NodeLayoutBuilder builder(8);
        Producer producer;
        std::vector<int> order;
        Consumer consumer { &order };

        iv::do_declare(producer, builder);
        iv::test::require(builder.has_export_array<float>("values"), "producer export should be visible during declaration");
        iv::do_declare(consumer, builder);
        iv::test::require(builder.has_import_array<float>("values"), "consumer import should be visible during declaration");

        iv::NodeLayout layout = std::move(builder).build();
        iv::test::require(layout.nodes.size() == 2, "layout should contain two nodes");
        iv::test::require(layout.initialize_order.size() == 2, "layout should solve initialize order");
        iv::test::require(layout.initialize_order[0] == 0, "producer should initialize before consumer");
        iv::test::require(layout.initialize_order[1] == 1, "consumer should initialize after producer");

        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        storage.initialize();

        auto& producer_state = *static_cast<Producer::State*>(storage.state_ptr(0));
        auto& consumer_state = *static_cast<Consumer::State*>(storage.state_ptr(1));

        iv::test::require(producer_state.initialized == 1, "producer initialize should run once");
        iv::test::require(consumer_state.initialized == 1, "consumer initialize should run once");
        iv::test::require(producer_state.values.size() == 4, "producer local array should be patched");
        iv::test::require(consumer_state.imported.size() == 4, "consumer import span should be patched");
        iv::test::require(consumer_state.imported.data() == producer_state.values.data(), "import should resolve to producer export");
        iv::test::require(consumer_state.observed_sum == 10.0f, "consumer should observe producer-initialized values");
        iv::test::require(order.size() == 1 && order[0] == 2, "consumer initialize order marker should be recorded");

        storage.release();
        iv::test::require(producer_state.released == 1, "producer release should run once");
        iv::test::require(consumer_state.released == 1, "consumer release should run once");
        iv::test::require(order.size() == 2 && order[1] == 3, "consumer release marker should be recorded");
    }

    {
        iv::NodeLayoutBuilder builder(4);
        LocalOnly node;
        iv::do_declare(node, builder);

        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        storage.initialize();

        auto& state = *static_cast<LocalOnly::State*>(storage.state_ptr(0));
        iv::test::require(state.guard == 0x1234abcd, "local-only node should initialize state");
        iv::test::require(state.scratch.size() == 3, "local array should be patched for local-only node");
        iv::test::require(state.scratch[0] == 0, "local scratch[0] should match initialization");
        iv::test::require(state.scratch[1] == 10, "local scratch[1] should match initialization");
        iv::test::require(state.scratch[2] == 20, "local scratch[2] should match initialization");
    }

    {
        iv::NodeLayoutBuilder builder(4);
        Movable node;
        iv::do_declare(node, builder);

        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();

        iv::NodeStorage original = layout.create_storage(resources);
        original.initialize();
        auto& original_state = *static_cast<Movable::State*>(original.state_ptr(0));
        iv::test::require(original_state.initialized == 1, "movable node should initialize once");
        iv::test::require(original_state.moved == 0, "movable node should not move on first initialization");

        iv::NodeStorage reloaded = layout.create_storage(resources);
        reloaded.initialize(&original);
        auto& reloaded_state = *static_cast<Movable::State*>(reloaded.state_ptr(0));

        iv::test::require(original.initialized_nodes.empty(), "reload should transfer old release ownership");
        iv::test::require(reloaded_state.initialized == 1, "anonymous node should initialize fresh on reload");
        iv::test::require(reloaded_state.moved == 0, "anonymous node should not move without a migration identity");
        iv::test::require(reloaded_state.scratch.size() == 2, "reloaded node local span should be patched");
        iv::test::require(reloaded_state.scratch[0] == 7, "fresh initialize should repopulate scratch[0]");
        iv::test::require(reloaded_state.scratch[1] == 9, "fresh initialize should repopulate scratch[1]");
    }

    {
        iv::NodeLayoutBuilder builder(4);
        AutoMovable node;
        iv::do_declare(node, builder);

        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();

        iv::NodeStorage original = layout.create_storage(resources);
        original.initialize();
        auto& original_state = *static_cast<AutoMovable::State*>(original.state_ptr(0));
        iv::test::require(original_state.value != nullptr, "auto-movable node should own a value after initialize");
        iv::test::require(*original_state.value == 41, "auto-movable node should initialize owned value");

        iv::NodeStorage reloaded = layout.create_storage(resources);
        reloaded.initialize(&original);
        auto& reloaded_state = *static_cast<AutoMovable::State*>(reloaded.state_ptr(0));

        iv::test::require(original.initialized_nodes.empty(), "reload should transfer old release ownership");
        iv::test::require(original_state.value != nullptr, "anonymous node should retain previous ownership when it is not moved");
        iv::test::require(reloaded_state.value != nullptr, "reloaded anonymous node should initialize its own value");
        iv::test::require(*reloaded_state.value == 41, "fresh initialize should produce the default owned value");
        iv::test::require(reloaded_state.initialized == 1, "anonymous auto-movable node should initialize fresh on reload");
        iv::test::require(original_state.released == 1, "release should run on the previous anonymous state");
        iv::test::require(reloaded_state.released == 0, "fresh anonymous reload state should not inherit prior release count");
        iv::test::require(reloaded_state.scratch[0] == 41, "initialize should repopulate scratch[0] after repatching");
        iv::test::require(reloaded_state.scratch[1] == 1, "fresh initialize should observe a fresh initialized count");
    }

    {
        iv::NodeLayoutBuilder original_builder(4);
        IdentifiedMovable original_a { .id = "moved-later" };
        IdentifiedMovable original_b { .id = "kept" };
        iv::do_declare(original_a, original_builder);
        iv::do_declare(original_b, original_builder);

        iv::NodeLayout original_layout = std::move(original_builder).build();
        auto resources = make_resources();

        iv::NodeStorage original = original_layout.create_storage(resources);
        original.initialize();

        auto& original_a_state = *static_cast<IdentifiedMovable::State*>(original.state_ptr(0));
        auto& original_b_state = *static_cast<IdentifiedMovable::State*>(original.state_ptr(1));
        original_a_state.scratch[0] = 101;
        original_a_state.scratch[1] = 103;
        original_b_state.scratch[0] = 107;
        original_b_state.scratch[1] = 109;

        iv::NodeLayoutBuilder reloaded_builder(4);
        LocalOnly inserted;
        IdentifiedMovable reloaded_b { .id = "kept" };
        IdentifiedMovable reloaded_a { .id = "moved-later" };
        iv::do_declare(inserted, reloaded_builder);
        iv::do_declare(reloaded_b, reloaded_builder);
        iv::do_declare(reloaded_a, reloaded_builder);

        iv::NodeLayout reloaded_layout = std::move(reloaded_builder).build();
        iv::NodeStorage reloaded = reloaded_layout.create_storage(resources);
        reloaded.initialize(&original);

        auto& inserted_state = *static_cast<LocalOnly::State*>(reloaded.state_ptr(0));
        auto& reloaded_b_state = *static_cast<IdentifiedMovable::State*>(reloaded.state_ptr(1));
        auto& reloaded_a_state = *static_cast<IdentifiedMovable::State*>(reloaded.state_ptr(2));

        iv::test::require(inserted_state.guard == 0x1234abcd, "new unmatched node should initialize normally");
        iv::test::require(reloaded_b_state.moved == 1, "identity match should move even when node indices shift");
        iv::test::require(reloaded_b_state.scratch[0] == 107, "identity move should preserve kept node scratch[0]");
        iv::test::require(reloaded_b_state.scratch[1] == 109, "identity move should preserve kept node scratch[1]");
        iv::test::require(reloaded_a_state.moved == 1, "identity move should preserve later node after insertion");
        iv::test::require(reloaded_a_state.scratch[0] == 101, "identity move should preserve moved-later scratch[0]");
        iv::test::require(reloaded_a_state.scratch[1] == 103, "identity move should preserve moved-later scratch[1]");
    }

    {
        iv::NodeLayoutBuilder original_builder(4);
        IdentifiedAutoMovable original_node { .id = "same-name" };
        iv::do_declare(original_node, original_builder);

        iv::NodeLayout original_layout = std::move(original_builder).build();
        auto resources = make_resources();
        iv::NodeStorage original = original_layout.create_storage(resources);
        original.initialize();

        auto& original_state = *static_cast<IdentifiedAutoMovable::State*>(original.state_ptr(0));
        iv::test::require(original_state.value != nullptr, "original identified auto-movable should initialize owned value");
        *original_state.value = 211;

        iv::NodeLayoutBuilder reloaded_builder(4);
        DifferentTypeSameIdentity changed_node { .id = "same-name" };
        iv::do_declare(changed_node, reloaded_builder);

        iv::NodeLayout reloaded_layout = std::move(reloaded_builder).build();
        iv::NodeStorage reloaded = reloaded_layout.create_storage(resources);
        reloaded.initialize(&original);

        auto& reloaded_state = *static_cast<DifferentTypeSameIdentity::State*>(reloaded.state_ptr(0));
        iv::test::require(reloaded_state.initialized == 1, "same identity with different type should initialize new node");
        iv::test::require(original.initialized_nodes.empty(), "previous storage should release unmatched old node ownership");
    }

    {
        iv::NodeLayoutBuilder builder(8);
        CompiledStorageProducer producer;
        CompiledStorageConsumer consumer;
        iv::do_declare(producer, builder);
        iv::do_declare(consumer, builder);

        auto layout = std::move(builder).build();
        auto resources = make_resources();
        auto storage = layout.create_storage(resources);
        storage.initialize();

        auto& producer_state = *static_cast<CompiledStorageProducer::CompiledState*>(
            storage.compiled_state_ptr(0));
        auto& consumer_state = *static_cast<CompiledStorageConsumer::CompiledState*>(
            storage.compiled_state_ptr(1));
        iv::test::require(
            producer_state.values.size() == 3,
            "local_array declared from CompiledState should be patched");
        iv::test::require(
            consumer_state.imported.data() == producer_state.values.data(),
            "CompiledState import/export bindings should resolve through CompiledState");
        auto const exported =
            storage.resolve_exported_array_storage<int>("compiled-values");
        iv::test::require(
            exported.data() == producer_state.values.data() && exported.size() == 3,
            "host export resolution should read CompiledState span fields");
        iv::test::require(
            consumer_state.observed_sum == 23,
            "CompiledState imports should be available during initialize");
    }

    {
        iv::NodeLayoutBuilder builder(4);
        auto const leading = builder.declare_raw_region(13, 32);
        LocalOnly node;
        iv::do_declare(node, builder);
        auto const trailing = builder.declare_raw_region(7, 8);

        iv::NodeLayout layout = std::move(builder).build();
        iv::test::require(leading.valid(), "raw region handle should be valid");
        iv::test::require(trailing.valid(), "second raw region handle should be valid");
        iv::test::require(
            layout.regions[leading.index].kind == iv::NodeLayout::Region::Kind::raw,
            "raw region should remain part of the canonical node layout");
        iv::test::require(
            layout.regions[leading.index].owner_node == iv::NodeLayout::no_owner_node,
            "raw region should not require a node owner");
        iv::test::require(
            layout.regions[leading.index].storage_offset % 32 == 0,
            "raw region storage should honor requested alignment");
        iv::test::require(
            layout.regions[trailing.index].storage_offset >
                layout.regions[leading.index].storage_offset,
            "raw regions should participate in declaration-order packing");

        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        auto leading_bytes = storage.region_bytes(leading);
        auto trailing_bytes = storage.region_bytes(trailing);
        iv::test::require(leading_bytes.size() == 13, "raw region size should survive layout");
        iv::test::require(trailing_bytes.size() == 7, "second raw region size should survive layout");
        iv::test::require(
            reinterpret_cast<std::uintptr_t>(leading_bytes.data()) % 32 == 0,
            "raw region address should satisfy requested alignment");
        leading_bytes[3] = std::byte { 0x5a };
        iv::test::require(
            storage.buffer()[layout.regions[leading.index].storage_offset + 3] ==
                std::byte { 0x5a },
            "raw region should be backed by the same NodeStorage allocation");
    }

    {
        bool rejected = false;
        try {
            iv::NodeLayoutBuilder builder(4);
            (void)builder.declare_raw_region(8, 3);
        } catch (std::invalid_argument const&) {
            rejected = true;
        }
        iv::test::require(
            rejected,
            "raw region declarations should reject non-power-of-two alignment");
    }

    {
        iv::test::require(
            CompiledLifecycleNode::CompiledState::live_instances == 0,
            "compiled-state lifecycle test should start without live objects");

        iv::NodeLayoutBuilder builder(4);
        CompiledLifecycleNode node { .id = "compiled-state" };
        iv::do_declare(node, builder);
        iv::NodeLayout layout = std::move(builder).build();
        iv::test::require(
            layout.nodes.front().compiled_state_structure.has_value(),
            "compiled-state layout should carry ABI metadata");
        layout.nodes.front().compiled_state_structure->type_identity = {
            .nominal_id = "test.CompiledLifecycleNode.CompiledState",
            .definition_fingerprint = "v1",
            .display_name = "CompiledLifecycleNode::CompiledState",
        };
        iv::NodeLayout reloaded_layout = layout;
        static int reloaded_node_type_token = 0;
        reloaded_layout.nodes.front().node_type = &reloaded_node_type_token;

        iv::test::require(layout.nodes.size() == 1, "compiled-state layout should contain its node");
        auto const& record = layout.nodes.front();
        iv::test::require(record.state_size == 0, "test node should have no sequential State");
        iv::test::require(
            record.compiled_state_size == sizeof(CompiledLifecycleNode::CompiledState),
            "layout should record CompiledState size");
        iv::test::require(
            record.compiled_state_alignment == alignof(CompiledLifecycleNode::CompiledState),
            "layout should record CompiledState alignment");
        iv::test::require(
            record.compiled_state_offset >= 0,
            "layout should assign CompiledState storage");
        iv::test::require(
            static_cast<size_t>(record.compiled_state_offset) %
                    alignof(CompiledLifecycleNode::CompiledState) ==
                0,
            "CompiledState offset should satisfy its alignment");

        auto resources = make_resources();
        {
            iv::NodeStorage original = layout.create_storage(resources);
            original.initialize();
            auto& original_compiled =
                *static_cast<CompiledLifecycleNode::CompiledState*>(
                    original.compiled_state_ptr(0));
            iv::test::require(
                original_compiled.initialized == 1,
                "initialize should receive the default-constructed CompiledState");
            original_compiled.value = 91;

            iv::NodeStorage reloaded = reloaded_layout.create_storage(resources);
            iv::test::require(
                reloaded.can_move_from(original, 0, 0),
                "same reflected CompiledState definition should remain movable across package generations");
            auto migration = reloaded.prepare_migration_from(original);
            migration.commit();
            auto& reloaded_compiled =
                *static_cast<CompiledLifecycleNode::CompiledState*>(
                    reloaded.compiled_state_ptr(0));
            iv::test::require(
                reloaded_compiled.initialized == 1,
                "move should preserve CompiledState initialization data");
            iv::test::require(
                reloaded_compiled.moved == 1,
                "move should receive current and previous CompiledState objects");
            iv::test::require(
                reloaded_compiled.value == 91,
                "move should be able to transfer CompiledState contents");
            iv::test::require(
                original.initialized_nodes.empty(),
                "successful compiled-state migration should transfer release ownership");

            reloaded.release();
            iv::test::require(
                reloaded_compiled.released == 1,
                "release should receive the same mutable CompiledState object");
        }

        iv::test::require(
            CompiledLifecycleNode::CompiledState::live_instances == 0,
            "NodeStorage destruction should destroy every constructed CompiledState");
    }

    {
        iv::NodeLayoutBuilder builder(4);
        NestedParent node;
        iv::do_declare(node, builder);

        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        storage.initialize();

        auto& state = *static_cast<NestedParent::State*>(storage.state_ptr(0));
        iv::test::require(state.nested.size() == 2, "nested_node_states should record directly declared child nodes");
        iv::test::require(state.nested_compiled.size() == 2, "nested_node_compiled_states should record the same directly declared child nodes");
        iv::test::require(state.nested[0].data() != nullptr, "first nested node state pointer should be patched");
        iv::test::require(state.nested[1].data() != nullptr, "second nested node state pointer should be patched");
        iv::test::require(state.nested_compiled[0].size() == sizeof(NestedLeaf::CompiledState), "first nested compiled-state span should have the exact CompiledState size");
        iv::test::require(state.nested_compiled[1].size() == sizeof(NestedLeaf::CompiledState), "second nested compiled-state span should have the exact CompiledState size");
        auto& first = *reinterpret_cast<NestedLeaf::State*>(state.nested[0].data());
        auto& second = *reinterpret_cast<NestedLeaf::State*>(state.nested[1].data());
        auto& first_compiled = *reinterpret_cast<NestedLeaf::CompiledState*>(state.nested_compiled[0].data());
        auto& second_compiled = *reinterpret_cast<NestedLeaf::CompiledState*>(state.nested_compiled[1].data());
        iv::test::require(first.marker == 17, "first nested node state should be addressable");
        iv::test::require(second.marker == 17, "second nested node state should be addressable");
        iv::test::require(first_compiled.marker == 23, "first nested CompiledState should be addressable");
        iv::test::require(second_compiled.marker == 23, "second nested CompiledState should be addressable");
    }

    {
        iv::NodeLayoutBuilder builder(8);
        iv::TypeErasedNode node = StatefulTickingNode {};
        iv::do_declare(node, builder);

        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        storage.initialize();

        auto& erased_state = *static_cast<iv::TypeErasedNode::State*>(storage.state_ptr(0));
        iv::test::require(erased_state.nested_node_states.size() == 1, "type-erased node should record exactly one nested child");
        iv::test::require(erased_state.nested_node_compiled_states.size() == 1, "type-erased node should record exactly one nested compiled child state");
        iv::test::require(erased_state.nested_node_states[0].data() != nullptr, "type-erased nested child state pointer should be patched");
        iv::test::require(erased_state.nested_node_compiled_states[0].size() == sizeof(StatefulTickingNode::CompiledState), "type-erased nested CompiledState should be patched");

        node.tick_block({
            iv::TickContext<iv::TypeErasedNode> {
                .inputs = {},
                .outputs = {},
                .event_inputs = {},
                .event_outputs = {},
                .buffer = storage.buffer(),
            },
            0,
            8,
        });

        auto& nested_state = *reinterpret_cast<StatefulTickingNode::State*>(erased_state.nested_node_states[0].data());
        auto& nested_compiled_state = *reinterpret_cast<StatefulTickingNode::CompiledState*>(erased_state.nested_node_compiled_states[0].data());
        iv::test::require(nested_state.initialized == 1, "type-erased nested child should initialize once");
        iv::test::require(nested_state.ticked == 8, "type-erased nested child should tick through nested state");
        iv::test::require(nested_compiled_state.initialized == 1, "type-erased nested child CompiledState should initialize once");
        iv::test::require(nested_compiled_state.ticked == 8, "type-erased nested child should tick through nested CompiledState");
    }

    {
        StatefulTickingNode concrete;
        iv::WeakTypeErasedNode node = concrete;
        iv::NodeLayoutBuilder builder(8);
        iv::do_declare(node, builder);

        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        storage.initialize();

        auto& erased_state = *static_cast<iv::WeakTypeErasedNode::State*>(
            storage.state_ptr(0));
        iv::test::require(
            erased_state.nested_node_states.size() == 1,
            "weak type-erased node should record exactly one nested child");
        iv::test::require(
            erased_state.nested_node_compiled_states.size() == 1,
            "weak type-erased node should record exactly one nested compiled child state");

        node.tick_block({
            iv::TickContext<iv::WeakTypeErasedNode> {
                .buffer = storage.buffer(),
            },
            0,
            8,
        });

        auto& nested_state = *reinterpret_cast<StatefulTickingNode::State*>(
            erased_state.nested_node_states[0].data());
        auto& nested_compiled_state =
            *reinterpret_cast<StatefulTickingNode::CompiledState*>(
                erased_state.nested_node_compiled_states[0].data());
        iv::test::require(
            nested_state.ticked == 8,
            "weak type-erased child should tick through nested State");
        iv::test::require(
            nested_compiled_state.ticked == 8,
            "weak type-erased child should tick through nested CompiledState");
    }

    {
        const iv::GraphNodeWrapper wrapper(
            iv::details::reflect_node(StatefulTickingNode {}),
            std::vector<iv::InputPortPlan>{},
            "standalone",
            std::vector<iv::SampleOutputBinding>{}
        );

        iv::NodeLayoutBuilder builder(8);
        iv::do_declare(wrapper, builder);
        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        storage.initialize();

        auto& wrapper_state = *static_cast<iv::GraphNodeWrapper::State*>(storage.state_ptr(0));
        iv::test::require(wrapper_state.nested_node_states.size() == 1, "standalone wrapper without inputs should expose one nested executable child");
        iv::test::require(wrapper_state.nested_node_compiled_states.size() == 1, "standalone wrapper should expose one nested executable child CompiledState");
        iv::test::require(wrapper_state.nested_node_states[0].data() != nullptr, "standalone wrapper nested executable child state should be patched");
        iv::test::require(wrapper_state.nested_node_compiled_states[0].size() == sizeof(StatefulTickingNode::CompiledState), "standalone wrapper nested executable child CompiledState should be patched");

        wrapper.tick({
            iv::TickContext<iv::GraphNodeWrapper> {
                .buffer = storage.buffer(),
            },
            0,
            8,
        });

        auto& nested_state = *reinterpret_cast<StatefulTickingNode::State*>(
            wrapper_state.nested_node_states[0].data());
        auto& nested_compiled_state =
            *reinterpret_cast<StatefulTickingNode::CompiledState*>(
                wrapper_state.nested_node_compiled_states[0].data());
        iv::test::require(nested_state.ticked == 8, "graph wrapper should tick nested State");
        iv::test::require(nested_compiled_state.ticked == 8, "graph wrapper should tick nested CompiledState");
    }

    return 0;
}
