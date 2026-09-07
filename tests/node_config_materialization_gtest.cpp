#include <intravenous/module/node_config_materialization.h>
#include <intravenous/node/config_string.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

struct alignas(32) StringConfig {
    iv::NodeConfigString payload;
    iv::NodeConfigString empty;
    std::uint32_t marker;
};

constexpr auto payload_offset =
    offsetof(StringConfig, payload) + offsetof(iv::NodeConfigString, data);
constexpr auto empty_offset =
    offsetof(StringConfig, empty) + offsetof(iv::NodeConfigString, data);

TEST(NodeConfigMaterialization, CopiesAlignedConfigAndOwnsEmptyAndEmbeddedNulStrings)
{
    iv::details::MaterializedNodeConfigs materialized;
    {
        std::string payload{"A\0B", 3};
        std::string empty;
        StringConfig source{
            .payload = iv::NodeConfigString{std::string_view{payload}},
            .empty = iv::NodeConfigString{std::string_view{empty}},
            .marker = 0xabcdu,
        };
        std::array configs{iv::ModuleNodeConfigRecord{
            .data = &source,
            .size = sizeof(source),
            .alignment = alignof(StringConfig),
        }};
        std::array relocations{
            iv::ModuleNodeConfigStringRelocationRecord{
                .config_ordinal = 0,
                .byte_offset = payload_offset,
                .string_data = payload.data(),
                .string_size = payload.size(),
            },
            iv::ModuleNodeConfigStringRelocationRecord{
                .config_ordinal = 0,
                .byte_offset = empty_offset,
                .string_data = empty.data(),
                .string_size = empty.size(),
            },
        };

        materialized = iv::details::materialize_node_configs(configs, relocations);

        auto duplicate = relocations;
        duplicate[1].byte_offset = payload_offset;
        EXPECT_THROW(
            iv::details::materialize_node_configs(configs, duplicate),
            std::runtime_error);
    }

    ASSERT_EQ(materialized.records.size(), 1u);
    ASSERT_EQ(materialized.storage.size(), 1u);
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(materialized.records.front().data)
            % alignof(StringConfig),
        0u);
    auto const* copied = static_cast<StringConfig const*>(materialized.records.front().data);
    EXPECT_EQ(copied->marker, 0xabcdu);
    auto const expected_payload = std::string_view{"A\0B", 3};
    EXPECT_EQ(copied->payload.view(), expected_payload);
    EXPECT_TRUE(copied->empty.view().empty());
}

} // namespace
