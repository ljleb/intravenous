#include <intravenous/dsl.h>
#include <iv/nodes/iv.test.this_does_not_exist>

namespace iv {
constexpr missing_dependency(GraphBuilder& g)
{
    g.outputs();
}
}

IV_MODULE("iv.test.missing_dependency", iv::missing_dependency);
