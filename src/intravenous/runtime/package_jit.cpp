#include <intravenous/runtime/package_jit.h>

#include <intravenous/juce/vst_runtime.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {
std::string describe_exception(std::exception_ptr exception)
{
    if (!exception) return "unknown exception";
    try {
        std::rethrow_exception(exception);
    } catch (std::exception const& error) {
        return error.what();
    } catch (...) {
        return "unknown exception";
    }
}
} // namespace

PackageJit::PackageJit(StartupConfigState startup_config)
    : startup_config_(std::move(startup_config))
{}

ModuleLoader& PackageJit::ensure_loader()
{
    std::scoped_lock lock(mutex_);
    if (!loader_) {
        loader_ = std::make_unique<ModuleLoader>(
            startup_config_.discovery_start,
            startup_config_.search_roots,
            startup_config_.toolchain,
            ModuleLoader::LogSink{});
    }
    return *loader_;
}

void PackageJit::set_toolchain_config(ModuleLoaderToolchainConfig toolchain)
{
    std::scoped_lock lock(mutex_);
    startup_config_.toolchain = toolchain;
    if (loader_) loader_->set_toolchain_config(std::move(toolchain));
}

ModuleLoaderToolchainConfig PackageJit::toolchain_config() const
{
    std::scoped_lock lock(mutex_);
    return startup_config_.toolchain;
}

PackageJitBatchResult PackageJit::build(
    std::vector<IvPackageDeclaration> const& declarations)
{
#if IV_ENABLE_JUCE_VST
    warmup_juce_vst_scan_cache();
#endif

    PackageJitBatchResult results;
    if (declarations.empty()) return results;

    auto& loader = ensure_loader();
    std::vector<std::filesystem::path> package_paths;
    package_paths.reserve(declarations.size());
    for (auto const& declaration : declarations) {
        package_paths.push_back(declaration.package_root);
    }
    auto batch = loader.load_packages(package_paths);

    for (std::size_t index = 0; index < declarations.size(); ++index) {
        auto const& declaration = declarations[index];
        try {
            if (index >= batch.size()) {
                throw std::logic_error("IV package JIT batch omitted a requested package");
            }
            auto& result = batch[index];
            if (!result) {
                throw std::runtime_error(result.error.empty()
                    ? "IV package JIT failed without an error"
                    : result.error);
            }

            auto loaded_package = std::move(*result.package);
            PackageRevision revision{
                .package_id = declaration.package_id,
                .package_root = declaration.package_root,
                .compiler_artifact = std::move(loaded_package.compiler_artifact),
                .package_code = std::move(loaded_package.package_code),
                .provider_definitions = std::move(loaded_package.provider_definitions),
                .config_pointer_fields = std::move(loaded_package.config_pointer_fields),
                .retained_globals = std::move(loaded_package.retained_globals),
                .node_state_structures = std::move(loaded_package.node_state_structures),
                .dependencies = std::move(loaded_package.dependencies),
            };
            {
                std::scoped_lock lock(mutex_);
                revision.revision = ++next_revision_by_package_id_[declaration.package_id];
            }
            revision.module_definitions.reserve(loaded_package.definitions.size());
            for (auto& loaded_definition : loaded_package.definitions) {
                revision.module_definitions.push_back(PackageModuleDefinition{
                    .package_id = declaration.package_id,
                    .definition_id = loaded_definition.module_id,
                    .package_root = declaration.package_root,
                    .module_id = std::move(loaded_definition.module_id),
                    .provider = NodeDefinitionProvider{
                        .module_build = loaded_definition.provider.module_build,
                        .signature = loaded_definition.provider.signature,
                    },
                    .introspection = std::move(loaded_definition.introspection),
                    .dependencies = std::move(loaded_definition.dependencies),
                    .module_refs = std::move(loaded_definition.module_refs),
                    .configured_graph = std::move(loaded_definition.configured_graph),
                });
            }
            revision.leaf_definitions.reserve(loaded_package.node_types.size());
            for (auto& node_type : loaded_package.node_types) {
                revision.leaf_definitions.push_back(PackageLeafDefinition{
                    .package_id = declaration.package_id,
                    .definition_id = std::move(node_type.node_type_id),
                    .package_root = declaration.package_root,
                    .provider = NodeDefinitionProvider{
                        .leaf_build = node_type.provider.node_build,
                        .signature = node_type.provider.signature,
                    },
                    .compiler_record = node_type.compiler_record,
                    .module_refs = std::move(node_type.module_refs),
                });
            }
            results.revisions.push_back(std::move(revision));
        } catch (...) {
            results.failed.push_back(PackageJitFailure{
                .package_id = declaration.package_id,
                .package_root = declaration.package_root,
                .message = describe_exception(std::current_exception()),
            });
        }
    }
    return results;
}

void PackageJit::handle_build_request(PackageJitBatchRequest& request)
{
    if (!request.removed_package_roots.empty()) {
        auto& loader = ensure_loader();
        for (auto const& package_root : request.removed_package_roots) {
            loader.remove_package(package_root);
        }
    }
    request.result = build(request.declarations);
}

void PackageJit::handle_project_override_settings(
    ProjectOverrideSettingsRequest const& request)
{
    bool touched = false;
    auto toolchain = toolchain_config();
    auto const assign_path = [&](std::optional<std::filesystem::path> const& value,
                                 std::optional<std::filesystem::path>
                                     ModuleLoaderToolchainConfig::*field) {
        if (!value.has_value() || value == toolchain.*field) return;
        toolchain.*field = value;
        touched = true;
    };
    auto const assign_string = [&](std::optional<std::string> const& value,
                                   std::optional<std::string>
                                       ModuleLoaderToolchainConfig::*field) {
        if (!value.has_value() || value == toolchain.*field) return;
        toolchain.*field = value;
        touched = true;
    };

    assign_path(request.c_compiler, &ModuleLoaderToolchainConfig::c_compiler);
    assign_path(request.cxx_compiler, &ModuleLoaderToolchainConfig::cxx_compiler);
    assign_path(request.cmake_program, &ModuleLoaderToolchainConfig::cmake_program);
    assign_string(request.cmake_generator, &ModuleLoaderToolchainConfig::cmake_generator);
    assign_path(request.make_program, &ModuleLoaderToolchainConfig::make_program);
    assign_path(request.juce_dir, &ModuleLoaderToolchainConfig::juce_dir);
    assign_path(request.iv_package_pch, &ModuleLoaderToolchainConfig::iv_package_pch);
    if (!touched) return;

    set_toolchain_config(std::move(toolchain));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void PackageJit::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder& builder) const
{
    builder.add_project_toolchain_config(toolchain_config());
}
} // namespace iv
