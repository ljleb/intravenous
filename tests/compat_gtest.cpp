#include <intravenous/compat.h>

#include <gtest/gtest.h>

#include <stdexcept>

TEST(Compat, WrapExceptionPreservesContextAndCause)
{
    std::runtime_error const cause("cause");
    EXPECT_EQ(iv::wrap_exception("context", cause), "context\ncaused by: cause");
}
