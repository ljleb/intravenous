#include "node_lifecycle.h"
#include "module_test_utils.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {
    inline iv::ResourceContext make_resources()
    {
        static iv::ResourceContext::VstResources vst {};
        return iv::ResourceContext { vst };
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

    return 0;
}
