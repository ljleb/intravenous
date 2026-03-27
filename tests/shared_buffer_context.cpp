#include "alligator.h"
#include "node.h"
#include "module_test_utils.h"

#include <cstdint>
#include <vector>

int main()
{
    iv::test::install_crash_handlers();

    {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x20000)));
        iv::InitBufferContext counting(iv::InitBufferContext::PassMode::counting, counter.get_buffer(), 1);

        auto shared = counting.register_init_buffer<iv::Sample>("shared", 8);
        auto counted_use = counting.use_init_buffer<iv::Sample>("shared");

        iv::test::require(counted_use.size() == shared.size(), "init buffer should preserve size during counting");
        iv::test::require(counted_use.data() == shared.data(), "init buffer should preserve heap storage during counting");
        counting.validate_after_counting();

        std::vector<std::byte> storage(256);
        iv::InitBufferContext replay = counting.make_initializing_context({ storage.data(), storage.size() });

        auto replayed = replay.register_init_buffer<iv::Sample>("shared", 8);
        auto used = replay.use_init_buffer<iv::Sample>("shared");

        iv::test::require(used.size() == replayed.size(), "replayed shared span should preserve size");
        iv::test::require(used.data() == replayed.data(), "replayed shared span should preserve offset");
        replay.validate_after_initialization();
    }

    iv::test::expect_failure([] {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x30000)));
        iv::InitBufferContext counting(iv::InitBufferContext::PassMode::counting, counter.get_buffer(), 1);
        counting.use_init_buffer<iv::Sample>("missing");
    }, "used before registration", "missing init registration should fail immediately");

    iv::test::expect_failure([] {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x40000)));
        iv::InitBufferContext counting(iv::InitBufferContext::PassMode::counting, counter.get_buffer(), 1);
        counting.register_init_buffer<iv::Sample>("shared", 4);
        counting.validate_after_counting();

        std::vector<std::byte> storage(256);
        iv::InitBufferContext replay = counting.make_initializing_context({ storage.data(), storage.size() });
        replay.validate_after_initialization();
    }, "was not registered again during the second pass", "missing second init registration should fail");

    iv::test::expect_failure([] {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x50000)));
        iv::InitBufferContext counting(iv::InitBufferContext::PassMode::counting, counter.get_buffer(), 1);
        counting.register_init_buffer<iv::Sample>("shared", 4);
        counting.validate_after_counting();

        std::vector<std::byte> storage(256);
        iv::InitBufferContext replay = counting.make_initializing_context({ storage.data(), storage.size() });
        replay.register_init_buffer<iv::Sample>("shared", 2);
    }, "changed element count", "init buffer size mismatch should fail");

    {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x60000)));
        iv::InitBufferContext counting(iv::InitBufferContext::PassMode::counting, counter.get_buffer(), 1);

        auto early = counting.use_tick_buffer<iv::Sample>("tick");
        iv::test::require(early.empty(), "counting tick use before register should not expose a live span");
        auto tick = counter.new_array<iv::Sample>(1);
        counting.register_tick_buffer("tick", tick);
        counting.validate_after_counting();

        std::vector<std::byte> storage(256);
        iv::FixedBufferAllocator allocator({ storage.data(), storage.size() });
        iv::InitBufferContext replay = counting.make_initializing_context({ storage.data(), storage.size() });

        auto resolved = replay.use_tick_buffer<iv::Sample>("tick");
        auto replayed_tick = allocator.new_array<iv::Sample>(1);
        replay.register_tick_buffer("tick", replayed_tick);

        iv::test::require(resolved.size() == replayed_tick.size(), "tick buffer should preserve size across passes");
        iv::test::require(resolved.data() == replayed_tick.data(), "tick buffer should resolve to the runtime span before registration");
        replay.validate_after_initialization();
    }

    iv::test::expect_failure([] {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x70000)));
        iv::InitBufferContext counting(iv::InitBufferContext::PassMode::counting, counter.get_buffer(), 1);
        counting.use_tick_buffer<iv::Sample>("missing");
        counting.validate_after_counting();
    }, "used but never registered during the first pass", "missing tick registration should fail after counting");

    iv::test::expect_failure([] {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x80000)));
        iv::InitBufferContext counting(iv::InitBufferContext::PassMode::counting, counter.get_buffer(), 1);
        auto tick = counter.new_array<iv::Sample>(1);
        counting.register_tick_buffer("tick", tick);
        counting.validate_after_counting();

        std::vector<std::byte> storage(256);
        iv::FixedBufferAllocator allocator({ storage.data(), storage.size() });
        iv::InitBufferContext replay = counting.make_initializing_context({ storage.data(), storage.size() });
        allocator.new_array<iv::Sample>(2);
        auto shifted = allocator.new_array<iv::Sample>(1);
        replay.register_tick_buffer("tick", shifted);
    }, "changed offset", "tick buffer offset mismatch should fail");

    return 0;
}
