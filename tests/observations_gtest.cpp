#include <intravenous/runtime/observation_types.h>
#include <intravenous/runtime/observation_delivery.h>
#include <intravenous/runtime/event_observation_window.h>
#include <intravenous/runtime/observation_window.h>
#include <intravenous/runtime/observations.h>
#include <intravenous/runtime/socket_rpc_routes.h>
#include <intravenous/bridge.h>
#include "event_source_test_support.h"
#include <nlohmann/json.hpp>
#include <intravenous/continuous_value.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <atomic>
#include <thread>

namespace {
template<class T> T value_at(iv::ObservationResultData const& result, std::size_t offset)
{
    T value;
    std::memcpy(&value, result.bytes.data() + offset, sizeof(value));
    return value;
}

TEST(Observations, RecordedEventTimestampsKeepEqualTimeEventsAndWindowOrderAcrossTheSeam)
{
    auto const low = iv::GlobalIndex::from_signed(std::numeric_limits<std::int64_t>::min());
    auto const high = low.retreated_by(1);
    std::array const timeline{
        iv::TimedEvent{low, iv::TriggerEvent{}},
        iv::TimedEvent{low.advanced_by(1), iv::TriggerEvent{}},
        iv::TimedEvent{high, iv::TriggerEvent{}},
        iv::TimedEvent{high, iv::TriggerEvent{}},
    };
    iv::ObservationWindowQuery query{.window = {high, 3}};
    iv::project_recorded_event_timestamps(query, timeline);
    EXPECT_EQ(query.result.event_indices, (std::vector{high, high, low, low.advanced_by(1)}));
    EXPECT_FALSE(query.result.truncated);

    iv::ObservationWindowQuery complete{.window = {high, 2}};
    iv::project_recorded_event_timestamps(complete, timeline);
    EXPECT_EQ(complete.result.event_indices, (std::vector{high, high, low}));
    EXPECT_FALSE(complete.result.truncated);
}

TEST(Observations, RecordedEventTimestampsAreNotLimitedByTheSampleWindowBudget)
{
    auto const count = iv::maximum_observation_window_points + 2;
    std::vector<iv::TimedEvent> timeline;
    for (std::size_t i = 0; i < count; ++i)
        timeline.push_back({iv::GlobalIndex{}.advanced_by(i), iv::TriggerEvent{}});
    iv::ObservationWindowQuery query{.window = {iv::GlobalIndex{}, count}};
    iv::project_recorded_event_timestamps(query, timeline);
    EXPECT_EQ(query.result.event_indices.size(), count);
    EXPECT_FALSE(query.result.truncated);
}

TEST(Observations, BeatGridFindsIrregularPositionsByIndexAndKeepsStridePhase)
{
    struct PublishedGrid {
        iv::GlobalIndexRange range{iv::GlobalIndex{}, 1000000000000ULL};
        iv::EventRegionInfo region{range, iv::IndexedEventRegionInfo{1000000, iv::BeatEvent{0}}};
        std::size_t probes = 0;
    } grid;
    // Each pair is coincident and the distance between pairs grows.
    auto const time_of = [](std::uint64_t index) {
        return iv::GlobalIndex::from_bits((index / 2) * (index / 2));
    };
    iv::BackgroundDemandSelection const candidate{
        .data = &grid,
        .output_regions = +[](void const* data, iv::graph_jit::BackgroundPortIndex) {
            return std::span{&static_cast<PublishedGrid const*>(data)->range, 1};
        },
        .event_regions = +[](void const* data, iv::graph_jit::BackgroundPortIndex) {
            return std::span{&static_cast<PublishedGrid const*>(data)->region, 1};
        },
        .read_events = +[](void* data, std::span<iv::IndexedRegionDemand const> requests) {
            auto& grid = *static_cast<PublishedGrid*>(data);
            std::vector<std::vector<iv::TimedEvent>> result;
            for (auto const& request : requests) {
                auto const index = request.runs.front().first;
                ++grid.probes;
                auto const pair = index / 2;
                result.push_back({{iv::GlobalIndex::from_bits(pair * pair), iv::BeatEvent{0}}});
            }
            return result;
        },
    };
    iv::graph_jit::BackgroundEvaluationPlan const plan;
    iv::graph_jit::PublicEventCoverageInputView const input{.sources = {{.output = 0}}};
    iv::ObservationWindowQuery query;
    query.window = {.begin = time_of(16000), .length = 10000000000ULL, .points = 16, .max_depth = 0};
    iv::CoveragePropagationRequest demand;
    iv::BeatGridObservation observation{query, input, plan, query.window.points};
    observation.select(candidate, demand, 8192, 8192);

    ASSERT_TRUE(demand.output_demands.empty());
    ASSERT_EQ(demand.indexed_output_demands.size(), 1u);
    auto const run = demand.indexed_output_demands.front().runs.front();
    ASSERT_GT(run.count, 0u);
    EXPECT_LE(run.count, query.window.points);
    EXPECT_LE(grid.probes, 40u);
    EXPECT_TRUE(std::has_single_bit(run.step));
    EXPECT_EQ(run.first % run.step, 0u);
    iv::GlobalIndexRange const viewport{query.window.begin, query.window.length};
    EXPECT_TRUE(viewport.contains(time_of(run.first)));
    EXPECT_TRUE(viewport.contains(time_of(run.first + (run.count - 1) * run.step)));
    if (run.first >= run.step) EXPECT_FALSE(viewport.contains(time_of(run.first - run.step)));
}

TEST(Observations, BeatGridRetainsSomeSubdivisionMarksWhenAFullLevelExceedsTheBudget)
{
    struct PublishedGrid {
        std::array<iv::GlobalIndexRange, 2> ranges{{{iv::GlobalIndex{}, 200}, {iv::GlobalIndex{}, 200}}};
        std::array<iv::EventRegionInfo, 2> regions{{
            {ranges[0], iv::IndexedEventRegionInfo{2, iv::BeatEvent{0}}},
            {ranges[1], iv::IndexedEventRegionInfo{100, iv::BeatEvent{1}}},
        }};
    } grid;
    iv::BackgroundDemandSelection const candidate{
        .data = &grid,
        .output_regions = +[](void const* data, iv::graph_jit::BackgroundPortIndex) -> std::span<iv::GlobalIndexRange const> {
            return static_cast<PublishedGrid const*>(data)->ranges;
        },
        .event_regions = +[](void const* data, iv::graph_jit::BackgroundPortIndex) -> std::span<iv::EventRegionInfo const> {
            return static_cast<PublishedGrid const*>(data)->regions;
        },
    };
    iv::graph_jit::BackgroundEvaluationPlan const plan;
    iv::graph_jit::PublicEventCoverageInputView const input{.sources = {{.output = 0}}};
    iv::ObservationWindowQuery query;
    query.window = {.begin = iv::GlobalIndex{}, .length = 200, .points = 10, .max_depth = 1};
    iv::CoveragePropagationRequest demand;
    iv::BeatGridObservation observation{query, input, plan, query.window.points};
    observation.select(candidate, demand, 8192, 8192);

    ASSERT_EQ(demand.indexed_output_demands.size(), 2u);
    auto const bars = demand.indexed_output_demands[0].runs.front();
    auto const beats = demand.indexed_output_demands[1].runs.front();
    EXPECT_EQ(bars.count, 2u);
    EXPECT_EQ(beats.first, 0u);
    EXPECT_EQ(beats.step, 16u);
    EXPECT_EQ(beats.count, 7u);
    EXPECT_TRUE(query.result.truncated);
}

TEST(Observations, WaveformReuseSeparatesEqualRangeRegionsAndUnavailablePoints)
{
    auto const first = iv::GlobalIndex::from_bits(10);
    auto const second = first.advanced_by(1);
    auto const gap = first.advanced_by(2);
    iv::ObservationCaptureState value1, value2;
    value1.store(1.0f);
    value2.store(2.0f);
    std::array const regions{
        iv::ObservationWindowRegion{{first, 2}, {{first, value1}, {second, value1}}, {0}},
        iv::ObservationWindowRegion{{first, 2}, {{first, value2}, {second, {}}}, {1}},
    };
    std::array const positions{first, second, gap};
    iv::ObservationWindowReuse const cache{{{positions, regions}}};
    std::array<std::size_t, 1> const first_key{0}, second_key{1};
    EXPECT_TRUE(cache.contains(first));
    EXPECT_FALSE(cache.contains(second));
    EXPECT_TRUE(cache.contains(gap));
    EXPECT_FALSE(cache.contains(gap.advanced_by(1)));
    ASSERT_NE(cache.value(first_key, first), nullptr);
    ASSERT_NE(cache.value(second_key, first), nullptr);
    EXPECT_FLOAT_EQ(cache.value(first_key, first)->load<float>(), 1.0f);
    EXPECT_FLOAT_EQ(cache.value(second_key, first)->load<float>(), 2.0f);
}
}

