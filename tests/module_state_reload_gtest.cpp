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
            return record.state_structure
                && std::ranges::any_of(
                    record.state_structure->fields,
                    [&](iv::NodeStateFieldStructure const& field) {
                        return field.name == field_name;
                    });
        });
}

auto compiled_state_node(
    iv::BlockNodeExecutor const& executor,
    std::string const& field_name)
{
    return std::ranges::find_if(
        executor.layout().nodes,
        [&](iv::NodeLayout::NodeRecord const& record) {
            return record.compiled_state_structure
                && std::ranges::any_of(
                    record.compiled_state_structure->fields,
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
        auto const probe = g.node<"iv.test.state_reload.probe">();
        g.outputs("main"_P = probe);
    }
}

IV_NODE("iv.test.state_reload.probe", ReloadProbe);
)");

    auto loader = iv::test::make_loader();
    auto first = loader.load_package_definitions(workspace).front();
    // The executor holds raw callbacks into both generations while it retires
    // the old state. Keep the loaded module references alive past executor
    // teardown, just as a live module instance does.
    std::optional<decltype(first)> second;
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(first.root), 8);

    auto old_node = state_node(executor, "value");
    ASSERT_NE(old_node, executor.layout().nodes.end());
    ASSERT_TRUE(old_node->state_structure.has_value());
    ASSERT_EQ(old_node->state_structure->fields.size(), 1u);
    auto const old_node_type_name = std::string(old_node->node_type_name);
    auto const old_field_type = old_node->state_structure->fields.front().type_name;
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
    ASSERT_TRUE(new_node->state_structure.has_value());
    ASSERT_EQ(new_node->state_structure->fields.size(), 1u);
    EXPECT_EQ(new_node->node_type_name, old_node_type_name);
    EXPECT_NE(new_node->state_structure->fields.front().type_name, old_field_type);
    auto const new_index = static_cast<std::size_t>(
        std::distance(executor.layout().nodes.begin(), new_node));
    EXPECT_FLOAT_EQ(*static_cast<float*>(executor.storage().state_ptr(new_index)), 4.5f);
}

