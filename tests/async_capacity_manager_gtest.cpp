#include <intravenous/runtime/async_capacity_manager.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr auto policy = iv::ProducerCapacityPolicy{
    .maximum_burst = 2,
    .low_watermark = 4,
    .high_watermark = 7,
};

void write_value(iv::AsyncQueueBlock& block, std::uint32_t value)
{
    ASSERT_GE(block.storage().size(), sizeof(value));
    std::memcpy(block.storage().data(), &value, sizeof(value));
    ASSERT_TRUE(block.set_used_size(sizeof(value)));
}

std::uint32_t read_value(iv::AsyncQueueBlock const& block)
{
    EXPECT_EQ(block.used_size(), sizeof(std::uint32_t));
    std::uint32_t value = 0;
    std::memcpy(&value, block.used_storage().data(), sizeof(value));
    return value;
}

std::vector<std::uint32_t> values(iv::PinnedBlockPrefix const& prefix)
{
    std::vector<std::uint32_t> result;
    prefix.for_each([&](iv::AsyncQueueBlock const& block) {
        result.push_back(read_value(block));
    });
    return result;
}

template<class Predicate>
bool eventually(Predicate&& predicate)
{
    auto const deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
}

TEST(AsyncCapacityManager, RefillsBelowLowWatermarkTowardHighWatermark)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncCapacityManager manager{4};

    EXPECT_EQ(manager.maintain(reserve, policy), 8u);
    EXPECT_EQ(reserve.ready_block_count(), 8u);
    EXPECT_EQ(manager.maintain(reserve, policy), 0u);

    auto consumed = reserve.acquire(5);
    ASSERT_TRUE(consumed);
    EXPECT_EQ(reserve.ready_block_count(), 3u);
    EXPECT_EQ(manager.maintain(reserve, policy), 4u);
    EXPECT_EQ(reserve.ready_block_count(), 7u);
}

TEST(AsyncCapacityManager, RejectsNonPowerOfTwoBlockAndSlabSizes)
{
    EXPECT_THROW(iv::ProducerReserve{63}, std::invalid_argument);
    EXPECT_THROW(iv::AsyncCapacityManager{3}, std::invalid_argument);
}

TEST(AsyncCapacityManager, PrivateChainIsInvisibleUntilOnePublication)
{
    iv::ProducerReserve reserve{64};
    iv::PendingQueue pending{reserve};
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    auto chain = reserve.acquire(3);
    ASSERT_TRUE(chain);
    std::uint32_t next = 10;
    chain.for_each([&](iv::AsyncQueueBlock& block) {
        write_value(block, next++);
    });

    EXPECT_TRUE(pending.pin().empty());
    ASSERT_TRUE(pending.publish(std::move(chain)));
    auto selected = pending.pin();
    EXPECT_EQ(values(selected), (std::vector<std::uint32_t>{10, 11, 12}));
}

TEST(AsyncCapacityManager, AbandonedPrivateChainReturnsToItsProducerReserve)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    {
        auto chain = reserve.acquire(5);
        ASSERT_TRUE(chain);
        EXPECT_EQ(reserve.ready_block_count(), 3u);
    }
    EXPECT_EQ(reserve.ready_block_count(), 8u);
}

TEST(AsyncCapacityManager, FailedWholeChainAcquisitionHasNoNetEffect)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    EXPECT_FALSE(reserve.acquire(9));
    EXPECT_EQ(reserve.ready_block_count(), 8u);
}

TEST(AsyncCapacityManager, LaterAppendDoesNotExtendPinnedPrefix)
{
    iv::ProducerReserve reserve{64};
    iv::PendingQueue pending{reserve};
    iv::ReleasedBlockQueue released;
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    auto first = reserve.acquire(2);
    std::uint32_t next = 1;
    first.for_each([&](iv::AsyncQueueBlock& block) {
        write_value(block, next++);
    });
    ASSERT_TRUE(pending.publish(std::move(first)));
    auto selected = pending.pin();

    auto later = reserve.acquire(2);
    later.for_each([&](iv::AsyncQueueBlock& block) {
        write_value(block, next++);
    });
    ASSERT_TRUE(pending.publish(std::move(later)));

    EXPECT_EQ(values(selected), (std::vector<std::uint32_t>{1, 2}));
    ASSERT_TRUE(pending.release(std::move(selected), released));
    EXPECT_EQ(manager.reclaim(released), 1u);

    auto next_selected = pending.pin();
    EXPECT_EQ(values(next_selected), (std::vector<std::uint32_t>{3, 4}));
}