TEST(Observations, SampleLevelReadsEachWrappedStridedChannelWithoutMixing)
{
    std::array<float, 8> interleaved{1, -2, 3, -4, 5, -6, 7, std::numeric_limits<float>::infinity()};
    std::array channels{
        iv::ObservationChannelView{reinterpret_cast<std::byte const*>(interleaved.data()), 4, 2 * sizeof(float), 1},
        iv::ObservationChannelView{reinterpret_cast<std::byte const*>(interleaved.data() + 1), 4, 2 * sizeof(float), 1},
    };
    auto const& type = iv::continuous_value_type_info(iv::ContinuousValueTypeId::sample);
    auto registered = std::ranges::find(type.observation_operations, "level", &iv::ObservationOperation::id);
    ASSERT_NE(registered, type.observation_operations.end());
    auto const& operation = *registered;
    iv::ObservationCaptureState left, right, unselected;
    operation.capture_block(left, {std::span{channels}.first(1), 4, 2}); // Frames 3, 0.
    operation.capture_block(right, {std::span{channels}.subspan(1), 4, 2});
    operation.capture_block(unselected, {channels, 4, 2});
    EXPECT_FALSE(unselected.captured); // No implicit mono reduction.
    auto left_result = operation.project(left), right_result = operation.project(right);
    EXPECT_DOUBLE_EQ(value_at<double>(left_result, 0), 7);
    EXPECT_EQ(value_at<std::uint64_t>(left_result, 16), 2u);
    EXPECT_DOUBLE_EQ(value_at<double>(left_result, 8), 5);
    EXPECT_DOUBLE_EQ(value_at<double>(right_result, 0), 2);
    EXPECT_EQ(value_at<std::uint64_t>(right_result, 16), 1u);
    EXPECT_EQ(value_at<std::uint64_t>(right_result, 24), 1u);
    EXPECT_TRUE(iv::continuous_value_type_info(iv::ContinuousValueTypeId::fft_block).observation_operations.empty());
}

TEST(Observations, SampleValuePreservesTheSignedFinalSampleAcrossSlicesAndEmptyWindows)
{
    std::array<float, 4> samples{.25f, -.5f, 2.5f, -3.0f};
    std::array channels{iv::ObservationChannelView{reinterpret_cast<std::byte const*>(samples.data()), 4, sizeof(float), 0}};
    auto const& operations = iv::continuous_value_type_info(iv::ContinuousValueTypeId::sample).observation_operations;
    auto operation = std::ranges::find(operations, "value", &iv::ObservationOperation::id);
    ASSERT_NE(operation, operations.end());
    iv::ObservationCaptureState state;
    operation->capture_block(state, {channels, 0, 2});
    EXPECT_FLOAT_EQ(value_at<float>(operation->project(state), 0), -.5f);
    operation->capture_block(state, {channels, 2, 2});
    EXPECT_FLOAT_EQ(value_at<float>(operation->project(state), 0), -3.0f);
    operation->capture_block(state, {channels, 4, 0});
    EXPECT_FLOAT_EQ(value_at<float>(operation->project(state), 0), -3.0f);
    operation->capture_block(state, {channels, 4, 1}); // Ring wrap.
    EXPECT_FLOAT_EQ(value_at<float>(operation->project(state), 0), .25f);
}

TEST(Observations, ScalarLatestCopiesOnlyItsRegisteredValue)
{
    iv::ObservationCaptureState accumulator;
    double value = -42.125;
    auto const& operation = iv::observation_details::double_latest;
    operation.capture_state(accumulator, &value);
    value = 99;
    EXPECT_DOUBLE_EQ(value_at<double>(operation.project(accumulator), 0), -42.125);
}

