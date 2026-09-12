#pragma once

#if defined(_WIN32)
#define IV_TEST_DEPENDENCY_EXPORT __declspec(dllexport)
#else
#define IV_TEST_DEPENDENCY_EXPORT
#endif

extern "C" float iv_test_lto_leaf_value();
extern "C" float iv_test_lto_root_value();
extern "C" IV_TEST_DEPENDENCY_EXPORT float iv_test_dynamic_value();
extern "C" float iv_test_native_archive_value();
