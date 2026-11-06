// Throw-site stacks (src/intravenous/throw_site.h): the `__cxa_throw` hook
// records the stack before unwinding, so a handler can report the frames that
// unwinding already removed.

#include <intravenous/throw_site.h>

#include <gtest/gtest.h>

#include <exception>
#include <stacktrace>
#include <stdexcept>

namespace {
[[gnu::noinline]] void throw_from_depth(int depth)
{
    if (depth == 0) throw std::runtime_error("deep failure");
    throw_from_depth(depth - 1);
    // Keeps the recursive call from becoming a tail call.
    asm volatile("");
}
} // namespace

TEST(ThrowSite, CapturesTheFramesThatUnwindingRemoved)
{
    try {
        throw_from_depth(8);
        FAIL() << "no exception was thrown";
    } catch (...) {
        auto const handler = std::stacktrace::current();
        auto const site = iv::throw_site_stacktrace(std::current_exception());
        ASSERT_TRUE(site.has_value());
        EXPECT_GE(site->size(), handler.size() + 8);
    }
}

TEST(ThrowSite, ANonStandardExceptionHasNoCapturedSite)
{
    try {
        throw 42;
    } catch (...) {
        EXPECT_FALSE(iv::throw_site_stacktrace(std::current_exception()).has_value());
    }
}

TEST(ThrowSite, RethrowingKeepsTheOriginalSite)
{
    std::exception_ptr failure;
    try {
        try {
            throw_from_depth(4);
        } catch (...) {
            throw;
        }
    } catch (...) {
        failure = std::current_exception();
    }
    auto const site = iv::throw_site_stacktrace(failure);
    ASSERT_TRUE(site.has_value());
    EXPECT_GE(site->size(), std::stacktrace::current().size() + 4);
}
