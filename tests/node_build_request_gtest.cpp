#include <intravenous/graph/reflected_node_description.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace {

struct RequestNode {
    char const* input_name = "input";
    char const* output_name = "output";
    std::size_t latency = 0;

    auto inputs() const
    {
        return std::array{iv::InputConfig{.name = input_name}};
    }

    auto outputs() const
    {
        return std::array{iv::OutputConfig{.name = output_name}};
    }

    std::size_t internal_latency() const
    {
        return latency;
    }

    std::optional<std::size_t> ttl_samples() const
    {
        return 64;
    }

    bool can_skip_block() const
    {
        return true;
    }

    void tick_block(auto const&) const {}
};

TEST(NodeBuildRequest, MaterializesHostOwnedDescriptionFromTypeSpecificCallback)
{
    RequestNode source{
        .input_name = "level",
        .output_name = "signal",
        .latency = 17,
    };
    auto request = iv::details::make_node_build_request(source);
    auto storage = iv::details::copy_node_config_bytes(
        request.config, request.config_size, request.config_alignment);
    auto description = iv::details::materialize_node_description(
        request, std::move(storage));

    ASSERT_NE(description.node_storage.get(), nullptr);
    EXPECT_NE(
        description.node_storage.get(),
        static_cast<void const*>(std::addressof(source)));
    EXPECT_EQ(description.operations.runtime.node_data, description.node_storage.get());
    EXPECT_EQ(description.code_key, iv::details::node_code_key_v<RequestNode>);
    EXPECT_EQ(description.type_name, iv::details::clang_type_name<RequestNode>());
    ASSERT_EQ(description.inputs().size(), 1u);
    EXPECT_EQ(description.inputs().front().name, "level");
    ASSERT_EQ(description.outputs().size(), 1u);
    EXPECT_EQ(description.outputs().front().name, "signal");
    EXPECT_EQ(description.internal_latency(), 17u);
    ASSERT_TRUE(description.ttl_samples().has_value());
    EXPECT_EQ(*description.ttl_samples(), 64u);
    EXPECT_TRUE(description.can_skip_block());
}

} // namespace
