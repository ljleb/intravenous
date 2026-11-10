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

[[gnu::noinline]] void capture_from_depth(void const* exception_object, int depth)
{
    if (depth == 0) {
        iv::details::capture_throw_site(exception_object);
        return;
    }
    capture_from_depth(exception_object, depth - 1);
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

TEST(ThrowSite, ReusedExceptionAddressSelectsTheNewestCapture)
{
    try {
        throw std::runtime_error("reused exception storage");
    } catch (std::exception const& error) {
        auto const* exception_object = dynamic_cast<void const*>(&error);
        ASSERT_NE(exception_object, nullptr);

        // Replace every retained slot with a deliberately deep stack for the
        // same address, then record one shallow stack as the newest capture.
        // An oldest-first lookup returns one of the stale deep stacks.
        for (int index = 0; index != 16; ++index) {
            capture_from_depth(exception_object, 12);
        }
        capture_from_depth(exception_object, 0);

        auto const handler = std::stacktrace::current();
        auto const site = iv::throw_site_stacktrace(std::current_exception());
        ASSERT_TRUE(site.has_value());
        EXPECT_LT(site->size(), handler.size() + 8);
    }
}
