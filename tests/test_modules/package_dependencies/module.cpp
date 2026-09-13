#include <intravenous/dsl.h>

#include "dependency_api.h"

#include <stdexcept>

namespace {
void package_dependencies_module(iv::GraphBuilder& g)
{
    // These calls run from the package's ORC configuration code. They prove
    // that the finalizer linked the transitive full-LTO archive and that the
    // package JITDylib resolved the native shared-library sidecar.
    if (iv_test_lto_root_value() != 7.0f) {
        throw std::runtime_error("transitive full-LTO archive was not linked");
    }
    if (iv_test_dynamic_value() != 9.0f) {
        throw std::runtime_error("package dynamic library was not resolved");
    }
    g.outputs();
}
}

IV_MODULE("iv.test.package_dependencies", package_dependencies_module);
