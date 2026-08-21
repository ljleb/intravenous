#include <intravenous/dsl.h>
#include <iv/modules/iv.test.this_does_not_exist>

namespace iv {
void missing_dependency(GraphBuilder& g)
{
    g.outputs();
}
}
