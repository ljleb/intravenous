#include <intravenous/dsl.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>

template<typename T>
concept CompleteType = requires {
    sizeof(T);
};

static_assert(!CompleteType<iv::NodeLayoutBuilder>);
static_assert(!CompleteType<iv::NodeStorage>);

namespace {
    struct ModuleDefinedNode {
        struct State {
            int value = 0;
        };

        void declare(iv::DeclarationContext<ModuleDefinedNode> const& ctx) const
        {
            (void)ctx;
        }

        void initialize(iv::InitializationContext<ModuleDefinedNode> const& ctx)
            const
        {
            ++ctx.state().value;
        }
    };

    static_assert(iv::details::has_declare<ModuleDefinedNode>);
    static_assert(iv::details::has_initialize<ModuleDefinedNode>);

    struct ReplacingLeafNode {
        struct ReplaceEvent {};

        int successor_value = 0;

        void on_message(
            iv::NodeMessageContext<ReplacingLeafNode>& context,
            ReplaceEvent const&) const
        {
            context.replace<"iv.test.successor">(successor_value);
        }
    };

    struct AddressingModule {
        struct SendEvent {
            int value = 0;
        };
    };
}

TEST(ModuleContextBoundary, DslKeepsLayoutAndStorageIncomplete)
{
    SUCCEED();
}

TEST(ModuleContextBoundary, LeafReplacementReadsConstructionStateThroughThis)
{
    struct Capture {
        iv::NodeAddress address{};
        std::string definition_id{};
        std::string arguments_json{};
    } capture;
    iv::NodeMessageContextOperations const operations{
        .context = &capture,
        .replace = [](
            void* opaque,
            iv::NodeAddress const& address,
            std::string definition_id,
            std::string arguments_json) {
            auto& result = *static_cast<Capture*>(opaque);
            result.address = address;
            result.definition_id = std::move(definition_id);
            result.arguments_json = std::move(arguments_json);
        },
    };
    iv::NodeAddress const address{.instance_id = "instance:leaf"};
    iv::NodeMessageContext<ReplacingLeafNode> context(operations, address);
    ReplacingLeafNode const node{.successor_value = 41};

    node.on_message(context, ReplacingLeafNode::ReplaceEvent{});

    EXPECT_EQ(capture.address, address);
    EXPECT_EQ(capture.definition_id, "iv.test.successor");
    EXPECT_EQ(
        nlohmann::ordered_json::parse(capture.arguments_json),
        nlohmann::ordered_json::array({41}));
}

TEST(ModuleContextBoundary, ModuleContextBuildsSemanticAddressesFromNamedLocals)
{
    struct Capture {
        iv::NodeAddress address{};
        int value = 0;
    } capture;
    iv::NodeMessageContextOperations const operations{
        .context = &capture,
        .send = [](
            void* opaque,
            iv::NodeAddress const& address,
            iv::OwnedNodeMessage event) {
            auto& result = *static_cast<Capture*>(opaque);
            result.address = address;
            result.value = static_cast<AddressingModule::SendEvent const*>(
                event.data())->value;
        },
    };
    iv::NodeAddress const address{.instance_id = "instance:module"};
    iv::ModuleMessageLocalRefs const locals{
        .values = {{
            .name = "voice",
            .relative_path = {
                iv::ProjectVirtualNodeSelector{
                    .source_identity = "module.cpp:voice",
                },
            },
        }},
    };
    iv::ModuleMessageContext<AddressingModule> context(
        operations, address, &locals);

    context.send(
        context.ref<"voice">().member(2).tiled_child<"right">(),
        AddressingModule::SendEvent{.value = 17});

    EXPECT_EQ(capture.value, 17);
    ASSERT_EQ(capture.address.path.size(), 3u);
    EXPECT_EQ(
        std::get<iv::ProjectVirtualNodeSelector>(capture.address.path[0])
            .source_identity,
        "module.cpp:voice");
    EXPECT_EQ(
        std::get<iv::ProjectVirtualMemberSelector>(capture.address.path[1]).index,
        2u);
    EXPECT_EQ(
        std::get<iv::ProjectTiledChildSelector>(capture.address.path[2]).name,
        "right");
}

TEST(ModuleContextBoundary, ModuleContextRejectsAnUnknownNamedLocal)
{
    iv::NodeMessageContextOperations const operations{};
    iv::NodeAddress const address{.instance_id = "instance:module"};
    iv::ModuleMessageLocalRefs const locals{};
    iv::ModuleMessageContext<AddressingModule> context(
        operations, address, &locals);

    EXPECT_THROW((void)context.ref<"missing">(), std::logic_error);
}
