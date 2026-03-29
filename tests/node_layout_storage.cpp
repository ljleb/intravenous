#include "node_lifecycle.h"
#include "module_test_utils.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
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

        iv::test::require(original.initialized_nodes.empty(), "move reload should transfer old release ownership");
        iv::test::require(reloaded_state.initialized == 1, "move should preserve initialized count");
        iv::test::require(reloaded_state.moved == 1, "move should run when layout is compatible");
        iv::test::require(reloaded_state.scratch.size() == 2, "moved node local span should be patched");
        iv::test::require(reloaded_state.scratch[0] == 7, "move should preserve scratch[0]");
        iv::test::require(reloaded_state.scratch[1] == 9, "move should preserve scratch[1]");
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

        iv::test::require(original.initialized_nodes.empty(), "fallback reload should transfer old release ownership");
        iv::test::require(original_state.value == nullptr, "state move construction should transfer unique ownership");
        iv::test::require(reloaded_state.value != nullptr, "reloaded state should receive moved ownership");
        iv::test::require(*reloaded_state.value == 41, "moved state value should be preserved");
        iv::test::require(reloaded_state.initialized == 2, "fallback initialize should still run after state move construction");
        iv::test::require(reloaded_state.released == 1, "fallback release should run on the previous state");
        iv::test::require(reloaded_state.scratch[0] == 41, "initialize should repopulate scratch[0] after repatching");
        iv::test::require(reloaded_state.scratch[1] == 2, "initialize should observe incremented initialized count");
    }

    return 0;
}
