#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(IV_TEST_USES_GTEST)
#include <gtest/gtest.h>
#endif

namespace iv::test {

// Escape path separators and punctuation without merging distinct test names.
inline std::string test_workspace_component(std::string_view name)
{
    if (name.empty()) throw std::invalid_argument("test workspace name is empty");
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

inline std::string& test_executable_name_storage()
{
    static std::string name;
    return name;
}

inline void set_test_executable_name(std::filesystem::path const& executable)
{
    test_executable_name_storage() = test_workspace_component(executable.stem().string());
}

inline std::string const& test_executable_name()
{
    auto const& name = test_executable_name_storage();
    if (name.empty()) throw std::logic_error("test executable name is not initialized");
    return name;
}

inline std::filesystem::path runtime_modules_root()
{
    auto const executable_root = std::filesystem::path(IV_CONFIGURED_BINARY_DIR)
        / "test_runtime_modules" / test_executable_name();
#if defined(IV_TEST_USES_GTEST)
    auto const* test = testing::UnitTest::GetInstance()->current_test_info();
    if (!test) throw std::logic_error("mutable test workspace requested outside a running test");
    return executable_root / test_workspace_component(test->test_suite_name())
        / test_workspace_component(test->name());
#else
    // A standalone executable is one test. Reset its stable root once, then
    // retain its files so the completed run can be inspected.
    static bool const prepared = [&] {
        std::filesystem::remove_all(executable_root);
        return true;
    }();
    (void)prepared;
    return executable_root;
#endif
}

} // namespace iv::test
