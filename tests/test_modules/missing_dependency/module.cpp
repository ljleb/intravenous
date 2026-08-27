#include <intravenous/dsl.h>
#include <iv/modules/iv.test.this_does_not_exist>

namespace iv {
constexpr missing_dependency(GraphBuilder& g)
{
    g.outputs();
}
}
