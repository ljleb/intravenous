#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/dsl.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/module/configured_graph_wire.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

struct ArchiveFixture {
    iv::SerializedConfiguredGraph archive;
    using Pass = iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>;
    std::array<iv::details::NodeCompilerRecord, 2> node_types{
        iv::details::node_compiler_record<iv::Constant>,
        iv::details::node_compiler_record<Pass>};

    ArchiveFixture()
    {
        using namespace iv;
        iv::GraphBuilder graph;
        auto const gain = graph.input<"gain">(0.25f);
        auto const source = iv::details::configure_concrete_node<iv::Constant>(
            graph, iv::Sample{0.75f});
        auto const source_handle = source.node_bundle_handle();
        auto const pass = iv::details::configure_concrete_node<Pass>(graph);
        pass(gain);
        pass._annotate_input_source_info(
            PortKind::sample, "", "archive-pass", "/tmp/archive-module.cpp", 10, 18);
        graph.outputs("gain_out"_P = pass, "main"_P = source);

        auto configured = std::move(graph).finish();
        configured.node_bundles.set_registered_node_type_identity(
            source_handle,
            iv::RegisteredNodeTypeIdentity{
                .node_type_id = "iv.test.archive.constant",
                .provider_package_root = "/tmp/iv-test-provider",
            });
        archive = iv::serialize_configured_graph(std::move(configured));
    }

    iv::ConfiguredGraph decode(std::span<std::byte const> bytes) const
    {
        return iv::deserialize_configured_graph(
            bytes, node_types, archive.node_configs);
    }
};

TEST(ConfiguredGraphBinaryArchive, RoundTripsNativeScalarsAndRejectsCorruption)
{
    ArchiveFixture fixture;

    auto decoded = fixture.decode(fixture.archive.bytes);
    std::optional<iv::RegisteredNodeTypeIdentity> registered_identity;
    decoded.node_bundles.for_each_configured_bundle(
        [&](iv::ConfiguredNodeBundleView const& bundle) {
            if (bundle.registered_node_type_identity) {
                ASSERT_FALSE(registered_identity.has_value());
                registered_identity = *bundle.registered_node_type_identity;
            }
        });
    ASSERT_TRUE(registered_identity.has_value());
    EXPECT_EQ(registered_identity->node_type_id, "iv.test.archive.constant");
    EXPECT_EQ(
        registered_identity->provider_package_root,
        "/tmp/iv-test-provider");

    auto const virtual_record = std::ranges::find_if(
        decoded.virtual_nodes.records(),
        [](auto const& record) { return record.source_identity == "archive-pass"; });
    ASSERT_NE(virtual_record, decoded.virtual_nodes.records().end());
    ASSERT_EQ(virtual_record->sample_inputs.size(), 1u);
    ASSERT_EQ(virtual_record->sample_inputs.front().source_infos.size(), 1u);
    EXPECT_EQ(
        virtual_record->sample_inputs.front().source_infos.front().span.file_path,
        "/tmp/archive-module.cpp");
    EXPECT_EQ(
        virtual_record->sample_inputs.front().source_infos.front().span.begin, 10u);
    EXPECT_EQ(
        virtual_record->sample_inputs.front().source_infos.front().span.end, 18u);

    auto plan = iv::GraphCompiler::compile(
        iv::GraphLowerer::lower(std::move(decoded)));
    auto const inputs = plan.graph.inputs();
    auto const outputs = plan.graph.outputs();

    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs.front().name, "gain");
    ASSERT_TRUE(iv::is_sample(inputs.front()));
    auto const& gain_properties = iv::sample_properties(inputs.front());
    EXPECT_FLOAT_EQ(gain_properties.default_value, 0.25f);
    EXPECT_TRUE(std::isinf(gain_properties.min.value));
    EXPECT_TRUE(std::isinf(gain_properties.max.value));
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