TEST(AsyncCapacityManager, ReleasedBlocksReturnThroughTheCapacityManager)
{
    iv::ProducerReserve reserve{64};
    iv::PendingQueue pending{reserve};
    iv::ReleasedBlockQueue released;
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    auto first = reserve.acquire(3);
    ASSERT_TRUE(pending.publish(std::move(first)));
    auto first_selection = pending.pin();
    ASSERT_TRUE(pending.release(std::move(first_selection), released));

    // The selected terminal remains the pending queue's consumer sentinel.
    EXPECT_EQ(manager.reclaim(released), 2u);
    EXPECT_EQ(reserve.ready_block_count(), 7u);

    auto second = reserve.acquire(1);
    ASSERT_TRUE(pending.publish(std::move(second)));
    auto second_selection = pending.pin();
    ASSERT_TRUE(pending.release(std::move(second_selection), released));
    EXPECT_EQ(manager.reclaim(released), 1u);
    EXPECT_EQ(reserve.ready_block_count(), 7u);
}

TEST(AsyncCapacityManager, HeadroomRemainsUsableWithoutAnotherMaintenancePass)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    auto first_burst = reserve.acquire(policy.maximum_burst);
    auto second_burst = reserve.acquire(policy.maximum_burst);
    auto third_burst = reserve.acquire(policy.maximum_burst);
    ASSERT_TRUE(first_burst);
    ASSERT_TRUE(second_burst);
    ASSERT_TRUE(third_burst);
    EXPECT_EQ(reserve.ready_block_count(), 2u);
}

TEST(AsyncCapacityManager, IndependentQueuesPinIndependentPrefixes)
{
    iv::ProducerReserve first_reserve{64};
    iv::ProducerReserve second_reserve{64};
    iv::PendingQueue first_pending{first_reserve};
    iv::PendingQueue second_pending{second_reserve};
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(first_reserve, policy), 8u);
    ASSERT_EQ(manager.maintain(second_reserve, policy), 8u);

    auto first = first_reserve.acquire(1);
    first.for_each([](iv::AsyncQueueBlock& block) { write_value(block, 1); });
    ASSERT_TRUE(first_pending.publish(std::move(first)));
    auto first_selection = first_pending.pin();

    auto second = second_reserve.acquire(1);
    second.for_each([](iv::AsyncQueueBlock& block) { write_value(block, 2); });
    ASSERT_TRUE(second_pending.publish(std::move(second)));

    EXPECT_EQ(values(first_selection), (std::vector<std::uint32_t>{1}));
    EXPECT_EQ(
        values(second_pending.pin()),
        (std::vector<std::uint32_t>{2}));
}

TEST(AsyncCapacityManager, RegisteredProducerIsReplenishedByManagerWorker)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncCapacityManager manager{4};
    auto registration = manager.register_producer(reserve, policy);
    ASSERT_TRUE(registration);
    ASSERT_EQ(reserve.ready_block_count(), 8u);

    auto retained = reserve.acquire(7);
    ASSERT_TRUE(retained);
    EXPECT_TRUE(eventually([&] {
        return reserve.ready_block_count() >= policy.high_watermark;
    }));
    EXPECT_FALSE(manager.failures().any());
}

TEST(AsyncCapacityManager, ManagerWorkerReclaimsSharedReleasedBlockStream)
{
    iv::ProducerReserve reserve{64};
    iv::PendingQueue pending{reserve};
    iv::AsyncCapacityManager manager{4};
    auto registration = manager.register_producer(reserve, policy);
    ASSERT_TRUE(registration);

    auto produced = reserve.acquire(3);
    ASSERT_TRUE(pending.publish(std::move(produced)));
    auto selected = pending.pin();
    ASSERT_TRUE(pending.release(
        std::move(selected), manager.released_blocks()));

    EXPECT_TRUE(eventually([&] {
        return reserve.ready_block_count() == 7;
    }));
}

TEST(AsyncCapacityManager, RegistrationRemovalStopsReserveMaintenance)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncCapacityManager manager{4};
    auto registration = manager.register_producer(reserve, policy);
    ASSERT_TRUE(registration);
    registration.reset();

    auto const available = reserve.ready_block_count();
    ASSERT_NE(available, 0u);
    auto retained = reserve.acquire(available);
    ASSERT_TRUE(retained);
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    EXPECT_EQ(reserve.ready_block_count(), 0u);
}

TEST(AsyncCapacityManager, OneReserveCannotHaveTwoManagerRegistrations)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncCapacityManager manager{4};
    auto registration = manager.register_producer(reserve, policy);
    ASSERT_TRUE(registration);

    EXPECT_THROW(
        {
            auto duplicate = manager.register_producer(reserve, policy);
            static_cast<void>(duplicate);
        },
        std::invalid_argument);
}

} // namespace
