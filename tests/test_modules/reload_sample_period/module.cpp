#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/dsl.h>

namespace {
void reload_sample_period(iv::ModuleContext const &context)
{
    auto &g = context.builder();
    auto const dt = g.node<iv::ValueSource>(&context.sample_period());
    g.outputs(dt);
}
} // namespace

IV_EXPORT_MODULE("iv.test.reload_sample_period", reload_sample_period);