TEST(Observations, HandoffCoalescesFramesAndPublishesEmptyActivation)
{
    auto handoff = std::make_unique<iv::LiveObservationHandoff>();
    iv::CompiledObservationPlan plan{.revision = 4, .probes = {{.id = 19,
        .operation = &iv::observation_details::float_latest, .available = true}}};
    for (std::uint64_t activation : {1u, 2u}) {
        auto& frame = handoff->begin(plan, activation, 5, {}, 64);
        frame.values[0].value.store(activation);
        handoff->publish();
    }
    auto const* frame = handoff->collect();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->activation_id, 2u);
    EXPECT_EQ(frame->values[0].value.load<std::uint64_t>(), 2u);
    EXPECT_EQ(handoff->collect(), nullptr);
    plan.probes.clear();
    handoff->begin(plan, 3, 6, {}, 64);
    handoff->publish();
    frame = handoff->collect();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->count, 0u);
    EXPECT_EQ(frame->activation_id, 3u);
}

TEST(Observations, HandoffNeverExposesConcurrentWriterStorage)
{
    auto handoff = std::make_unique<iv::LiveObservationHandoff>();
    std::atomic<bool> done = false;
    std::jthread writer([&] {
        iv::CompiledObservationPlan plan{.revision = 1, .probes = {{.id = 1}}};
        for (std::uint64_t n = 1; n <= 20000; ++n) {
            auto& frame = handoff->begin(plan, n, n, iv::GlobalIndex::from_bits(n), 64);
            frame.values[0].value.store(n);
            handoff->publish();
        }
        done.store(true, std::memory_order_release);
    });
    std::uint64_t last = 0;
    do {
        if (auto const* frame = handoff->collect()) {
            EXPECT_GE(frame->activation_id, last);
            last = frame->activation_id;
            EXPECT_EQ(frame->project_generation, last);
            EXPECT_EQ(frame->sample_index.bits(), last);
            EXPECT_EQ(frame->values[0].value.load<std::uint64_t>(), last);
        }
    } while (!done.load(std::memory_order_acquire));
}

TEST(Observations, RpcUsesServerOwnedIdentityAndRejectsUnknownSourceKinds)
{
    auto parsed = iv::parse_socket_rpc_request(R"({"jsonrpc":"2.0","id":1,"method":"observations.reconcile","params":{"clientId":"forged","scopeId":"viewport","interests":[{"id":"level","source":{"node":{"instanceId":"node","path":[]},"kind":"output","member":"out"},"operation":"level"}]}})");
    auto const* request = std::get_if<iv::ReconcileObservationsRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_TRUE(request->client_id.empty());
    EXPECT_EQ(request->interests.size(), 1u);
    EXPECT_THROW(iv::parse_socket_rpc_request(R"({"jsonrpc":"2.0","id":1,"method":"observations.reconcile","params":{"scopeId":"viewport","interests":[{"id":"x","source":{"node":{"instanceId":"node"},"kind":"input","member":"in"},"operation":"level"}]}})"), std::exception);

}

