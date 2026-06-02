#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>

namespace {
    template<int I>
    iv::NodeRef make_value(iv::GraphBuilder& g, iv::ModuleContext const& context)
    {
        (void)I;
        return g.node<iv::ValueSource>(&context.sample_period()).node_ref();
    }

    void merged_logical_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = make_value<0>(g, context);
        auto const b = make_value<1>(g, context);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.merged_logical_module", merged_logical_module);
