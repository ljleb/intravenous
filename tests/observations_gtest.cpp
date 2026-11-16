#include <intravenous/runtime/observation_types.h>
#include <intravenous/runtime/observations.h>
#include <intravenous/runtime/socket_rpc_routes.h>
#include <intravenous/bridge.h>
#include "event_source_test_support.h"
#include <nlohmann/json.hpp>
#include <intravenous/continuous_value.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace {
template<class T> T value_at(iv::ObservationResultData const& result, std::size_t offset)
{
    T value;
    std::memcpy(&value, result.bytes.data() + offset, sizeof(value));
    return value;
}
}

TEST(Observations, SampleLevelReadsWrappedStridedChannelsAndReportsNonfiniteValues)
{
    std::array<float, 8> interleaved{1, -2, 3, -4, 5, -6, 7, std::numeric_limits<float>::infinity()};
    std::array channels{
        iv::ObservationChannelView{reinterpret_cast<std::byte const*>(interleaved.data()), 4, 2 * sizeof(float), 1},
        iv::ObservationChannelView{reinterpret_cast<std::byte const*>(interleaved.data() + 1), 4, 2 * sizeof(float), 1},
    };
    auto const& type = iv::continuous_value_type_info(iv::ContinuousValueTypeId::sample);
    ASSERT_EQ(type.observation_operations.size(), 1u);
    auto const& operation = type.observation_operations.front();
    iv::ObservationCaptureState state;
    operation.capture_block(state, {channels, 4, 2}); // Wrapped frames 3, 0.
    auto result = operation.project(state);
    EXPECT_DOUBLE_EQ(value_at<double>(result, 0), 7);
    EXPECT_EQ(value_at<std::uint64_t>(result, 16), 3u);
    EXPECT_EQ(value_at<std::uint64_t>(result, 24), 1u);
    EXPECT_DOUBLE_EQ(value_at<double>(result, 8), std::sqrt(54.0 / 3));
    EXPECT_TRUE(iv::continuous_value_type_info(iv::ContinuousValueTypeId::fft_block).observation_operations.empty());
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
    ObservationRequirements accepted{};
    std::size_t compilations = 0;
    void apply(ObservationGraphRequest& request)
    {
        request.handled = true;
        if (request.requirements) { accepted = *request.requirements; ++compilations; }
        request.capabilities = std::make_shared<Json const>(Json::array());
    }
};
struct Executor {
    std::optional<LiveObservationFrame> frame;
    void collect(ObservationCollectRequest& request)
    { request.handled = true; request.frame = std::move(frame); frame.reset(); }
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
    ObservationProtocolRequest reconcile(std::string client, std::string scope, std::vector<ObservationInterest> interests)
    {
        ObservationProtocolRequest request{.kind = ObservationProtocolRequest::Kind::reconcile,
            .client_id = std::move(client), .scope_id = std::move(scope), .interests = std::move(interests)};
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
