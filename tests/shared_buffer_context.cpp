#include "node/lifecycle.h"
#include "module_test_utils.h"

#include <span>
#include <string>

namespace {
    inline iv::ResourceContext make_resources()
    {
        return iv::ResourceContext {};
    }

    struct Exporter {
        struct State {
            std::span<iv::Sample> values;
        };

        void declare(iv::DeclarationContext<Exporter> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.values, 8);
            ctx.export_array("shared", state.values);
        }

        void initialize(iv::InitializationContext<Exporter> const& ctx) const
        {
            auto& state = ctx.state();
            for (size_t i = 0; i < state.values.size(); ++i) {
                state.values[i] = iv::Sample(i + 1);
            }
        }
    };

    struct Importer {
        struct State {
            std::span<iv::Sample> imported;
            iv::Sample sum = 0.0f;
        };

        void declare(iv::DeclarationContext<Importer> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.import_array("shared", state.imported);
        }

        void initialize(iv::InitializationContext<Importer> const& ctx) const
        {
            auto& state = ctx.state();
            for (iv::Sample sample : state.imported) {
                state.sum += sample;
            }
        }
    };

    struct StorageResolver {
        struct State {
            std::span<iv::Sample const> resolved;
            iv::Sample first = -1.0f;
            size_t size = 0;
        };

        void initialize(iv::InitializationContext<StorageResolver> const& ctx) const
        {
            auto& state = ctx.state();
            state.resolved = ctx.resolve_exported_array_storage<iv::Sample>("shared");
            state.size = state.resolved.size();
            if (!state.resolved.empty()) {
                state.first = state.resolved.front();
            }
        }
    };

    struct MissingResolver {
        struct State {
            std::span<iv::Sample const> resolved;
        };

        void initialize(iv::InitializationContext<MissingResolver> const& ctx) const
        {
            auto& state = ctx.state();
            state.resolved = ctx.resolve_exported_array_storage<iv::Sample>("missing");
        }
    };
}

int main()
{
    iv::test::install_crash_handlers();

    {
        iv::NodeLayoutBuilder builder(8);
        Exporter exporter;
        Importer importer;
        StorageResolver resolver;

        iv::do_declare(exporter, builder);
        iv::test::require(builder.has_export_array<iv::Sample>("shared"), "shared export should be visible during declaration");
        iv::do_declare(importer, builder);
        iv::test::require(builder.has_import_array<iv::Sample>("shared"), "shared import should be visible during declaration");
        iv::do_declare(resolver, builder);

        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        storage.initialize();

        auto& exporter_state = *static_cast<Exporter::State*>(storage.state_ptr(0));
        auto& importer_state = *static_cast<Importer::State*>(storage.state_ptr(1));
        auto& resolver_state = *static_cast<StorageResolver::State*>(storage.state_ptr(2));

        iv::test::require(importer_state.imported.size() == exporter_state.values.size(), "imported span should preserve exported size");
        iv::test::require(importer_state.imported.data() == exporter_state.values.data(), "imported span should preserve exported address");
        iv::test::require(importer_state.sum == 36.0f, "importer should observe exporter-initialized data");
        iv::test::require(resolver_state.size == exporter_state.values.size(), "resolved exported storage should preserve size");
        iv::test::require(resolver_state.resolved.data() == exporter_state.values.data(), "resolved exported storage should preserve address");
        iv::test::require(resolver_state.first == 1.0f, "resolved exported storage should expose initialized samples");
    }

    {
        iv::NodeLayoutBuilder builder(8);
        MissingResolver resolver;
        iv::do_declare(resolver, builder);

        iv::NodeLayout layout = std::move(builder).build();
        auto resources = make_resources();
        iv::NodeStorage storage = layout.create_storage(resources);
        storage.initialize();

        auto& state = *static_cast<MissingResolver::State*>(storage.state_ptr(0));
        iv::test::require(state.resolved.empty(), "missing exported storage should resolve to an empty span");
    }

    return 0;
}
