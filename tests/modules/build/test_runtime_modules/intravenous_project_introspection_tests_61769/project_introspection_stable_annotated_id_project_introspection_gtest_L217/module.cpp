#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/buffers.h>

namespace {
    void annotated_symbol_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = _annotate_node_source_info(
            g.node<ValueSource>(&context.sample_period()).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const sink = context.target_factory().sink(g, 0);
        sink(a);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.annotated_symbol_module", annotated_symbol_module);
