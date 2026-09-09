#include <intravenous/dsl.h>

#include <gtest/gtest.h>

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
            (void)ctx.state();
        }

        void initialize(iv::InitializationContext<ModuleDefinedNode> const& ctx)
            const
        {
            ++ctx.state().value;
        }
    };

    static_assert(iv::details::has_declare<ModuleDefinedNode>);
    static_assert(iv::details::has_initialize<ModuleDefinedNode>);
}

TEST(ModuleContextBoundary, DslKeepsLayoutAndStorageIncomplete)
{
    SUCCEED();
}