namespace iv::test::observation_tests {
using Json = nlohmann::ordered_json;
struct ProtocolCaller {};
struct Graph {
    std::optional<std::uint64_t> generation = 0;
    std::optional<std::uint64_t> active_generation = 0;
    std::optional<std::uint64_t> execution_generation;
    ObservationRequirements accepted{};
    std::optional<ObservationRequirements> prediction{};
    std::size_t compilations = 0;
    void apply(ObservationGraphRequest& request)
    {
        request.handled = true;
        request.configured_generation = generation;
        request.window_configured_generation = active_generation;
        request.window_execution_generation = execution_generation;
        if (request.requirements) { accepted = *request.requirements; ++compilations; }
        if (request.prediction_changed) prediction = request.prediction;
        for (auto& window : request.windows)
            if (window.select_execution(active_generation.value_or(0), execution_generation))
                window.query->operation = &observation_details::sample_operations.back();
        request.capabilities = std::make_shared<Json const>(Json::array());
    }
};
struct Executor {
    std::optional<LiveObservationFrame> frame;
    std::vector<std::shared_ptr<ObservationWindowQuery>> windows{};
    std::uint64_t data_version = 0;
    void collect(ObservationCollectRequest& request)
    {
        request.handled = true; request.frame = std::move(frame); frame.reset();
        windows.insert(windows.end(), request.windows.begin(), request.windows.end());
        request.execution_data_version = data_version;
    }
    void publish(ObservationRequirements const& requirements, std::uint64_t activation, bool available = true)
    {
        frame.emplace();
        frame->requirements_revision = requirements.revision;
        frame->activation_id = activation;
        frame->count = requirements.probes.size();
        for (std::size_t i = 0; i < frame->count; ++i) frame->values[i] = {
            .request_id = requirements.probes[i].id,
            .operation = &observation_details::float_latest,
            .status = available ? ObservationValueStatus::available : ObservationValueStatus::unavailable,
            .value = {.captured = true},
        };
    }
};
struct Delivery {
    std::vector<ObservationDelivery> batches;
    void receive(std::span<ObservationDelivery const> values)
    { batches.assign(values.begin(), values.end()); }
};
IV_DECLARE_BRIDGE(observation_test_protocol, ProtocolCaller, Observations);
IV_DEFINE_BRIDGE(observation_test_protocol)
IV_SUBSCRIBE_LINKER_EVENT(observation_test_protocol, iv_socket_rpc_observation_event, &Observations::handle_protocol)
IV_DECLARE_BRIDGE(observation_test_graph, Observations, Graph);
IV_DECLARE_BRIDGE(observation_test_executor, Observations, Executor);
IV_DECLARE_BRIDGE(observation_test_delivery, Observations, Delivery);
IV_DEFINE_BRIDGE(observation_test_graph)
IV_SUBSCRIBE_LINKER_EVENT(observation_test_graph, iv_observations_project_graph_event, &Graph::apply)
IV_DEFINE_BRIDGE(observation_test_executor)
IV_SUBSCRIBE_LINKER_EVENT(observation_test_executor, iv_observations_graph_executor_event, &Executor::collect)
IV_DEFINE_BRIDGE(observation_test_delivery)
IV_SUBSCRIBE_LINKER_EVENT(observation_test_delivery, iv_observations_socket_rpc_event, &Delivery::receive)

class ObservationService : public ::testing::Test {
protected:
    Observations observations;
    Graph graph;
    Executor executor;
    Delivery delivery;
    decltype(observation_test_graph::bind(observations, graph)) graph_binding = observation_test_graph::bind(observations, graph);
    decltype(observation_test_executor::bind(observations, executor)) executor_binding = observation_test_executor::bind(observations, executor);
    decltype(observation_test_delivery::bind(observations, delivery)) delivery_binding = observation_test_delivery::bind(observations, delivery);
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    ObservationInterest interest(std::string id = "meter", std::string node = "node")
    { return {.id = std::move(id), .source = {.node = {.instance_id = std::move(node)}, .member = "out"}, .operation = "level"}; }
    ObservationProtocolRequest reconcile(std::string client, std::string scope, std::vector<ObservationInterest> interests,
        std::vector<ObservationInterest> prediction = {})
    {
        ObservationProtocolRequest request{.kind = ObservationProtocolRequest::Kind::reconcile,
            .client_id = std::move(client), .scope_id = std::move(scope), .interests = std::move(interests),
            .predicted_interests = std::move(prediction)};
        in_event_source([&] { observations.handle_protocol(request); });
        return request;
    }
    void collect()
    { now += std::chrono::milliseconds(60); observations.collect(now); }
};

TEST_F(ObservationService, SharesOneCaptureAcrossClientsAndRenewsWithoutCompilation)
{
    EXPECT_TRUE(reconcile("a", "lane", {interest()}).error.empty());
    EXPECT_TRUE(reconcile("b", "lane", {interest("other")}).error.empty());
    EXPECT_EQ(graph.compilations, 1u);
    ASSERT_EQ(graph.accepted.probes.size(), 1u);
    EXPECT_TRUE(reconcile("a", "lane", {interest()}).error.empty());
    EXPECT_EQ(graph.compilations, 1u);
    executor.publish(graph.accepted, 1);
    collect();
    ASSERT_EQ(delivery.batches.size(), 2u);
    EXPECT_EQ((*delivery.batches[0].payload)["scopes"][0]["values"][0]["id"], "meter");
    EXPECT_EQ((*delivery.batches[1].payload)["scopes"][0]["values"][0]["id"], "other");
}

TEST_F(ObservationService, PredictionReservesProbeIdsAndKeepsOtherScopesActive)
{
    auto current = interest("a", "row-1"), next = interest("b", "row-2"), other = interest("c", "other-panel");
    ASSERT_TRUE(reconcile("other", "panel", {other}).error.empty());
    ASSERT_TRUE(reconcile("a", "panel", {current}).error.empty());
    auto const active = graph.accepted;
    auto const compiles = graph.compilations;
    ASSERT_TRUE(reconcile("a", "panel", {current}, {next}).error.empty());
    EXPECT_EQ(graph.accepted, active);
    EXPECT_EQ(graph.compilations, compiles);
    ASSERT_TRUE(graph.prediction);
    auto const prepared = *graph.prediction;
    ASSERT_EQ(prepared.probes.size(), 2u);
    EXPECT_EQ(prepared.probes[0], active.probes[0]);
    EXPECT_EQ(prepared.probes[1].source, next.source);

    ASSERT_TRUE(reconcile("a", "panel", {next}).error.empty());
    EXPECT_EQ(graph.accepted, prepared);
    EXPECT_FALSE(graph.prediction);
    ASSERT_TRUE(reconcile("a", "panel", {next}, {current}).error.empty());
    ASSERT_TRUE(graph.prediction);
    ASSERT_TRUE(reconcile("a", "panel", {}).error.empty());
    EXPECT_FALSE(graph.prediction);
    ASSERT_EQ(graph.accepted.probes.size(), 1u);
    EXPECT_EQ(graph.accepted.probes[0], active.probes[0]);
}

TEST_F(ObservationService, SharesOnlyMatchingChannelsAndOperationsAcrossClients)
{
    auto left = interest("left"); left.source.channel = 0;
    auto right = interest("right"); right.source.channel = 1;
    ASSERT_TRUE(reconcile("a", "lane", {left, right}).error.empty());
    ASSERT_EQ(graph.accepted.probes.size(), 2u);
    left.id = "shared-left";
    ASSERT_TRUE(reconcile("b", "lane", {left}).error.empty());
    EXPECT_EQ(graph.compilations, 1u);
    right.id = "right-value";
    right.operation = "value";
    ASSERT_TRUE(reconcile("b", "lane", {left, right}).error.empty());
    ASSERT_EQ(graph.accepted.probes.size(), 3u);
    EXPECT_EQ(graph.compilations, 2u);
}

TEST_F(ObservationService, DeliveryOwnsItsDeadlineAndSkipsMissedFrames)
{
    ASSERT_TRUE(reconcile("a", "lane", {interest()}).error.empty());
    auto const interval = std::chrono::microseconds(33333);
    executor.publish(graph.accepted, 1);
    auto const deadline = observations.collect(now);
    ASSERT_EQ(delivery.batches.size(), 1u);
    EXPECT_EQ(deadline, now + interval);

    delivery.batches.clear();
    executor.publish(graph.accepted, 1);
    executor.frame->values[0].value.store(1.0f);
    EXPECT_EQ(observations.collect(deadline - std::chrono::microseconds(1)), deadline);
    EXPECT_TRUE(delivery.batches.empty());
    EXPECT_EQ(observations.collect(deadline), deadline + interval);
    EXPECT_EQ(delivery.batches.size(), 1u);

    auto const delayed = deadline + std::chrono::seconds(2);
    executor.publish(graph.accepted, 1);
    executor.frame->values[0].value.store(2000.0f);
    delivery.batches.clear();
    EXPECT_EQ(observations.collect(delayed), delayed + interval);
    EXPECT_EQ(delivery.batches.size(), 1u);
    delivery.batches.clear();
    executor.publish(graph.accepted, 1);
    executor.frame->values[0].value.store(2001.0f);
    observations.collect(delayed + std::chrono::milliseconds(1));
    EXPECT_TRUE(delivery.batches.empty());
}

TEST_F(ObservationService, CoalescedClientBatchContainsEveryScopeAndInvalidatesAfterActivation)
{
    ASSERT_TRUE(reconcile("a", "first", {interest("first")}).error.empty());
    ASSERT_TRUE(reconcile("a", "second", {interest("second")}).error.empty());
    executor.publish(graph.accepted, 10);
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    EXPECT_EQ((*delivery.batches.front().payload)["scopes"].size(), 2u);
    delivery.batches.clear();
    executor.publish(graph.accepted, 11, false);
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    auto const& payload = *delivery.batches.front().payload;
    EXPECT_EQ(payload["activationId"], "11");
    for (auto const& scope : payload["scopes"]) EXPECT_EQ(scope["values"][0]["available"], false);
    delivery.batches.clear();
    executor.publish(graph.accepted, 10);
    collect();
    EXPECT_TRUE(delivery.batches.empty());
}

TEST_F(ObservationService, DisconnectAndLeaseExpiryReleaseTheSharedPlanOnTheControlPath)
{
    ASSERT_TRUE(reconcile("a", "first", {interest()}).error.empty());
    SocketRpcClientDisconnectedRequest disconnect{.client_id = "a"};
    in_event_source([&] { observations.handle_disconnect(disconnect); });
    EXPECT_EQ(graph.compilations, 1u);
    collect();
    EXPECT_EQ(graph.compilations, 2u);
    EXPECT_TRUE(graph.accepted.probes.empty());
    ASSERT_TRUE(reconcile("b", "second", {interest()}).error.empty());
    now += std::chrono::seconds(31);
    collect();
    EXPECT_TRUE(graph.accepted.probes.empty());
}

TEST_F(ObservationService, RemovingOnlyAPredictionDefersTheNextGraphRead)
{
    auto const current = interest("current");
    ASSERT_TRUE(reconcile("a", "lane", {current}, {interest("next", "row-2")}).error.empty());
    ASSERT_TRUE(reconcile("b", "lane", {current}).error.empty());
    ASSERT_TRUE(graph.prediction);

    SocketRpcClientDisconnectedRequest disconnect{.client_id = "a"};
    observations.handle_disconnect(disconnect);
    collect();
    EXPECT_FALSE(graph.prediction);
    EXPECT_EQ(graph.compilations, 1u);

    executor.publish(graph.accepted, 1);
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    EXPECT_EQ(delivery.batches.front().client_id, "b");
}

TEST_F(ObservationService, CombinedLimitRejectsTheNewScopeWithoutRetainingIt)
{
    for (int scope = 0; scope < 4; ++scope) {
        std::vector<ObservationInterest> interests;
        for (int n = 0; n < 64; ++n) interests.push_back(interest(std::to_string(n), std::to_string(scope * 64 + n)));
        ASSERT_TRUE(reconcile("a", std::to_string(scope), std::move(interests)).error.empty());
    }
    EXPECT_FALSE(reconcile("a", "rejected", {interest("overflow", "overflow")}).error.empty());
    EXPECT_EQ(graph.compilations, 4u);
    EXPECT_TRUE(reconcile("a", "0", {}).error.empty());
    EXPECT_EQ(graph.accepted.probes.size(), 192u);
}
} // namespace iv::test::observation_tests