TEST(ModuleStateReload, SameSizeCompiledStateDefinitionChangeInitializesInsteadOfMigrating)
{
    auto const workspace = iv::test_support::make_inline_module_workspace(
        "module_compiled_state_reload_same_size_type",
        R"(#include <intravenous/dsl.h>

#include <array>
#include <cstdint>
#include <string>

namespace {
    struct ReloadProbe {
        struct CompiledState {
            std::int32_t value = 0;
        };

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        std::string identity() const
        {
            return "stable-compiled-reload-probe";
        }

        void initialize(iv::InitializationContext<ReloadProbe> const& ctx) const
        {
            ctx.compiled_state().value = 123;
        }

        void move(iv::MoveContext<ReloadProbe> const& ctx) const
        {
            ctx.compiled_state().value =
                ctx.previous_compiled_state().value + 1;
        }

        void tick(iv::TickSampleContext<ReloadProbe> const& ctx) const
        {
            ctx.outputs[0].push(
                static_cast<float>(ctx.compiled_state().value));
        }
    };

    void compiled_state_reload_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const probe = g.node<"iv.test.compiled_state_reload.probe">();
        g.outputs("main"_P = probe);
    }
}

IV_NODE("iv.test.compiled_state_reload.probe", ReloadProbe);
)");

    auto loader = iv::test::make_loader();
    auto first = loader.load_package_definitions(workspace).front();
    std::optional<decltype(first)> second;
    std::optional<decltype(first)> third;
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(first.root), 8);

    auto old_node = compiled_state_node(executor, "value");
    ASSERT_NE(old_node, executor.layout().nodes.end());
    ASSERT_TRUE(old_node->compiled_state_structure.has_value());
    EXPECT_TRUE(old_node->compiled_state_structure->type_identity.valid());
    auto const old_fingerprint =
        old_node->compiled_state_structure->type_identity.definition_fingerprint;
    auto const old_index = static_cast<std::size_t>(
        std::distance(executor.layout().nodes.begin(), old_node));
    EXPECT_EQ(
        *static_cast<std::int32_t*>(
            executor.storage().compiled_state_ptr(old_index)),
        123);

    auto source = iv::test::read_text(workspace / "module.cpp");
    auto const original_initialization =
        std::string("ctx.compiled_state().value = 123;");
    auto const changed_initialization =
        std::string("ctx.compiled_state().value = 999;");
    ASSERT_NE(source.find(original_initialization), std::string::npos);
    source.replace(
        source.find(original_initialization),
        original_initialization.size(),
        changed_initialization);
    iv::test::write_text_advancing_timestamp(workspace / "module.cpp", source);

    second.emplace(loader.load_package_definitions(workspace).front());
    executor.reload(iv::TypeErasedNode(second->root));

    auto moved_node = compiled_state_node(executor, "value");
    ASSERT_NE(moved_node, executor.layout().nodes.end());
    ASSERT_TRUE(moved_node->compiled_state_structure.has_value());
    EXPECT_EQ(
        moved_node->compiled_state_structure->type_identity.definition_fingerprint,
        old_fingerprint);
    auto const moved_index = static_cast<std::size_t>(
        std::distance(executor.layout().nodes.begin(), moved_node));
    EXPECT_EQ(
        *static_cast<std::int32_t*>(
            executor.storage().compiled_state_ptr(moved_index)),
        124);

    source = iv::test::read_text(workspace / "module.cpp");
    auto const old_state = std::string("std::int32_t value = 0;");
    auto const new_state = std::string("float value = 0.0f;");
    auto const old_initialization = changed_initialization;
    auto const new_initialization =
        std::string("ctx.compiled_state().value = 4.5f;");
    ASSERT_NE(source.find(old_state), std::string::npos);
    ASSERT_NE(source.find(old_initialization), std::string::npos);
    source.replace(source.find(old_state), old_state.size(), new_state);
    source.replace(
        source.find(old_initialization),
        old_initialization.size(),
        new_initialization);
    iv::test::write_text_advancing_timestamp(workspace / "module.cpp", source);

    third.emplace(loader.load_package_definitions(workspace).front());
    executor.reload(iv::TypeErasedNode(third->root));

    auto new_node = compiled_state_node(executor, "value");
    ASSERT_NE(new_node, executor.layout().nodes.end());
    ASSERT_TRUE(new_node->compiled_state_structure.has_value());
    EXPECT_TRUE(new_node->compiled_state_structure->type_identity.valid());
    EXPECT_NE(
        new_node->compiled_state_structure->type_identity.definition_fingerprint,
        old_fingerprint);
    auto const new_index = static_cast<std::size_t>(
        std::distance(executor.layout().nodes.begin(), new_node));
    EXPECT_FLOAT_EQ(
        *static_cast<float*>(executor.storage().compiled_state_ptr(new_index)),
        4.5f);
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
            return std::array<iv::InputConfig, 2>{
                iv::realtime_sample_input("signal"),
                iv::realtime_event_input("reset", iv::EventTypeId::trigger),
            };
        }

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 2>{
                iv::realtime_sample_output("out"),
                iv::realtime_event_output("changed", iv::EventTypeId::trigger),
            };
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
        auto const node = g.node<"iv.test.node_definition.contract">();
        node("signal"_P = signal);
        g.outputs("main"_P = node);
    }
}

IV_NODE("iv.test.node_definition.contract", ModuleNodeDefinitionContract);
)");

    auto loader = iv::test::make_loader();
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

        constexpr CStringCaptureProbe(
            char const* first = nullptr,
            char const* second = nullptr,
            char const* trailing = nullptr)
            : labels{first, second}
            , details{trailing}
        {}

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
        auto const probe = g.node<"iv.test.c_string_capture.probe">(
            first, second, trailing);
        g.outputs("main"_P = probe);
    }
}

IV_NODE("iv.test.c_string_capture.probe", CStringCaptureProbe);
)");

    auto loader = iv::test::make_loader();
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
            return std::array<iv::InputConfig, 2>{
                iv::realtime_sample_input("input"),
                iv::realtime_sample_input("modulation"),
            };
        }

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{
                iv::realtime_sample_output("out"),
            };
        }

        void tick(iv::TickSampleContext<ScalarPassthrough> const& ctx) const
        {
            ctx.outputs[0].push(ctx.inputs[0].get() + ctx.inputs[1].get());
        }
    };

    void implicit_constant_configuration_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const passthrough = g.node<"iv.test.implicit_constant.passthrough">();
        passthrough(
            "input"_P = 0.25f,
            "modulation"_P = 0.5f);
        g.outputs("main"_P = passthrough);
    }
}

IV_NODE("iv.test.implicit_constant.passthrough", ScalarPassthrough);
)");

    auto loader = iv::test::make_loader();
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
            return std::array<iv::OutputConfig, 1>{
                iv::realtime_sample_output("out"),
            };
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
        auto const node = g.node<"iv.test.metadata.used_node">();
        g.outputs("main"_P = node);
    }
}

IV_NODE("iv.test.metadata.used_node", UsedNode);
)");

    auto loader = iv::test::make_loader();
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
