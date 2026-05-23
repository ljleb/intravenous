#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void assigned_ref_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        NodeRef x;
        x = g.node<ValueSource>(&context.sample_period()).node_ref();
        auto const sink = context.target_factory().sink(g, 0);
        sink(x);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.assigned_ref_module", assigned_ref_module);