namespace iv::test::observation_tests {
TEST_F(ObservationService, RoutesRequireReadPermissionAndBindTheConnectionIdentityWithoutAProjectRevision)
{
    ProtocolCaller caller;
    auto protocol = observation_test_protocol::bind(caller, observations);
    SocketRpcRouteRegistry routes;
    auto request = parse_socket_rpc_request(R"({"id":1,"method":"observations.reconcile","params":{"clientId":"forged","scopeId":"lane","interests":[{"id":"out","source":{"node":{"instanceId":"node","path":[]},"kind":"output","member":"out"},"operation":"level"}]}})");
    std::string capability;
    bool checked = false;
    auto result = in_event_source([&] { return routes.dispatch(request, {
        .client_id = "assigned",
        .authorize = [&](auto const& value) { capability = value; },
        .before_invoke = [&](bool mutation) { checked = true; EXPECT_FALSE(mutation); },
    }); });
    EXPECT_TRUE(checked);
    EXPECT_EQ(capability, "project.read");
    EXPECT_FALSE(result.mutates_session);
    EXPECT_TRUE(Json::parse(result.response).contains("result")) << result.response;
    executor.publish(graph.accepted, 1);
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    EXPECT_EQ(delivery.batches[0].client_id, "assigned");
    request.method = "observations.describe"; // The two payload variants are distinct.
    EXPECT_THROW((void)in_event_source([&] { return routes.dispatch(request, {}); }), std::logic_error);
}
} // namespace iv::test::observation_tests


