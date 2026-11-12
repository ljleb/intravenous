#include <intravenous/bridge.h>
#include <intravenous/linker_event.h>

#include <gtest/gtest.h>

#include <optional>
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
IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_echo_event);
IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_echo_reply_event);
IV_DECLARE_LINKER_EVENT(VoidEvent, iv_test_propagation_open_transaction_event);

struct RootEndpoint {
    int echo_replies = 0;

    void handle_echo_reply() { ++echo_replies; }
};

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
        IV_EVENT_SOURCE(NestedSourceModule, "nested") {
            IV_INVOKE_LINKER_EVENT(iv_test_propagation_direct_event);
        }
    }
};

struct ThrowingModule {
    void handle_root()
    {
        throw std::runtime_error("propagation test failure");
    }
};

// Replies to the root event with an event that RootEndpoint handles.
struct EchoModule {
    void handle_echo()
    {
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_echo_reply_event);
    }
};

// Opens a transaction through one event; each later run reaches SinkModule
// as part of that entry.
struct TransactionModule {
    std::optional<iv::EventModuleVisit> visit{};

    void handle_open() { visit = iv::EventModuleVisit::current<TransactionModule>(); }

    void run() const
    {
        iv::EventModuleVisitScope entry(*visit);
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_direct_event);
    }
};

IV_DECLARE_BRIDGE(test_root_left_bridge, RootEndpoint, LeftModule);
IV_DECLARE_BRIDGE(test_root_right_bridge, RootEndpoint, RightModule);
IV_DECLARE_BRIDGE(test_left_sink_bridge, LeftModule, SinkModule);
IV_DECLARE_BRIDGE(test_right_sink_bridge, RightModule, SinkModule);
IV_DECLARE_BRIDGE(test_direct_sink_bridge, RootEndpoint, SinkModule);
IV_DECLARE_BRIDGE(test_root_nested_bridge, RootEndpoint, NestedSourceModule);
IV_DECLARE_BRIDGE(test_root_throwing_bridge, RootEndpoint, ThrowingModule);
IV_DECLARE_BRIDGE(test_root_echo_bridge, RootEndpoint, EchoModule);
IV_DECLARE_BRIDGE(test_root_transaction_bridge, RootEndpoint, TransactionModule);
IV_DECLARE_BRIDGE(test_transaction_sink_bridge, TransactionModule, SinkModule);

IV_DEFINE_BRIDGE(test_root_left_bridge)
IV_DEFINE_BRIDGE(test_root_right_bridge)
IV_DEFINE_BRIDGE(test_left_sink_bridge)
IV_DEFINE_BRIDGE(test_right_sink_bridge)
IV_DEFINE_BRIDGE(test_direct_sink_bridge)
IV_DEFINE_BRIDGE(test_root_nested_bridge)
IV_DEFINE_BRIDGE(test_root_throwing_bridge)
IV_DEFINE_BRIDGE(test_root_echo_bridge)
IV_DEFINE_BRIDGE(test_root_transaction_bridge)
IV_DEFINE_BRIDGE(test_transaction_sink_bridge)

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
IV_SUBSCRIBE_LINKER_EVENT(
    test_root_echo_bridge,
    iv_test_propagation_echo_event,
    &EchoModule::handle_echo);
IV_SUBSCRIBE_LINKER_EVENT(
    test_root_echo_bridge,
    iv_test_propagation_echo_reply_event,
    &RootEndpoint::handle_echo_reply);
IV_SUBSCRIBE_LINKER_EVENT(
    test_root_transaction_bridge,
    iv_test_propagation_open_transaction_event,
    &TransactionModule::handle_open);
IV_SUBSCRIBE_LINKER_EVENT(
    test_transaction_sink_bridge,
    iv_test_propagation_direct_event,
    &SinkModule::handle_direct);

IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_root_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_left_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_right_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_direct_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_nested_source_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_throwing_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_echo_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_echo_reply_event)
IV_DEFINE_LINKER_EVENT(VoidEvent, iv_test_propagation_open_transaction_event)

void raise_from_root(char const* name, auto&& raise)
{
    IV_EVENT_SOURCE(RootEndpoint, name) {
        raise();
    }
}

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
        raise_from_root("convergence check", [] {
            IV_INVOKE_LINKER_EVENT(iv_test_propagation_root_event);
        });
        FAIL() << "expected propagation convergence to throw";
    } catch (std::logic_error const& error) {
        auto const message = std::string(error.what());
        EXPECT_NE(message.find("SinkModule"), std::string::npos);
        EXPECT_NE(message.find("RootEndpoint \"convergence check\""), std::string::npos);
        EXPECT_NE(message.find("first path"), std::string::npos);
        EXPECT_NE(message.find("second path"), std::string::npos);
    }
}

