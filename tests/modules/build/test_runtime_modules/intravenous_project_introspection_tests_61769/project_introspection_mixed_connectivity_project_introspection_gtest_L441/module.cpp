#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/arithmetic.h>

namespace {
    template<int I>
    iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        (void)I;
        return g.node<iv::Sum<1>>().node_ref();
    }

    void mixed_connectivity_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const value = g.node<iv::ValueSource>(&context.sample_period()).node_ref();
        auto const a = make_sum<0>(g);
        auto const b = make_sum<1>(g);
        a(value);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.mixed_connectivity_module", mixed_connectivity_module);
