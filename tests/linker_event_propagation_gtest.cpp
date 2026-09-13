#include <intravenous/bridge.h>
#include <intravenous/linker_event.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {
using VoidEvent = void (*)();

IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_root_event);
IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_left_event);
IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_right_event);
IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_direct_event);
IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_nested_source_event);
IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_throwing_event);

struct RootEndpoint {};

struct LeftModule {
    void handle_root()
    {
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_left_event);
    }
};

struct RightModule {
    void handle_root()
    {
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_right_event);
    }
};

struct SinkModule {
    int calls = 0;

    void handle_left() { ++calls; }
    void handle_right() { ++calls; }
    void handle_direct() { ++calls; }
};

struct NestedSourceModule {
    void handle_root()
    {
        IV_INVOKE_LINKER_EVENT_SOURCE(iv_test_propagation_nested_source_event);
    }
};

struct ThrowingModule {
    void handle_root()
    {
        throw std::runtime_error("propagation test failure");
    }
};

IV_DECLARE_BRIDGE(test_root_left_bridge, RootEndpoint, LeftModule);
IV_DECLARE_BRIDGE(test_root_right_bridge, RootEndpoint, RightModule);
IV_DECLARE_BRIDGE(test_left_sink_bridge, LeftModule, SinkModule);
IV_DECLARE_BRIDGE(test_right_sink_bridge, RightModule, SinkModule);
IV_DECLARE_BRIDGE(test_direct_sink_bridge, RootEndpoint, SinkModule);
IV_DECLARE_BRIDGE(test_root_nested_bridge, RootEndpoint, NestedSourceModule);
IV_DECLARE_BRIDGE(test_root_throwing_bridge, RootEndpoint, ThrowingModule);

IV_DEFINE_BRIDGE(test_root_left_bridge)
IV_DEFINE_BRIDGE(test_root_right_bridge)
IV_DEFINE_BRIDGE(test_left_sink_bridge)
IV_DEFINE_BRIDGE(test_right_sink_bridge)
IV_DEFINE_BRIDGE(test_direct_sink_bridge)
IV_DEFINE_BRIDGE(test_root_nested_bridge)
IV_DEFINE_BRIDGE(test_root_throwing_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    test_root_left_bridge,
    iv_test_propagation_root_event,
    &LeftModule::handle_root);
IV_SUBSCRIBE_LINKER_EVENT(
    test_root_right_bridge,
    iv_test_propagation_root_event,
    &RightModule::handle_root);
IV_SUBSCRIBE_LINKER_EVENT(
    test_left_sink_bridge,
    iv_test_propagation_left_event,
    &SinkModule::handle_left);
IV_SUBSCRIBE_LINKER_EVENT(
    test_right_sink_bridge,
    iv_test_propagation_right_event,
    &SinkModule::handle_right);
IV_SUBSCRIBE_LINKER_EVENT(
    test_direct_sink_bridge,
    iv_test_propagation_direct_event,
    &SinkModule::handle_direct);
IV_SUBSCRIBE_LINKER_EVENT(
    test_root_nested_bridge,
    iv_test_propagation_nested_source_event,
    &NestedSourceModule::handle_root);
IV_SUBSCRIBE_LINKER_EVENT(
    test_root_throwing_bridge,
    iv_test_propagation_throwing_event,
    &ThrowingModule::handle_root);

IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_root_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_left_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_right_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_direct_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_nested_source_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_throwing_event)

TEST(LinkerEventPropagation, RejectsSiblingConvergenceOnOneModule)
{
    RootEndpoint root;
    LeftModule left;
    RightModule right;
    SinkModule sink;
    auto root_left = test_root_left_bridge::bind(root, left);
    auto root_right = test_root_right_bridge::bind(root, right);
    auto left_sink = test_left_sink_bridge::bind(left, sink);
    auto right_sink = test_right_sink_bridge::bind(right, sink);

    try {
        IV_INVOKE_LINKER_EVENT_SOURCE(iv_test_propagation_root_event);
        FAIL() << "expected propagation convergence to throw";
    } catch (std::logic_error const& error) {
        auto const message = std::string(error.what());
        EXPECT_NE(message.find("SinkModule"), std::string::npos);
        EXPECT_NE(message.find("first path"), std::string::npos);
        EXPECT_NE(message.find("second path"), std::string::npos);
    }
}

TEST(LinkerEventPropagation, SeparateSourcesMayVisitTheSameModule)
{
    RootEndpoint root;
    SinkModule sink;
    auto scope = test_direct_sink_bridge::bind(root, sink);

    EXPECT_NO_THROW(IV_INVOKE_LINKER_EVENT_SOURCE(iv_test_propagation_direct_event));
    EXPECT_NO_THROW(IV_INVOKE_LINKER_EVENT_SOURCE(iv_test_propagation_direct_event));
    EXPECT_EQ(sink.calls, 2);
}

TEST(LinkerEventPropagation, SourceContextIsRestoredAfterSubscriberThrows)
{
    RootEndpoint root;
    ThrowingModule throwing;
    {
        auto scope = test_root_throwing_bridge::bind(root, throwing);
        EXPECT_THROW(
            IV_INVOKE_LINKER_EVENT_SOURCE(iv_test_propagation_throwing_event),
            std::runtime_error);
    }

    SinkModule sink;
    auto scope = test_direct_sink_bridge::bind(root, sink);
    EXPECT_NO_THROW(IV_INVOKE_LINKER_EVENT_SOURCE(iv_test_propagation_direct_event));
    EXPECT_EQ(sink.calls, 1);
}

TEST(LinkerEventPropagation, RejectsNestedSourceEvent)
{
    RootEndpoint root;
    NestedSourceModule nested;
    auto scope = test_root_nested_bridge::bind(root, nested);

    // The outer event is an ordinary source for this test. Once its subscriber is
    // executing, attempting to start another source on the same thread is invalid.
    EXPECT_THROW(
        IV_INVOKE_LINKER_EVENT_SOURCE(iv_test_propagation_nested_source_event),
        std::logic_error);
}
} // namespace