TEST(LinkerEventPropagation, OneSourceEntryIsOneTreeAcrossSeveralRaises)
{
    RootEndpoint root;
    SinkModule sink;
    auto scope = test_direct_sink_bridge::bind(root, sink);

    EXPECT_THROW(
        raise_from_root("two raises", [] {
            IV_INVOKE_LINKER_EVENT(iv_test_propagation_direct_event);
            IV_INVOKE_LINKER_EVENT(iv_test_propagation_direct_event);
        }),
        std::logic_error);
    EXPECT_EQ(sink.calls, 1);
}

TEST(LinkerEventPropagation, SeparateSourceEntriesMayVisitTheSameModule)
{
    RootEndpoint root;
    SinkModule sink;
    auto scope = test_direct_sink_bridge::bind(root, sink);

    auto const raise_direct = [] {
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_direct_event);
    };
    EXPECT_NO_THROW(raise_from_root("first", raise_direct));
    EXPECT_NO_THROW(raise_from_root("second", raise_direct));
    EXPECT_EQ(sink.calls, 2);
}

TEST(LinkerEventPropagation, RejectsATreeThatReturnsToItsSourceModule)
{
    RootEndpoint root;
    EchoModule echo;
    auto scope = test_root_echo_bridge::bind(root, echo);

    try {
        raise_from_root("echo", [] {
            IV_INVOKE_LINKER_EVENT(iv_test_propagation_echo_event);
        });
        FAIL() << "expected the echo to re-enter its source module";
    } catch (std::logic_error const& error) {
        auto const message = std::string(error.what());
        EXPECT_NE(message.find("re-entered module"), std::string::npos);
        EXPECT_NE(message.find("RootEndpoint"), std::string::npos);
    }
    EXPECT_EQ(root.echo_replies, 0);
}

TEST(LinkerEventPropagation, RejectsAnEventRaisedOutsideASource)
{
    RootEndpoint root;
    SinkModule sink;
    auto scope = test_direct_sink_bridge::bind(root, sink);

    try {
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_direct_event);
        FAIL() << "expected a raise outside an event source to throw";
    } catch (std::logic_error const& error) {
        EXPECT_NE(
            std::string(error.what()).find("outside an event source"),
            std::string::npos);
    }
    EXPECT_EQ(sink.calls, 0);
}

TEST(LinkerEventPropagation, SourceContextIsRestoredAfterSubscriberThrows)
{
    RootEndpoint root;
    ThrowingModule throwing;
    {
        auto scope = test_root_throwing_bridge::bind(root, throwing);
        EXPECT_THROW(
            raise_from_root("throwing", [] {
                IV_INVOKE_LINKER_EVENT(iv_test_propagation_throwing_event);
            }),
            std::runtime_error);
    }

    SinkModule sink;
    auto scope = test_direct_sink_bridge::bind(root, sink);
    EXPECT_NO_THROW(raise_from_root("after throw", [] {
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_direct_event);
    }));
    EXPECT_EQ(sink.calls, 1);
}

TEST(LinkerEventPropagation, RejectsNestedSource)
{
    RootEndpoint root;
    NestedSourceModule nested;
    auto scope = test_root_nested_bridge::bind(root, nested);

    EXPECT_THROW(
        raise_from_root("outer", [] {
            IV_INVOKE_LINKER_EVENT(iv_test_propagation_nested_source_event);
        }),
        std::logic_error);
}

TEST(LinkerEventPropagation, TransactionRunsBelongToTheEntryThatOpenedThem)
{
    RootEndpoint root;
    TransactionModule transaction;
    SinkModule sink;
    auto root_transaction = test_root_transaction_bridge::bind(root, transaction);
    auto transaction_sink = test_transaction_sink_bridge::bind(transaction, sink);

    EXPECT_NO_THROW(raise_from_root("transaction", [&] {
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_open_transaction_event);
        transaction.run();
    }));
    EXPECT_EQ(sink.calls, 1);
}

TEST(LinkerEventPropagation, TransactionRunsAreStillChecked)
{
    RootEndpoint root;
    TransactionModule transaction;
    SinkModule sink;
    auto root_transaction = test_root_transaction_bridge::bind(root, transaction);
    auto transaction_sink = test_transaction_sink_bridge::bind(transaction, sink);

    EXPECT_THROW(
        raise_from_root("transaction", [&] {
            IV_INVOKE_LINKER_EVENT(iv_test_propagation_open_transaction_event);
            transaction.run();
            transaction.run();
        }),
        std::logic_error);
    EXPECT_EQ(sink.calls, 1);
}

TEST(LinkerEventPropagation, RejectsATransactionUsedAfterItsPropagation)
{
    RootEndpoint root;
    TransactionModule transaction;
    SinkModule sink;
    auto root_transaction = test_root_transaction_bridge::bind(root, transaction);
    auto transaction_sink = test_transaction_sink_bridge::bind(transaction, sink);

    raise_from_root("open", [] {
        IV_INVOKE_LINKER_EVENT(iv_test_propagation_open_transaction_event);
    });
    EXPECT_THROW(
        raise_from_root("later", [&] { transaction.run(); }),
        std::logic_error);
    EXPECT_EQ(sink.calls, 0);
}
} // namespace
