#include "module_test_utils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

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
    auto first = loader.load_package_definitions(workspace).front();
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

    second.emplace(loader.load_package_definitions(workspace).front());
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

TEST(ModuleNodeDefinition, DslRetainsPortConfigsTraitsAndContexts)
{
    auto const workspace = iv::test_support::make_inline_module_workspace(
        "module_node_definition_surface",
        R"(#include <intravenous/dsl.h>

#include <array>
#include <cstdint>
#include <type_traits>

namespace {
    struct ModuleNodeDefinitionContract {
        struct State {
            std::int32_t counter = 0;
        };

        static_assert(std::is_same_v<
            typename iv::NodeState<ModuleNodeDefinitionContract>::Type,
            State>);

        static constexpr auto inputs()
        {
            return std::array<iv::InputConfig, 1>{{{
                .name = "signal",
            }}};
        }

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{{{
                .name = "out",
            }}};
        }

        static constexpr auto event_inputs()
        {
            return std::array<iv::EventInputConfig, 1>{{{
                .name = "reset", .type = iv::EventTypeId::trigger,
            }}};
        }

        static constexpr auto event_outputs()
        {
            return std::array<iv::EventOutputConfig, 1>{{{
                .name = "changed", .type = iv::EventTypeId::trigger,
            }}};
        }

        void declare(iv::DeclarationContext<ModuleNodeDefinitionContract> const&) const {}

        void initialize(
            iv::InitializationContext<ModuleNodeDefinitionContract> const& ctx) const
        {
            ctx.state().counter = 7;
        }

        void move(iv::MoveContext<ModuleNodeDefinitionContract> const& ctx) const
        {
            ctx.state().counter = ctx.previous_state().counter;
        }

        void release(iv::ReleaseContext<ModuleNodeDefinitionContract> const&) const {}

        void tick(iv::TickSampleContext<ModuleNodeDefinitionContract> const& ctx) const
        {
            ctx.outputs[0].push(ctx.inputs[0].get());
        }
    };

    void node_definition_contract_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const signal = g.input<"signal">(0.0f);
        auto const node = g.node<ModuleNodeDefinitionContract>();
        node("signal"_P = signal);
        g.outputs("main"_P = node);
    }
}
)");

    iv::ModuleLoader loader(iv::test::repo_root(), {});
    auto definition = loader.load_package_definitions(workspace).front();
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(definition.root), 8);

    auto node = state_node(executor, "counter");
    ASSERT_NE(node, executor.layout().nodes.end());
    auto const node_index = static_cast<std::size_t>(
        std::distance(executor.layout().nodes.begin(), node));
    EXPECT_EQ(*static_cast<std::int32_t*>(executor.storage().state_ptr(node_index)), 7);
}

TEST(ModuleNodeConfiguration, CopiesCStringFieldsBeforeModuleBuildEnds)
{
    struct CStringCaptureProbeLayout {
        char const* labels[2];
        struct Details {
            char const* trailing;
        } details;
    };

    auto const workspace = iv::test_support::make_inline_module_workspace(
        "module_c_string_configuration",
        R"(#include <intravenous/dsl.h>

#include <array>
#include <string>

namespace {
    struct CStringCaptureProbe {
        struct Details {
            char const* trailing = nullptr;
        };
        char const* labels[2] = {nullptr, nullptr};
        Details details{};

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick(iv::TickSampleContext<CStringCaptureProbe> const& ctx) const
        {
            ctx.outputs[0].push(0.0f);
        }
    };

    void c_string_configuration_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        char const* first = "A first configuration string";
        char const* second = "A second configuration string";
        char const* trailing = "A nested configuration string";
        auto const probe = g.node<CStringCaptureProbe>(CStringCaptureProbe{
            .labels = {first, second},
            .details = {trailing},
        });
        g.outputs("main"_P = probe);
    }
}
)");

    iv::ModuleLoader loader(iv::test::repo_root(), {});
    auto definition = loader.load_package_definitions(workspace).front();
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(definition.root), 8);

    auto const probe = std::ranges::find_if(
        executor.layout().nodes,
        [](iv::NodeLayout::NodeRecord const& record) {
            return record.node && record.node_type_name
                && std::string_view(record.node_type_name)
                    .contains("CStringCaptureProbe");
        });
    ASSERT_NE(probe, executor.layout().nodes.end());
    std::array const offsets{
        offsetof(CStringCaptureProbeLayout, labels),
        offsetof(CStringCaptureProbeLayout, labels) + sizeof(char const*),
        offsetof(CStringCaptureProbeLayout, details)
            + offsetof(CStringCaptureProbeLayout::Details, trailing),
    };
    std::array<char const*, 3> const expected{
        "A first configuration string",
        "A second configuration string",
        "A nested configuration string",
    };
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        char const* value = nullptr;
        std::memcpy(
            &value,
            static_cast<std::byte const*>(probe->node) + offsets[index],
            sizeof(value));
        ASSERT_NE(value, nullptr);
        EXPECT_STREQ(value, expected[index]);
    }
}

