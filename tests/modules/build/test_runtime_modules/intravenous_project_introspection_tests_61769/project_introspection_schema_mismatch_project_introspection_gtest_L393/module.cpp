#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/arithmetic.h"

namespace {
    template<size_t Inputs>
    iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        return g.node<iv::Sum<Inputs>>().node_ref();
    }

    void schema_mismatch_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = make_sum<2>(g);
        auto const b = make_sum<3>(g);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.schema_mismatch_module", schema_mismatch_module);
