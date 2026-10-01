#include <intravenous/runtime/async_capacity_manager.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <span>
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

TEST(AsyncCapacityManager, ControlWakeDoesNotManufacturePublishedWork)
{
    iv::AsyncWorkSignal signal;
    auto const work = signal.work_revision();
    auto const wake = signal.wake_revision();

    signal.notify_control();

    EXPECT_EQ(signal.work_revision(), work);
    EXPECT_EQ(signal.wake_revision(), wake + 1);
}

TEST(AsyncCapacityManager, PrivateChainIsInvisibleUntilOnePublication)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    auto chain = reserve.acquire(3);
    ASSERT_TRUE(chain);
    std::uint32_t next = 10;
    chain.for_each([&](iv::AsyncQueueBlock& block) {
        write_value(block, next++);
    });

    auto const unpublished_revision = work_signal.work_revision();
    EXPECT_TRUE(pending.pin().empty());
    EXPECT_EQ(work_signal.work_revision(), unpublished_revision);
    ASSERT_TRUE(pending.publish(std::move(chain)));
    EXPECT_EQ(work_signal.work_revision(), unpublished_revision + 1);
    auto selected = pending.pin();
    EXPECT_EQ(values(selected), (std::vector<std::uint32_t>{10, 11, 12}));
}

TEST(AsyncCapacityManager, PrivateChainWriterAdvancesAcrossPhysicalBlocks)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    auto chain = reserve.acquire(2);
    ASSERT_TRUE(chain);
    auto writer = chain.writer();
    std::vector<std::byte> first(20, std::byte{0x31});
    std::vector<std::byte> second(70, std::byte{0x72});
    EXPECT_TRUE(writer.append(first));
    EXPECT_TRUE(writer.append(second));
    EXPECT_EQ(writer.bytes_written(), 90u);
    EXPECT_EQ(writer.remaining_capacity(), 38u);

    std::vector<std::byte> actual;
    chain.for_each([&](iv::AsyncQueueBlock const& block) {
        actual.insert(
            actual.end(), block.used_storage().begin(), block.used_storage().end());
    });
    ASSERT_EQ(actual.size(), 90u);
    EXPECT_TRUE(std::ranges::all_of(
        std::span{actual}.first(20),
        [](std::byte value) { return value == std::byte{0x31}; }));
    EXPECT_TRUE(std::ranges::all_of(
        std::span{actual}.subspan(20),
        [](std::byte value) { return value == std::byte{0x72}; }));

    auto const before_failed_append = writer.bytes_written();
    std::vector<std::byte> too_large(39);
    EXPECT_FALSE(writer.append(too_large));
    EXPECT_EQ(writer.bytes_written(), before_failed_append);
}

TEST(AsyncCapacityManager, PrivateChainsJoinBeforeOneQueuePublication)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    auto pass = reserve.acquire(1);
    auto later_in_same_pass = reserve.acquire(2);
    ASSERT_TRUE(pass);
    ASSERT_TRUE(later_in_same_pass);
    pass.for_each([](iv::AsyncQueueBlock& block) { write_value(block, 1); });
    std::uint32_t next = 2;
    later_in_same_pass.for_each([&](iv::AsyncQueueBlock& block) {
        write_value(block, next++);
    });

    ASSERT_TRUE(pass.append(std::move(later_in_same_pass)));
    EXPECT_TRUE(pending.pin().empty());
    ASSERT_TRUE(pending.publish(std::move(pass)));
    EXPECT_EQ(values(pending.pin()),
        (std::vector<std::uint32_t>{1, 2, 3}));
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
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
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
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
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

TEST(AsyncCapacityManager,
     ClosedQueueHasAFinalTailAndReleasesItsLastConsumerSentinel)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    iv::ReleasedBlockQueue released;
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    auto final_chain = reserve.acquire(3);
    ASSERT_TRUE(final_chain);
    ASSERT_TRUE(pending.publish(std::move(final_chain)));
    pending.close();

    EXPECT_TRUE(pending.is_closed());
    EXPECT_FALSE(pending.is_closed_and_drained());
    EXPECT_FALSE(pending.release_closed_sentinel(released));

    {
        auto after_close = reserve.acquire(1);
        ASSERT_TRUE(after_close);
        EXPECT_FALSE(pending.publish(std::move(after_close)));
        // Rejected publication leaves the private chain with its producer so
        // normal chain destruction returns its block to the reserve.
        EXPECT_TRUE(after_close);
    }

    auto selected = pending.pin();
    std::size_t selected_block_count = 0;
    selected.for_each([&](iv::AsyncQueueBlock const&) {
        ++selected_block_count;
    });
    EXPECT_EQ(selected_block_count, 3u);
    ASSERT_TRUE(pending.release(std::move(selected), released));
    EXPECT_TRUE(pending.is_closed_and_drained());

    ASSERT_TRUE(pending.release_closed_sentinel(released));
    EXPECT_TRUE(pending.pin().empty());
    EXPECT_EQ(manager.reclaim(released), 3u);
    EXPECT_EQ(reserve.ready_block_count(), 8u);

    // Queue retirement is idempotent and cannot publish the terminal block a
    // second time.
    EXPECT_TRUE(pending.release_closed_sentinel(released));
    EXPECT_EQ(manager.reclaim(released), 0u);
}

TEST(AsyncCapacityManager, ClosingAnEmptyQueueNeedsNoBlockReclamation)
{
    iv::ProducerReserve reserve{64};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    iv::ReleasedBlockQueue released;
    iv::AsyncCapacityManager manager{4};
    ASSERT_EQ(manager.maintain(reserve, policy), 8u);

    EXPECT_FALSE(pending.is_closed_and_drained());
    pending.close();
    EXPECT_TRUE(pending.is_closed_and_drained());
    EXPECT_TRUE(pending.release_closed_sentinel(released));
    EXPECT_EQ(manager.reclaim(released), 0u);
    EXPECT_EQ(reserve.ready_block_count(), 8u);
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
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue first_pending{first_reserve, work_signal};
    iv::PendingQueue second_pending{second_reserve, work_signal};
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
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
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