TEST(ModuleCompilerMetadata, CapturesImplicitConstantForScalarNodeInput)
{
    auto const workspace = iv::test_support::make_inline_module_workspace(
        "module_implicit_constant_configuration",
        R"(#include <intravenous/dsl.h>

#include <array>

namespace {
    struct ScalarPassthrough {
        static constexpr auto inputs()
        {
            return std::array<iv::InputConfig, 2>{{
                {.name = "input"},
                {.name = "modulation"},
            }};
        }

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{{{
                .name = "out",
            }}};
        }

        void tick(iv::TickSampleContext<ScalarPassthrough> const& ctx) const
        {
            ctx.outputs[0].push(ctx.inputs[0].get() + ctx.inputs[1].get());
        }
    };

    void implicit_constant_configuration_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const passthrough = g.node<ScalarPassthrough>();
        passthrough(
            "input"_P = 0.25f,
            "modulation"_P = 0.5f);
        g.outputs("main"_P = passthrough);
    }
}
)");

    iv::ModuleLoader loader(iv::test::repo_root(), {});
    auto const definition = loader.load_package_definitions(workspace).front();
    EXPECT_EQ(definition.module_id, "iv.test.implicit_constant_configuration_module");
}

TEST(ModuleCompilerMetadata, IgnoresUnusedTypesThatOnlyResembleNodes)
{
    auto const workspace = iv::test_support::make_inline_module_workspace(
        "module_metadata_ignores_unused_state_carrier",
        R"(#include <intravenous/dsl.h>

#include <array>
#include <cstdint>

namespace {
    struct MetadataOnlyBase {};

    // This is not a graph node: it is never passed to g.node(). In particular,
    // its State is intentionally outside the node-state ABI contract.
    struct UnusedStateCarrier {
        struct State : MetadataOnlyBase {
            std::int32_t ignored = 0;
        };
    };

    struct UsedNode {
        struct State {
            std::int32_t value = 0;
        };

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{{{
                .name = "out",
            }}};
        }

        void initialize(iv::InitializationContext<UsedNode> const& ctx) const
        {
            ctx.state().value = 17;
        }

        void tick(iv::TickSampleContext<UsedNode> const& ctx) const
        {
            ctx.outputs[0].push(0.0f);
        }
    };

    void metadata_ignores_unused_state_carrier_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const node = g.node<UsedNode>();
        g.outputs("main"_P = node);
    }
}
)");

    iv::ModuleLoader loader(iv::test::repo_root(), {});
    auto const definition = loader.load_package_definitions(workspace).front();
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(definition.root), 8);

    auto const node = state_node(executor, "value");
    ASSERT_NE(node, executor.layout().nodes.end());
    auto const node_index = static_cast<std::size_t>(
        std::distance(executor.layout().nodes.begin(), node));
    EXPECT_EQ(*static_cast<std::int32_t*>(executor.storage().state_ptr(node_index)), 17);
}

} // namespace
