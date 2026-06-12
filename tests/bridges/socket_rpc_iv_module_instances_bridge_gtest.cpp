#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string_view>

namespace {
using Json = nlohmann::ordered_json;

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}
} // namespace

TEST(SocketRpcIvModuleInstancesBridge, UnboundCreateEventLeavesBuilderUnbuilt)
{
    iv::SocketRpcCreateIvModuleInstanceResultBuilder builder;

    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_create_iv_module_instance_event,
        iv::CreateIvModuleInstanceRequest{
            .module_root = std::filesystem::path("/tmp/module-a"),
        },
        builder);

    EXPECT_THROW((void)builder.build(1), std::runtime_error);
}

TEST(SocketRpcIvModuleInstancesBridge, BoundEventsCreateAndDeleteInstances)
{
    iv::IvModuleInstances instances;
    iv::bind_socket_rpc_iv_module_instances_bridge(instances);

    iv::SocketRpcCreateIvModuleInstanceResultBuilder create_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_create_iv_module_instance_event,
        iv::CreateIvModuleInstanceRequest{
            .module_root = std::filesystem::path("/tmp/module-a"),
        },
        create_builder);
    auto const create_response = parse_json_line(create_builder.build(2));
    auto const created_instance_id =
        create_response["result"]["instanceId"].get<std::string>();
    EXPECT_FALSE(created_instance_id.empty());

    iv::SocketRpcAckResponseBuilder delete_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_delete_iv_module_instance_event,
        iv::DeleteIvModuleInstanceRequest{
            .instance_id = created_instance_id,
        },
        delete_builder);
    auto const delete_response = parse_json_line(delete_builder.build(3));
    EXPECT_EQ(delete_response["result"]["ok"], true);

    iv::unbind_socket_rpc_iv_module_instances_bridge(instances);
}

TEST(SocketRpcIvModuleInstancesBridge, BoundSetDefaultSilenceTtlUpdatesInstance)
{
    iv::IvModuleInstances instances;
    iv::bind_socket_rpc_iv_module_instances_bridge(instances);

    iv::SocketRpcCreateIvModuleInstanceResultBuilder create_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_create_iv_module_instance_event,
        iv::CreateIvModuleInstanceRequest{
            .module_root = std::filesystem::path("/tmp/module-a"),
        },
        create_builder);
    auto const create_response = parse_json_line(create_builder.build(2));
    auto const created_instance_id =
        create_response["result"]["instanceId"].get<std::string>();

    iv::SocketRpcAckResponseBuilder ttl_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_set_iv_module_instance_default_silence_ttl_samples_event,
        iv::SetIvModuleInstanceDefaultSilenceTtlSamplesRequest{
            .instance_id = created_instance_id,
            .default_silence_ttl_samples = 1234,
        },
        ttl_builder);
    auto const ttl_response = parse_json_line(ttl_builder.build(4));
    EXPECT_EQ(ttl_response["result"]["ok"], true);

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    ASSERT_TRUE(listed.front().default_silence_ttl_samples.has_value());
    EXPECT_EQ(*listed.front().default_silence_ttl_samples, 1234u);

    iv::unbind_socket_rpc_iv_module_instances_bridge(instances);
}
