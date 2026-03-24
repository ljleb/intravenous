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
        iv::GraphInitContext counting(iv::GraphInitContext::PassMode::counting, counter.get_buffer());

        auto shared = counter.new_array<iv::Sample>(8);
        counting.register_buffer("shared", shared);
        auto counted_use = counting.use_buffer<iv::Sample>("shared");

        iv::test::require(counted_use.empty(), "counting pass should not return a usable shared span");
        counting.validate_after_counting();

        std::vector<std::byte> storage(256);
        iv::FixedBufferAllocator allocator({ storage.data(), storage.size() });
        iv::GraphInitContext replay = counting.make_replay_context({ storage.data(), storage.size() });

        auto replayed = allocator.new_array<iv::Sample>(8);
        replay.register_buffer("shared", replayed);
        auto used = replay.use_buffer<iv::Sample>("shared");

        iv::test::require(used.size() == replayed.size(), "replayed shared span should preserve size");
        iv::test::require(used.data() == replayed.data(), "replayed shared span should preserve offset");
        replay.validate_after_initialization();
    }

    iv::test::expect_failure([] {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x30000)));
        iv::GraphInitContext counting(iv::GraphInitContext::PassMode::counting, counter.get_buffer());
        counting.use_buffer<iv::Sample>("missing");
        counting.validate_after_counting();
    }, "never registered during the first pass", "missing registration should fail after counting");

    iv::test::expect_failure([] {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x40000)));
        iv::GraphInitContext counting(iv::GraphInitContext::PassMode::counting, counter.get_buffer());
        auto shared = counter.new_array<iv::Sample>(4);
        counting.register_buffer("shared", shared);
        counting.validate_after_counting();

        std::vector<std::byte> storage(256);
        iv::FixedBufferAllocator allocator({ storage.data(), storage.size() });
        iv::GraphInitContext replay = counting.make_replay_context({ storage.data(), storage.size() });
        replay.validate_after_initialization();
    }, "was not registered again during the second pass", "missing second registration should fail");

    iv::test::expect_failure([] {
        iv::CountingNonAllocator counter(reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x50000)));
        iv::GraphInitContext counting(iv::GraphInitContext::PassMode::counting, counter.get_buffer());
        auto shared = counter.new_array<iv::Sample>(4);
        counting.register_buffer("shared", shared);
        counting.validate_after_counting();

        std::vector<std::byte> storage(256);
        iv::FixedBufferAllocator allocator({ storage.data(), storage.size() });
        iv::GraphInitContext replay = counting.make_replay_context({ storage.data(), storage.size() });
        allocator.new_array<iv::Sample>(2);
        auto shifted = allocator.new_array<iv::Sample>(4);
        replay.register_buffer("shared", shifted);
    }, "changed offset between init passes", "offset mismatch should fail");

    return 0;
}
