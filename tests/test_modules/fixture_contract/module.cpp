#include <intravenous/dsl.h>

// fixture contract source
void fixture_contract(iv::GraphBuilder& g)
{
    g.outputs();
}

IV_MODULE("iv.test.fixture_contract", fixture_contract);