namespace iv::test::observation_tests {
TEST_F(ObservationService, ObservationDeliveryFollowsExecutionWhileNewConfigurationWaits)
{
    ASSERT_TRUE(reconcile("a", "lane", {interest()}).error.empty());
    executor.publish(graph.accepted, 1);
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    EXPECT_EQ((*delivery.batches.back().payload)["scopes"][0]["values"][0]["available"], true);
    delivery.batches.clear();
    graph.generation = 5;
    executor.publish(graph.accepted, 2);
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    EXPECT_EQ((*delivery.batches.back().payload)["generation"], "0");
    EXPECT_EQ((*delivery.batches.back().payload)["scopes"][0]["values"][0]["available"], true);
    delivery.batches.clear();
    graph.active_generation = 4;
    graph.execution_generation = 9;
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    EXPECT_EQ((*delivery.batches.back().payload)["generation"], "4");
    EXPECT_EQ((*delivery.batches.back().payload)["executionGeneration"], "9");
    EXPECT_EQ((*delivery.batches.back().payload)["scopes"][0]["values"][0]["available"], false);
    EXPECT_EQ((*delivery.batches.back().payload)["scopes"][0]["values"][0]["pending"], true);
    delivery.batches.clear();
    executor.publish(graph.accepted, 3);
    executor.frame->project_generation = 9;
    executor.frame->configured_generation = 4;
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    EXPECT_EQ((*delivery.batches.back().payload)["scopes"][0]["values"][0]["available"], true);
    EXPECT_FALSE((*delivery.batches.back().payload)["scopes"][0]["values"][0].contains("pending"));

    // Actual invalidation after cutover is different from waiting for it.
    delivery.batches.clear();
    executor.publish(graph.accepted, 4, false);
    executor.frame->project_generation = 9;
    executor.frame->configured_generation = 4;
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    auto const& invalid = (*delivery.batches.back().payload)["scopes"][0]["values"][0];
    EXPECT_EQ(invalid["available"], false);
    EXPECT_FALSE(invalid.contains("pending"));

    // Failure of the desired configuration leaves observation delivery on the
    // same active execution. With no new readings, there is no update to send.
    delivery.batches.clear();
    graph.generation.reset();
    collect();
    EXPECT_TRUE(delivery.batches.empty());

    executor.publish(graph.accepted, 5);
    executor.frame->project_generation = 9;
    executor.frame->configured_generation = 4;
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    auto const& frame = *delivery.batches.back().payload;
    EXPECT_EQ(frame["executionGeneration"], "9");
    EXPECT_EQ(frame["scopes"][0]["values"][0]["available"], true);
}

TEST_F(ObservationService, WindowScopesShareCancelableQueriesWithoutCreatingLiveProbes)
{
    auto window = interest("wave");
    window.operation = "waveform";
    window.window = ObservationWindow{GlobalIndex::from_signed(-2), 12, 12};
    ASSERT_TRUE(reconcile("a", "lane", {window}).error.empty());
    auto shared = window; shared.id = "other";
    ASSERT_TRUE(reconcile("b", "lane", {shared}).error.empty());
    EXPECT_EQ(graph.compilations, 0u);
    collect();
    ASSERT_EQ(executor.windows.size(), 1u);
    auto query = executor.windows.front();
    query->result.status = ObservationWindowResult::Status::ready;
    query->result.sample_rate = 48000;
    query->result.regions.push_back({.range = {GlobalIndex::from_signed(0), 3}});
    ObservationCaptureState zero; zero.store(0.0f);
    query->result.regions.back().values.emplace_back(GlobalIndex::from_signed(0), zero);
    query->complete.store(true, std::memory_order_release);
    collect();
    ASSERT_EQ(delivery.batches.size(), 2u);
    ObservationDeliveryState connection;
    auto const payload = connection.payload_for_write(delivery.batches[0]);
    auto const& result = payload["scopes"]["lane"]["windows"]["wave"];
    EXPECT_EQ(result["status"], "ready");
    auto const& region = result["body"]["regions"][0];
    EXPECT_EQ(region["points"][0]["value"]["value"], 0);
    EXPECT_EQ(region["begin"], "0");
    EXPECT_EQ(region["length"], "3");
    EXPECT_EQ(graph.compilations, 0u);

    window.window->begin = GlobalIndex::from_signed(30);
    ASSERT_TRUE(reconcile("a", "lane", {window}).error.empty());
    ASSERT_TRUE(reconcile("b", "lane", {}).error.empty());
    collect();
    EXPECT_TRUE(query->canceled.load(std::memory_order_acquire));
    ASSERT_EQ(executor.windows.size(), 2u);
    auto pending = executor.windows.back();
    EXPECT_EQ(pending->window.begin.signed_value(), 30);
    auto const& moved = (*delivery.batches[0].payload)["scopes"][0]["windows"][0];
    EXPECT_EQ(moved["window"]["begin"], "30");
    EXPECT_EQ(moved["status"], "pending");
    EXPECT_EQ(graph.compilations, 0u);
    ASSERT_TRUE(reconcile("a", "lane", {}).error.empty());
    collect();
    EXPECT_TRUE(pending->canceled.load(std::memory_order_acquire));
}

TEST_F(ObservationService, VisibleWindowPromotesSharedPrefetchWithoutRestartingIt)
{
    auto window = interest("prefetch");
    window.operation = "waveform";
    window.window = ObservationWindow{GlobalIndex{}, 64, 16};
    window.prefetch = true;
    ASSERT_TRUE(reconcile("a", "lane", {window}).error.empty());
    collect();
    ASSERT_EQ(executor.windows.size(), 1u);
    auto const query = executor.windows.front();
    EXPECT_TRUE(query->prefetch.load());

    auto visible = window;
    visible.id = "visible";
    visible.prefetch = false;
    ASSERT_TRUE(reconcile("b", "lane", {visible}).error.empty());
    collect();
    EXPECT_EQ(executor.windows.size(), 1u);
    EXPECT_FALSE(query->prefetch.load());
    EXPECT_FALSE(query->canceled.load());

    ASSERT_TRUE(reconcile("b", "lane", {}).error.empty());
    collect();
    EXPECT_EQ(executor.windows.size(), 1u);
    EXPECT_TRUE(query->prefetch.load());
}

TEST_F(ObservationService, OnlyNewerExecutionDataInvalidatesWindowContents)
{
    auto window = interest(); window.operation = "waveform";
    window.window = ObservationWindow{GlobalIndex{}, 32, 8};
    ASSERT_TRUE(reconcile("a", "lane", {window}).error.empty());
    executor.data_version = 1;
    collect();
    ASSERT_EQ(executor.windows.size(), 1u);
    auto query = executor.windows.front();
    query->result.status = ObservationWindowResult::Status::ready;
    query->result.data_version = 2;
    query->complete.store(true, std::memory_order_release);
    collect();
    EXPECT_EQ((*delivery.batches[0].payload)["scopes"][0]["windows"][0]["status"], "ready");
    EXPECT_EQ((*delivery.batches[0].payload)["scopes"][0]["windows"][0]["outdated"], false);
    collect(); collect(); collect();
    EXPECT_FALSE(query->canceled.load(std::memory_order_acquire));
    EXPECT_EQ(executor.windows.size(), 1u);

    executor.data_version = 3;
    collect();
    EXPECT_EQ((*delivery.batches[0].payload)["dataVersion"], "3");
    EXPECT_EQ((*delivery.batches[0].payload)["scopes"][0]["windows"][0]["status"], "ready");
    EXPECT_EQ((*delivery.batches[0].payload)["scopes"][0]["windows"][0]["outdated"], true);
    collect(); collect(); collect();
    EXPECT_TRUE(query->canceled.load(std::memory_order_acquire));
    EXPECT_EQ(executor.windows.size(), 2u);
    EXPECT_EQ(graph.compilations, 0u);
}

TEST_F(ObservationService, PrefetchRefreshesLessOftenThanVisibleWindows)
{
    auto visible = interest("visible");
    visible.operation = "waveform";
    visible.window = ObservationWindow{GlobalIndex{}, 64, 32};
    auto prefetch = visible;
    prefetch.id = "nearby";
    prefetch.window->points = 4;
    prefetch.prefetch = true;
    executor.data_version = 1;
    ASSERT_TRUE(reconcile("a", "lane", {visible, prefetch}).error.empty());
    collect();
    ASSERT_EQ(executor.windows.size(), 2u);
    auto const visible_query = executor.windows[0];
    auto const prefetch_query = executor.windows[1];
    for (auto const& query : {visible_query, prefetch_query}) {
        query->result.status = ObservationWindowResult::Status::ready;
        query->result.data_version = 1;
        query->complete.store(true, std::memory_order_release);
    }

    executor.data_version = 2;
    for (int i = 0; i < 6; ++i) collect();
    EXPECT_TRUE(visible_query->canceled.load());
    EXPECT_FALSE(prefetch_query->canceled.load());
    ASSERT_EQ(executor.windows.size(), 3u);
    EXPECT_FALSE(executor.windows.back()->prefetch.load());

    now += std::chrono::seconds(2);
    collect();
    EXPECT_TRUE(prefetch_query->canceled.load());
    ASSERT_EQ(executor.windows.size(), 4u);
    EXPECT_TRUE(executor.windows.back()->prefetch.load());
    EXPECT_EQ(graph.compilations, 0u);
}

TEST_F(ObservationService, ExecutionActivationReplacesInflightWindowsBeforeTheDeliveryDeadline)
{
    auto window = interest("wave");
    window.operation = "waveform";
    window.window = ObservationWindow{GlobalIndex{}, 32, 8};
    graph.generation = 5;
    graph.active_generation = 4;
    graph.execution_generation = 8;
    ASSERT_TRUE(reconcile("a", "lane", {window}).error.empty());
    auto const deadline = observations.collect(now);
    ASSERT_EQ(executor.windows.size(), 1u);
    auto const previous = executor.windows.front();

    graph.execution_generation = 9;
    delivery.batches.clear();
    EXPECT_EQ(observations.collect(now + std::chrono::milliseconds(1), true), deadline);
    EXPECT_TRUE(delivery.batches.empty());
    ASSERT_EQ(executor.windows.size(), 2u);
    EXPECT_TRUE(previous->canceled.load());
    auto const current = executor.windows.back();
    EXPECT_EQ(current->configured_generation, 4u);
    EXPECT_EQ(current->execution_generation, 9u);
    current->result.status = ObservationWindowResult::Status::ready;
    current->result.execution_generation = 9;
    current->complete.store(true, std::memory_order_release);
    observations.collect(deadline);
    ASSERT_FALSE(delivery.batches.empty());
    auto const& frame = *delivery.batches.back().payload;
    EXPECT_EQ(frame["generation"], "4");
    EXPECT_EQ(frame["executionGeneration"], "9");
    EXPECT_EQ(frame["scopes"][0]["windows"][0]["status"], "ready");
}

TEST_F(ObservationService, WindowBodiesFollowContentIndependentlyOfRefreshStatusAndDataVersion)
{
    auto window = interest("wave");
    window.operation = "waveform";
    window.window = ObservationWindow{GlobalIndex{}, 32, 8};
    ASSERT_TRUE(reconcile("a", "lane", {window}).error.empty());
    auto complete = [](ObservationWindowQuery& query, std::uint64_t version, float value) {
        query.result.status = ObservationWindowResult::Status::ready;
        query.result.data_version = version;
        query.result.regions.push_back({.range = {GlobalIndex{}, 32}});
        ObservationCaptureState sample;
        sample.store(value);
        query.result.regions.back().values.emplace_back(GlobalIndex{}, sample);
        query.complete.store(true, std::memory_order_release);
    };
    ObservationDeliveryState connection;
    executor.data_version = 1;
    collect();
    ASSERT_EQ(executor.windows.size(), 1u);
    complete(*executor.windows.back(), 1, 0.5f);
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    auto const first = connection.payload_for_write(delivery.batches.front());
    EXPECT_TRUE(first["scopes"]["lane"]["windows"]["wave"].contains("body"));

    executor.data_version = 2;
    collect();
    auto const outdated = connection.payload_for_write(delivery.batches.front());
    EXPECT_EQ(outdated["scopes"]["lane"]["windows"]["wave"], (Json{{"outdated", true}}));
    now += std::chrono::milliseconds(300);
    collect();
    ASSERT_EQ(executor.windows.size(), 2u);
    executor.windows.back()->result.status = ObservationWindowResult::Status::pending_execution;
    executor.windows.back()->complete.store(true, std::memory_order_release);
    delivery.batches.clear();
    collect();
    EXPECT_TRUE(delivery.batches.empty());

    now += std::chrono::milliseconds(300);
    collect();
    ASSERT_EQ(executor.windows.size(), 3u);
    complete(*executor.windows.back(), 2, 0.5f);
    collect();
    ASSERT_EQ(delivery.batches.size(), 1u);
    auto const refreshed = connection.payload_for_write(delivery.batches.front());
    auto const& changes = refreshed["scopes"]["lane"]["windows"]["wave"];
    EXPECT_EQ(changes["outdated"], false);
    EXPECT_EQ(changes["dataVersion"], "2");
    EXPECT_FALSE(changes.contains("body"));

    executor.data_version = 3;
    collect();
    now += std::chrono::milliseconds(300);
    collect();
    ASSERT_EQ(executor.windows.size(), 4u);
    complete(*executor.windows.back(), 3, -0.5f);
    collect();
    auto const changed = connection.payload_for_write(delivery.batches.front());
    EXPECT_EQ(changed["scopes"]["lane"]["windows"]["wave"]["body"]["regions"][0]["points"][0]["value"]["value"], -0.5);

    ASSERT_TRUE(reconcile("a", "lane", {}).error.empty());
    collect();
    auto const removed = connection.payload_for_write(delivery.batches.front());
    EXPECT_TRUE(removed["scopes"]["lane"].is_null());
}

TEST(ObservationDelivery, CoalescedFramesCanOnlyReferToBodiesFromTheLastWrite)
{
    ObservationDeliveryState connection;
    auto body = std::make_shared<ObservationWindowBody const>(ObservationWindowBody{"1", Json{{"regions", Json::array()}}});
    auto frame = [](bool outdated) {
        return Json{{"generation", "0"}, {"scopes", Json::array({{
            {"scopeId", "lane"}, {"values", Json::array()}, {"windows", Json::array({{
                {"id", "wave"}, {"status", "ready"}, {"bodyId", "1"}, {"outdated", outdated}}})}}})}};
    };
    auto pending = ObservationDelivery{.client_id = "a", .payload = std::make_shared<Json const>(frame(false)), .window_bodies = {body}};
    auto selected = pending;
    selected.payload = std::make_shared<Json const>(frame(true));
    auto const first = connection.payload_for_write(selected);
    EXPECT_EQ(first["snapshot"], true);
    EXPECT_TRUE(first["scopes"]["lane"]["windows"]["wave"].contains("body"));
    auto const unchanged = connection.payload_for_write(selected);
    EXPECT_TRUE(unchanged["scopes"].empty());
    auto const delta = connection.payload_for_write(pending);
    EXPECT_EQ(delta["snapshot"], false);
    EXPECT_EQ(delta["scopes"]["lane"]["windows"]["wave"], (Json{{"outdated", false}}));
}

}


