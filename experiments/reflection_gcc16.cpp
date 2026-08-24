#include <array>
#include <cstddef>
#include <iostream>
#include <meta>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sm = std::meta;

#ifndef __cpp_impl_reflection
#error "GCC reflection support is not enabled"
#endif

static_assert(__cpp_impl_reflection >= 202506L);

struct NodeConfig {
    int channels;
    float gain;

    constexpr bool operator==(NodeConfig const&) const = default;
};

consteval bool reflects_node_structure()
{
    auto const members = sm::nonstatic_data_members_of(^^NodeConfig, sm::access_context::current());

    return members.size() == 2 && sm::identifier_of(members[0]) == "channels" &&
           sm::type_of(members[0]) == (^^int)&&sm::offset_of(members[0]).bytes ==
               offsetof(NodeConfig, channels) &&
           sm::identifier_of(members[1]) == "gain" &&
           sm::type_of(members[1]) == (^^float)&&sm::offset_of(members[1]).bytes ==
               offsetof(NodeConfig, gain);
}

static_assert(reflects_node_structure());

constexpr auto node_config_reflection = sm::reflect_constant(NodeConfig { 2, 0.5F });
constexpr NodeConfig reflected_node_config = [:node_config_reflection:];
static_assert(reflected_node_config == NodeConfig { 2, 0.5F });

consteval auto freeze_transient_vector()
{
    std::vector<int> values;
    values.push_back(3);
    values.push_back(5);
    values.push_back(8);
    return std::define_static_array(values);
}

constexpr std::span<const int> frozen_values = freeze_transient_vector();
static_assert(frozen_values.size() == 3);
static_assert(frozen_values[0] == 3 && frozen_values[1] == 5 && frozen_values[2] == 8);

consteval auto freeze_transient_nodes()
{
    std::vector<NodeConfig> nodes;
    nodes.push_back({ 2, 0.5F });
    nodes.push_back({ 2, 3.0F });
    return std::define_static_array(nodes);
}

constexpr auto frozen_nodes = freeze_transient_nodes();

[[gnu::noinline]] constexpr float expanded_tick(float sample)
{
    template for (constexpr auto node : frozen_nodes)
    {
        sample *= node.gain;
    }
    return sample;
}

static_assert(expanded_tick(4.0F) == 6.0F);

template <class Tag>
struct GeneratedLayout;

struct LayoutTag;

consteval
{
    std::vector<std::meta::info> members;
    members.push_back(sm::data_member_spec(^^std::size_t, { .name = "node_count" }));
    members.push_back(sm::data_member_spec(^^NodeConfig, { .name = "root" }));
    sm::define_aggregate(^^GeneratedLayout<LayoutTag>, members);
}

static_assert(std::is_aggregate_v<GeneratedLayout<LayoutTag>>);
static_assert(sizeof(GeneratedLayout<LayoutTag>) >= sizeof(std::size_t) + sizeof(NodeConfig));

consteval std::size_t reflected_generated_member_count()
{
    return sm::nonstatic_data_members_of(
               ^^GeneratedLayout<LayoutTag>, sm::access_context::current()
    )
        .size();
}

static_assert(reflected_generated_member_count() == 2);

int main(int argc, char**)
{
    GeneratedLayout<LayoutTag> layout { frozen_values.size(), reflected_node_config };
    std::cout << "reflection=" << reflects_node_structure() << " frozen=" << frozen_values.size()
              << " generated=" << layout.node_count << " gain=" << layout.root.gain
              << " expanded=" << expanded_tick(static_cast<float>(argc)) << '\n';
}
