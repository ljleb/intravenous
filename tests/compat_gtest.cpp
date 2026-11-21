#include <intravenous/compat.h>
#include <intravenous/node/context.h>
#include <intravenous/runtime/handlers.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>

#if GTEST_HAS_DEATH_TEST
namespace {
void use_stderr_diagnostics()
{
#if defined(_WIN32)
    _putenv_s("IV_LOG_FILE", "");
#else
    unsetenv("IV_LOG_FILE");
#endif
}
} // namespace
#endif

TEST(Compat, WrapExceptionPreservesContextAndCause)
{
    std::runtime_error const cause("cause");
    EXPECT_EQ(iv::wrap_exception("context", cause), "context\ncaused by: cause");
}

#if GTEST_HAS_DEATH_TEST
TEST(CompatDeathTest, EscapedNodeLifecycleExceptionIsFatalWithContext)
{
    EXPECT_DEATH(
        {
            use_stderr_diagnostics();
            iv::install_crash_handlers();
            iv::details::invoke_node_lifecycle<int>("move", 17, [] {
                throw std::runtime_error("authored move failure");
            });
        },
        "operation: move");
}
#endif