TEST(Observations, WindowProjectionPreservesGapsOverlapsAndUnavailableValues)
{
    using namespace iv;
    auto index = [](std::int64_t value) { return GlobalIndex::from_signed(value); };
    std::array<float, 3> first{0, .25f, .5f};
    std::array<float, 2> second{1, 2}, third{-1, -2};
    std::array logical{GlobalIndexRange{index(0), 3}, GlobalIndexRange{index(10), 2}, GlobalIndexRange{index(10), 2}};
    std::array views{
        ContinuousRegionStorageView{.storage = reinterpret_cast<std::byte*>(first.data()), .range = logical[0]},
        ContinuousRegionStorageView{.storage = reinterpret_cast<std::byte*>(second.data()), .range = logical[1]},
        ContinuousRegionStorageView{.storage = reinterpret_cast<std::byte*>(third.data()), .range = logical[2]},
    };
    IndexCoverage coverage{logical[0], logical[1]};
    std::array<std::size_t, 1> channels{0};
    BackgroundSampleReadView view{.coverage_value = &coverage, .regions = views,
        .layout = {.channel_type = ChannelTypeId::mono}, .channels = channels};
    ObservationWindowQuery query;
    query.operation = &observation_details::sample_operations.back();
    ASSERT_EQ(query.operation->id, "waveform");
    EXPECT_EQ(query.operation->capture_block, nullptr);
    auto positions = observation_window_positions({index(0), 12}, 12);
    project_observation_window(query, {index(0), 12}, positions, logical, &view);
    ASSERT_EQ(query.result.regions.size(), 3u);
    EXPECT_EQ(query.result.regions[0].values.size(), 3u);
    EXPECT_FLOAT_EQ(query.result.regions[0].values[0].second.load<float>(), 0);
    EXPECT_FLOAT_EQ(query.result.regions[1].values[0].second.load<float>(), 1);
    EXPECT_FLOAT_EQ(query.result.regions[2].values[0].second.load<float>(), -1);
    EXPECT_EQ(query.result.regions[1].range.begin, index(10));

    // Covered data not prepared is unavailable, never a fabricated zero.
    views[0].storage = nullptr;
    query.result.regions.clear();
    project_observation_window(query, {index(0), 12}, positions, logical, &view);
    EXPECT_FALSE(query.result.regions[0].values[0].second.captured);
    EXPECT_TRUE(query.result.regions[1].values[0].second.captured);

    query.result.regions.clear();
    view.regions = std::span{views}.last(1);
    std::array<std::size_t, 1> prepared{2};
    project_observation_window(query, {index(0), 12}, positions, logical, &view,
        false, std::span<std::size_t const>{prepared});
    ASSERT_EQ(query.result.regions.size(), 3u);
    EXPECT_FALSE(query.result.regions[1].values[0].second.captured);
    EXPECT_TRUE(query.result.regions[2].values[0].second.captured);
    EXPECT_FLOAT_EQ(query.result.regions[2].values[0].second.load<float>(), -1);
}

