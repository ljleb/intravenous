#include "module_test_utils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>

namespace {

auto state_node(
    iv::BlockNodeExecutor const& executor,
    std::string const& field_name)
{
    return std::ranges::find_if(
        executor.layout().nodes,
        [&](iv::NodeLayout::NodeRecord const& record) {
            return record.node_state_structure
                && std::ranges::any_of(
                    record.node_state_structure->fields,
                    [&](iv::NodeStateFieldStructure const& field) {
                        return field.name == field_name;
                    });
        });
}

TEST(ModuleStateReload, SameSizeStateFieldTypeChangeInitializesInsteadOfMigrating)
{
    auto const workspace = iv::test_support::make_inline_module_workspace(
        "module_state_reload_same_size_field_type",
        R"(#include <intravenous/dsl.h>

#include <array>
#include <cstdint>
#include <string>

namespace {
    struct ReloadProbe {
        struct State {
            std::int32_t value = 0;
        };

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        std::string identity() const
        {
            return "stable-reload-probe";
        }

        void initialize(iv::InitializationContext<ReloadProbe> const& ctx) const
        {
            ctx.state().value = 123;
        }

        void move(iv::MoveContext<ReloadProbe> const& ctx) const
        {
            ctx.state().value = ctx.previous_state().value + 1;
        }

        void tick(iv::TickSampleContext<ReloadProbe> const& ctx) const
        {
            ctx.outputs[0].push(0.0f);
        }
    };

    void state_reload_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const probe = g.node<ReloadProbe>();
        g.outputs("main"_P = probe);
    }
}
)");

    iv::ModuleLoader loader(iv::test::repo_root(), {});
    auto first = loader.load_root_definition(workspace);
    // The executor holds raw callbacks into both generations while it retires
    // the old state. Keep the loaded module references alive past executor
    // teardown, just as a live module instance does.
    std::optional<decltype(first)> second;
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(first.root), 8);

    auto old_node = state_node(executor, "value");
    ASSERT_NE(old_node, executor.layout().nodes.end());
    ASSERT_TRUE(old_node->node_state_structure.has_value());
    ASSERT_EQ(old_node->node_state_structure->fields.size(), 1u);
    auto const old_node_type_name = std::string(old_node->node_type_name);
    auto const old_field_type = old_node->node_state_structure->fields.front().type_name;
    auto const old_index = static_cast<std::size_t>(
        std::distance(executor.layout().nodes.begin(), old_node));
    EXPECT_EQ(*static_cast<std::int32_t*>(executor.storage().state_ptr(old_index)), 123);

    auto source = iv::test::read_text(workspace / "module.cpp");
    auto const old_state = std::string("std::int32_t value = 0;");
    auto const new_state = std::string("float value = 0.0f;");
    auto const old_initialization = std::string("ctx.state().value = 123;");
    auto const new_initialization = std::string("ctx.state().value = 4.5f;");
    ASSERT_NE(source.find(old_state), std::string::npos);
    ASSERT_NE(source.find(old_initialization), std::string::npos);
    source.replace(source.find(old_state), old_state.size(), new_state);
    source.replace(
        source.find(old_initialization),
        old_initialization.size(),
        new_initialization);
    iv::test::write_text_advancing_timestamp(workspace / "module.cpp", source);

    second.emplace(loader.load_root_definition(workspace));
    executor.reload(iv::TypeErasedNode(second->root));

    auto new_node = state_node(executor, "value");
    ASSERT_NE(new_node, executor.layout().nodes.end());
    ASSERT_TRUE(new_node->node_state_structure.has_value());
    ASSERT_EQ(new_node->node_state_structure->fields.size(), 1u);
    EXPECT_EQ(new_node->node_type_name, old_node_type_name);
    EXPECT_NE(new_node->node_state_structure->fields.front().type_name, old_field_type);
    auto const new_index = static_cast<std::size_t>(
        std::distance(executor.layout().nodes.begin(), new_node));
    EXPECT_FLOAT_EQ(*static_cast<float*>(executor.storage().state_ptr(new_index)), 4.5f);
}

} // namespace
