#include <intravenous/node/lifecycle.h>
#include <intravenous/node/layout.h>
#include "module_test_utils.h"
#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/node.h>
#include <intravenous/graph/node_wrapper.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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

        struct IndexedState {
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
            std::span<std::span<std::byte>> nested_indexed;
        };

        void declare(iv::DeclarationContext<NestedParent> const& ctx) const
        {
            auto const& state = ctx.state();
            NestedLeaf a;
            NestedLeaf b;
            do_declare(a, ctx);
            do_declare(b, ctx);
            ctx.nested_node_states(state.nested);
            ctx.nested_node_indexed_states(state.nested_indexed);
        }
    };

    struct IndexedLifecycleNode {
        std::string id;

        struct IndexedState {
            static inline int live_instances = 0;

            int initialized = 0;
            int moved = 0;
            int released = 0;
            int value = 0;

            IndexedState()
            {
                ++live_instances;
            }

            ~IndexedState()
            {
                --live_instances;
            }
        };

        std::string identity() const
        {
            return id;
        }

        void initialize(
            iv::InitializationContext<IndexedLifecycleNode> const& ctx) const
        {
            auto& state = ctx.indexed_state();
            ++state.initialized;
            state.value = 17;
        }

        void move(iv::MoveContext<IndexedLifecycleNode> const& ctx) const
        {
            auto& state = ctx.indexed_state();
            auto const& previous = ctx.previous_indexed_state();
            state.initialized = previous.initialized;
            state.moved = previous.moved + 1;
            state.released = previous.released;
            state.value = previous.value;
        }

        void release(
            iv::ReleaseContext<IndexedLifecycleNode> const& ctx) const
        {
            ++ctx.indexed_state().released;
        }
    };

    struct StatefulTickingNode {
        struct State {
            int initialized = 0;
            int ticked = 0;
        };

        struct IndexedState {
            int initialized = 0;
            int ticked = 0;
        };

        void initialize(iv::InitializationContext<StatefulTickingNode> const& ctx) const
        {
            ctx.state().initialized += 1;
            ctx.indexed_state().initialized += 1;
        }

        void tick_block(iv::TickBlockContext<StatefulTickingNode> const& ctx) const
        {
            ctx.state().ticked += static_cast<int>(ctx.block_size);
        }
    };

    struct IndexedStorageProducer {
        struct IndexedState {
            std::span<int> values;
        };

        void declare(iv::DeclarationContext<IndexedStorageProducer> const& ctx) const
        {
            auto const& state = ctx.indexed_state();
            ctx.local_array(state.values, 3);
            ctx.export_array("indexed-values", state.values);
        }

        void initialize(
            iv::InitializationContext<IndexedStorageProducer> const& ctx) const
        {
            auto& state = ctx.indexed_state();
            state.values[0] = 5;
            state.values[1] = 7;
            state.values[2] = 11;
        }
    };

    struct IndexedStorageConsumer {
        struct IndexedState {
            std::span<int> imported;
            int observed_sum = 0;
        };

        void declare(iv::DeclarationContext<IndexedStorageConsumer> const& ctx) const
        {
            auto const& state = ctx.indexed_state();
            ctx.import_array("indexed-values", state.imported);
        }

        void initialize(
            iv::InitializationContext<IndexedStorageConsumer> const& ctx) const
        {
            auto& state = ctx.indexed_state();
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
        // Compiler-owned persistent raw regions migrate by stable identity, not
        // by incidental region order/offset. Transient raw storage stays fresh.
        iv::NodeLayoutBuilder previous_builder(8);
        auto previous_persistent = previous_builder.declare_raw_region(
            16, 8, "graphjit.test.persistent");
        auto previous_transient = previous_builder.declare_raw_region(16, 8);
        auto previous_layout = std::move(previous_builder).build();
        auto resources = make_resources();
        auto previous = previous_layout.create_storage(resources);
        previous.initialize();
        std::memset(
            previous.region_bytes(previous_persistent).data(),
            0x5a,
            previous.region_bytes(previous_persistent).size());
        std::memset(
            previous.region_bytes(previous_transient).data(),
            0x33,
            previous.region_bytes(previous_transient).size());

        iv::NodeLayoutBuilder current_builder(8);
        auto leading_transient = current_builder.declare_raw_region(7, 1);
        auto current_persistent = current_builder.declare_raw_region(
            16, 8, "graphjit.test.persistent");
        auto current_transient = current_builder.declare_raw_region(16, 8);
        auto current_layout = std::move(current_builder).build();
        auto current = current_layout.create_storage(resources);
        current.initialize(&previous);

        for (auto const byte : current.region_bytes(current_persistent)) {
            iv::test::require(
                byte == std::byte{0x5a},
                "stable persistent raw region should migrate by identity");
        }
        for (auto const byte : current.region_bytes(current_transient)) {
            iv::test::require(
                byte == std::byte{0},
                "unidentified transient raw region must not migrate");
        }
        for (auto const byte : current.region_bytes(leading_transient)) {
            iv::test::require(
                byte == std::byte{0},
                "new raw storage should remain zero initialized");
        }
    }

    {
        // Compiler-owned raw-region initialization runs as part of
        // NodeStorage::initialize(). Exact-shape migration wins over a new
        // initializer so persistent graph state is never reset during a
        // generation swap; shape changes run the new initializer.
        auto fill_raw_region = +[](
            std::span<std::byte> storage,
            std::span<std::byte const> payload) {
            iv::test::require(
                payload.size() == 1,
                "raw-region initializer should receive its declared payload");
            std::fill(storage.begin(), storage.end(), payload.front());
        };
        auto payload = [](std::byte value) {
            return std::vector<std::byte>{value};
        };

        iv::NodeLayoutBuilder previous_builder(8);
        auto previous_region = previous_builder.declare_raw_region(
            8,
            4,
            "graphjit.test.initialized",
            fill_raw_region,
            payload(std::byte{0x11}));
        auto previous_layout = std::move(previous_builder).build();
        auto resources = make_resources();
        auto previous = previous_layout.create_storage(resources);
        previous.initialize();
        for (auto const byte : previous.region_bytes(previous_region)) {
            iv::test::require(
                byte == std::byte{0x11},
                "fresh raw storage should run its initialize callback");
        }
        std::fill(
            previous.region_bytes(previous_region).begin(),
            previous.region_bytes(previous_region).end(),
            std::byte{0x5a});

        iv::NodeLayoutBuilder current_builder(8);
        auto current_region = current_builder.declare_raw_region(
            8,
            4,
            "graphjit.test.initialized",
            fill_raw_region,
            payload(std::byte{0x22}));
        auto current_layout = std::move(current_builder).build();
        auto current = current_layout.create_storage(resources);
        current.initialize(&previous);
        for (auto const byte : current.region_bytes(current_region)) {
            iv::test::require(
                byte == std::byte{0x5a},
                "migrated raw storage must not rerun its initialize callback");
        }

        iv::NodeLayoutBuilder reshaped_builder(8);
        auto reshaped_region = reshaped_builder.declare_raw_region(
            12,
            4,
            "graphjit.test.initialized",
            fill_raw_region,
            payload(std::byte{0x33}));
        auto reshaped_layout = std::move(reshaped_builder).build();
        auto reshaped = reshaped_layout.create_storage(resources);
        reshaped.initialize(&current);
        for (auto const byte : reshaped.region_bytes(reshaped_region)) {
            iv::test::require(
                byte == std::byte{0x33},
                "shape-mismatched raw storage should run its initialize callback");
        }
    }

    {
        // Exact shape is part of the migration contract. Reusing an identity
        // with a different byte extent must conservatively start from zero.
        iv::NodeLayoutBuilder previous_builder(8);
        auto previous_region = previous_builder.declare_raw_region(
            16, 8, "graphjit.test.shape");
        auto previous_layout = std::move(previous_builder).build();
        auto resources = make_resources();
        auto previous = previous_layout.create_storage(resources);
        previous.initialize();
        std::memset(
            previous.region_bytes(previous_region).data(),
            0x6b,
            previous.region_bytes(previous_region).size());

        iv::NodeLayoutBuilder current_builder(8);
        auto current_region = current_builder.declare_raw_region(
            24, 8, "graphjit.test.shape");
        auto current_layout = std::move(current_builder).build();
        auto current = current_layout.create_storage(resources);
        current.initialize(&previous);
        for (auto const byte : current.region_bytes(current_region)) {
            iv::test::require(
                byte == std::byte{0},
                "shape-mismatched persistent raw region must not migrate");
        }
    }

    {
        // The prepared migration path used for safe-point generation swaps must
        // preserve the same compiler-owned persistent raw storage contract.
        iv::NodeLayoutBuilder previous_builder(8);
        auto previous_region = previous_builder.declare_raw_region(
            8, 4, "graphjit.test.prepared");
        auto previous_layout = std::move(previous_builder).build();
        auto resources = make_resources();
        auto previous = previous_layout.create_storage(resources);
        previous.initialize();
        std::memset(
            previous.region_bytes(previous_region).data(),
            0x7c,
            previous.region_bytes(previous_region).size());

        iv::NodeLayoutBuilder current_builder(8);
        auto current_region = current_builder.declare_raw_region(
            8, 4, "graphjit.test.prepared");
        auto current_layout = std::move(current_builder).build();
        auto current = current_layout.create_storage(resources);
        auto migration = current.prepare_migration_from(previous);
        migration.commit();
        for (auto const byte : current.region_bytes(current_region)) {
            iv::test::require(
                byte == std::byte{0x7c},
                "prepared migration should preserve persistent raw storage");
        }
    }

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
        IndexedStorageProducer producer;
        IndexedStorageConsumer consumer;
        iv::do_declare(producer, builder);
        iv::do_declare(consumer, builder);

        auto layout = std::move(builder).build();
        auto resources = make_resources();
        auto storage = layout.create_storage(resources);
        storage.initialize();

        auto& producer_state = *static_cast<IndexedStorageProducer::IndexedState*>(
            storage.indexed_state_ptr(0));
        auto& consumer_state = *static_cast<IndexedStorageConsumer::IndexedState*>(
            storage.indexed_state_ptr(1));
        iv::test::require(
            producer_state.values.size() == 3,
            "local_array declared from IndexedState should be patched");
        iv::test::require(
            consumer_state.imported.data() == producer_state.values.data(),
            "IndexedState import/export bindings should resolve through IndexedState");
        auto const exported =
            storage.resolve_exported_array_storage<int>("indexed-values");
        iv::test::require(
            exported.data() == producer_state.values.data() && exported.size() == 3,
            "host export resolution should read IndexedState span fields");
        iv::test::require(
            consumer_state.observed_sum == 23,
            "IndexedState imports should be available during initialize");
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
            IndexedLifecycleNode::IndexedState::live_instances == 0,
            "indexed-state lifecycle test should start without live objects");

        iv::NodeLayoutBuilder builder(4);
        IndexedLifecycleNode node { .id = "indexed-state" };
        iv::do_declare(node, builder);
        iv::NodeLayout layout = std::move(builder).build();
        iv::test::require(
            layout.nodes.front().indexed_state_structure.has_value(),
            "indexed-state layout should carry ABI metadata");
        layout.nodes.front().indexed_state_structure->type_identity = {
            .nominal_id = "test.IndexedLifecycleNode.IndexedState",
            .definition_fingerprint = "v1",
            .display_name = "IndexedLifecycleNode::IndexedState",
        };
        iv::NodeLayout reloaded_layout = layout;
        static int reloaded_node_type_token = 0;
        reloaded_layout.nodes.front().node_type = &reloaded_node_type_token;

        iv::test::require(layout.nodes.size() == 1, "indexed-state layout should contain its node");
        auto const& record = layout.nodes.front();
        iv::test::require(record.state_size == 0, "test node should have no sequential State");
        iv::test::require(
            record.indexed_state_size == sizeof(IndexedLifecycleNode::IndexedState),
            "layout should record IndexedState size");
        iv::test::require(
            record.indexed_state_alignment == alignof(IndexedLifecycleNode::IndexedState),
            "layout should record IndexedState alignment");
        iv::test::require(
            record.indexed_state_offset >= 0,
            "layout should assign IndexedState storage");
        iv::test::require(
            static_cast<size_t>(record.indexed_state_offset) %
                    alignof(IndexedLifecycleNode::IndexedState) ==
                0,
            "IndexedState offset should satisfy its alignment");

        auto resources = make_resources();
        {
            iv::NodeStorage original = layout.create_storage(resources);
            original.initialize();
            auto& original_indexed =
                *static_cast<IndexedLifecycleNode::IndexedState*>(
                    original.indexed_state_ptr(0));
            iv::test::require(
                original_indexed.initialized == 1,
                "initialize should receive the default-constructed IndexedState");
            original_indexed.value = 91;

            iv::NodeStorage reloaded = reloaded_layout.create_storage(resources);
            iv::test::require(
                reloaded.can_move_from(original, 0, 0),
                "same reflected IndexedState definition should remain movable across package generations");
            auto migration = reloaded.prepare_migration_from(original);
            migration.commit();
            auto& reloaded_indexed =
                *static_cast<IndexedLifecycleNode::IndexedState*>(
                    reloaded.indexed_state_ptr(0));
            iv::test::require(
                reloaded_indexed.initialized == 1,
                "move should preserve IndexedState initialization data");
            iv::test::require(
                reloaded_indexed.moved == 1,
                "move should receive current and previous IndexedState objects");
            iv::test::require(
                reloaded_indexed.value == 91,
                "move should be able to transfer IndexedState contents");
            iv::test::require(
                original.initialized_nodes.empty(),
                "successful indexed-state migration should transfer release ownership");

            reloaded.release();
            iv::test::require(
                reloaded_indexed.released == 1,
                "release should receive the same mutable IndexedState object");
        }

        iv::test::require(
            IndexedLifecycleNode::IndexedState::live_instances == 0,
            "NodeStorage destruction should destroy every constructed IndexedState");
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
        iv::test::require(state.nested_indexed.size() == 2, "nested_node_indexed_states should record the same directly declared child nodes");
        iv::test::require(state.nested[0].data() != nullptr, "first nested node state pointer should be patched");
        iv::test::require(state.nested[1].data() != nullptr, "second nested node state pointer should be patched");
        iv::test::require(state.nested_indexed[0].size() == sizeof(NestedLeaf::IndexedState), "first nested indexed-state span should have the exact IndexedState size");
        iv::test::require(state.nested_indexed[1].size() == sizeof(NestedLeaf::IndexedState), "second nested indexed-state span should have the exact IndexedState size");
        auto& first = *reinterpret_cast<NestedLeaf::State*>(state.nested[0].data());
        auto& second = *reinterpret_cast<NestedLeaf::State*>(state.nested[1].data());
        auto& first_indexed = *reinterpret_cast<NestedLeaf::IndexedState*>(state.nested_indexed[0].data());
        auto& second_indexed = *reinterpret_cast<NestedLeaf::IndexedState*>(state.nested_indexed[1].data());
        iv::test::require(first.marker == 17, "first nested node state should be addressable");
        iv::test::require(second.marker == 17, "second nested node state should be addressable");
        iv::test::require(first_indexed.marker == 23, "first nested IndexedState should be addressable");
        iv::test::require(second_indexed.marker == 23, "second nested IndexedState should be addressable");
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
        iv::test::require(erased_state.nested_node_indexed_states.size() == 1, "type-erased node should record exactly one nested indexed child state");
        iv::test::require(erased_state.nested_node_states[0].data() != nullptr, "type-erased nested child state pointer should be patched");
        iv::test::require(erased_state.nested_node_indexed_states[0].size() == sizeof(StatefulTickingNode::IndexedState), "type-erased nested IndexedState should be patched");

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
        auto& nested_indexed_state = *reinterpret_cast<StatefulTickingNode::IndexedState*>(erased_state.nested_node_indexed_states[0].data());
        iv::test::require(nested_state.initialized == 1, "type-erased nested child should initialize once");
        iv::test::require(nested_state.ticked == 8, "type-erased nested child should tick through nested state");
        iv::test::require(nested_indexed_state.initialized == 1, "type-erased nested child IndexedState should initialize once");
        iv::test::require(nested_indexed_state.ticked == 0, "type-erased realtime tick must not mutate nested IndexedState");
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
            erased_state.nested_node_indexed_states.size() == 1,
            "weak type-erased node should record exactly one nested indexed child state");

        node.tick_block({
            iv::TickContext<iv::WeakTypeErasedNode> {
                .buffer = storage.buffer(),
            },
            0,
            8,
        });

        auto& nested_state = *reinterpret_cast<StatefulTickingNode::State*>(
            erased_state.nested_node_states[0].data());
        auto& nested_indexed_state =
            *reinterpret_cast<StatefulTickingNode::IndexedState*>(
                erased_state.nested_node_indexed_states[0].data());
        iv::test::require(
            nested_state.ticked == 8,
            "weak type-erased child should tick through nested State");
        iv::test::require(
            nested_indexed_state.ticked == 0,
            "weak type-erased realtime tick must not mutate nested IndexedState");
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
        iv::test::require(wrapper_state.nested_node_indexed_states.size() == 1, "standalone wrapper should expose one nested executable child IndexedState");
        iv::test::require(wrapper_state.nested_node_states[0].data() != nullptr, "standalone wrapper nested executable child state should be patched");
        iv::test::require(wrapper_state.nested_node_indexed_states[0].size() == sizeof(StatefulTickingNode::IndexedState), "standalone wrapper nested executable child IndexedState should be patched");

        wrapper.tick({
            iv::TickContext<iv::GraphNodeWrapper> {
                .buffer = storage.buffer(),
            },
            0,
            8,
        });

        auto& nested_state = *reinterpret_cast<StatefulTickingNode::State*>(
            wrapper_state.nested_node_states[0].data());
        auto& nested_indexed_state =
            *reinterpret_cast<StatefulTickingNode::IndexedState*>(
                wrapper_state.nested_node_indexed_states[0].data());
        iv::test::require(nested_state.ticked == 8, "graph wrapper should tick nested State");
        iv::test::require(nested_indexed_state.ticked == 0, "graph wrapper realtime tick must not mutate nested IndexedState");
    }

    return 0;
}