TEST(Observations, WindowRpcPreservesSignedIndicesAndBoundsTheRequest)
{
    auto message = nlohmann::ordered_json::parse(R"({"jsonrpc":"2.0","id":1,"method":"observations.reconcile","params":{"scopeId":"viewport","interests":[{"id":"wave","source":{"node":{"instanceId":"node","path":[]},"kind":"output","member":"out"},"operation":"waveform","window":{"begin":"-50","length":"1000000000","points":1024}}]}})");
    auto parsed = iv::parse_socket_rpc_request(message.dump());
    auto const* request = std::get_if<iv::ReconcileObservationsRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    ASSERT_EQ(request->interests.size(), 1u);
    ASSERT_TRUE(request->interests[0].window);
    EXPECT_EQ(request->interests[0].window->begin.signed_value(), -50);
    EXPECT_EQ(request->interests[0].window->length, 1000000000u);
    EXPECT_EQ(request->interests[0].window->points, 1024u);
    message["params"]["interests"][0]["prefetch"] = true;
    auto const prefetched = iv::parse_socket_rpc_request(message.dump());
    EXPECT_TRUE(std::get<iv::ReconcileObservationsRequest>(prefetched.payload).interests[0].prefetch);
    message["params"]["interests"][0]["prefetch"] = "true";
    EXPECT_THROW(iv::parse_socket_rpc_request(message.dump()), std::invalid_argument);
    message["params"]["interests"][0]["prefetch"] = true;
    message["params"]["interests"][0]["window"]["points"] = 1025u;
    EXPECT_THROW(iv::parse_socket_rpc_request(message.dump()), std::invalid_argument);
    message["params"]["interests"][0]["window"]["points"] = 1u;
    message["params"]["interests"][0]["window"]["length"] = "-1";
    EXPECT_THROW(iv::parse_socket_rpc_request(message.dump()), std::invalid_argument);
}
