#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/module/configured_graph_wire.h>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

struct ArchiveFixture {
    iv::SerializedConfiguredGraph archive;
    using Pass = iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>;
    std::array<iv::details::NodeCompilerRecord, 2> node_types{
        iv::details::node_compiler_record<iv::Constant>,
        iv::details::node_compiler_record<Pass>};
    std::vector<iv::ModuleNodeConfigRecord> configs;

    ArchiveFixture()
    {
        using namespace iv;
        iv::GraphBuilder graph;
        auto const gain = graph.input<"gain">(0.25f);
        auto const source = graph.node<iv::Constant>(iv::Sample{0.75f});
        auto const pass = graph.node<Pass>();
        pass(gain);
        graph.outputs("gain_out"_P = pass, "main"_P = source);

        archive = iv::serialize_configured_graph(std::move(graph).finish());
        configs.reserve(archive.node_configs.size());
        for (auto const& config : archive.node_configs) {
            configs.push_back({
                .data = config.bytes.data(),
                .size = config.bytes.size(),
                .alignment = config.alignment,
            });
        }
    }

    iv::ConfiguredGraph decode(std::span<std::byte const> bytes) const
    {
        return iv::deserialize_configured_graph(bytes, node_types, configs);
    }
};

TEST(ConfiguredGraphBinaryArchive, RoundTripsNativeScalarsAndRejectsCorruption)
{
    ArchiveFixture fixture;

    auto decoded = fixture.decode(fixture.archive.bytes);
    auto plan = iv::GraphCompiler::compile(
        iv::GraphLowerer::lower(std::move(decoded)));
    auto const inputs = plan.graph.inputs();
    auto const outputs = plan.graph.outputs();

    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs.front().name, "gain");
    EXPECT_FLOAT_EQ(inputs.front().default_value, 0.25f);
    EXPECT_TRUE(std::isinf(inputs.front().min.value));
    EXPECT_TRUE(std::isinf(inputs.front().max.value));
    ASSERT_EQ(outputs.size(), 2u);
    EXPECT_EQ(outputs[0].name, "gain_out");
    EXPECT_EQ(outputs[1].name, "main");

    auto corrupt = fixture.archive.bytes;
    corrupt.front() = std::byte{};
    EXPECT_THROW(fixture.decode(corrupt), std::runtime_error);

    auto truncated = fixture.archive.bytes;
    truncated.pop_back();
    EXPECT_THROW(fixture.decode(truncated), std::runtime_error);

    auto trailing = fixture.archive.bytes;
    trailing.push_back(std::byte{});
    EXPECT_THROW(fixture.decode(trailing), std::runtime_error);
}

} // namespace
