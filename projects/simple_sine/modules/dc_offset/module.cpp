#include <intravenous/dsl.h>

void module_main(iv::GraphBuilder& g)
{
    using namespace iv;
    g.outputs(0.01);
}

IV_MODULE("iv.project.dc_offset", module_main);
