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
    , installation_toolchain_(startup_config_.toolchain)
{}

ModuleLoader& PackageJit::ensure_loader()
{
    std::scoped_lock lock(mutex_);
    if (!loader_) {
        loader_ = std::make_unique<ModuleLoader>(
            startup_config_.discovery_start,
            startup_config_.toolchain,
            ModuleLoader::LogSink{});
    } else if (loader_toolchain_dirty_) {
        loader_->set_toolchain_config(startup_config_.toolchain);
    }
    loader_toolchain_dirty_ = false;
    return *loader_;
}

void PackageJit::set_toolchain_config(ModuleLoaderToolchainConfig toolchain)
{
    std::scoped_lock lock(mutex_);
    set_toolchain_config_locked(std::move(toolchain));
}

void PackageJit::set_toolchain_config_locked(ModuleLoaderToolchainConfig toolchain)
{
    // Empty project overrides select this installation's defaults, including
    // defaults selected in .intravenous_defaults rather than compiled paths.
    auto restore_default = [&](auto field) {
        auto& value = toolchain.*field;
        if (!value || value->empty()) value = installation_toolchain_.*field;
    };
    restore_default(&ModuleLoaderToolchainConfig::c_compiler);
    restore_default(&ModuleLoaderToolchainConfig::cxx_compiler);
    restore_default(&ModuleLoaderToolchainConfig::cmake_program);
    restore_default(&ModuleLoaderToolchainConfig::cmake_generator);
    restore_default(&ModuleLoaderToolchainConfig::make_program);
    restore_default(&ModuleLoaderToolchainConfig::juce_dir);
    restore_default(&ModuleLoaderToolchainConfig::iv_package_pch);
    startup_config_.toolchain = std::move(toolchain);
    loader_toolchain_dirty_ = true;
}

ProjectToolchainTransaction::ProjectToolchainTransaction(PackageJit& owner, EventModuleVisit visit)
    : owner_(&owner), visit_(visit), lock_(owner.mutex_)
{}

ProjectToolchainSettings ProjectToolchainTransaction::record() const
{
    if (!owner_ || !lock_.owns_lock()) throw std::logic_error("toolchain transaction is not open");
    EventModuleVisitScope entry(visit_);
    auto const& value = owner_->startup_config_.toolchain;
    return {value.c_compiler, value.cxx_compiler, value.cmake_program,
        value.cmake_generator, value.make_program, value.juce_dir, value.iv_package_pch};
}

void ProjectToolchainTransaction::commit(ProjectToolchainSettings const& settings)
{
    if (!owner_ || !lock_.owns_lock() || committed_) throw std::logic_error("toolchain transaction commits once");
    EventModuleVisitScope entry(visit_);
    auto value = owner_->startup_config_.toolchain;
    value.c_compiler = settings.c_compiler;
    value.cxx_compiler = settings.cxx_compiler;
    value.cmake_program = settings.cmake_program;
    value.cmake_generator = settings.cmake_generator;
    value.make_program = settings.make_program;
    value.juce_dir = settings.juce_dir;
    value.iv_package_pch = settings.iv_package_pch;
    owner_->set_toolchain_config_locked(std::move(value));
    committed_ = true;
}

void PackageJit::handle_project_graph_toolchain(ProjectToolchainTransactionRequest& request)
{
    if (request.handled) throw std::logic_error("toolchain transaction was handled twice");
    request.handled = true;
    request.transaction = ProjectToolchainTransaction(*this, EventModuleVisit::current<PackageJit>());
}

ModuleLoaderToolchainConfig PackageJit::toolchain_config() const
{
    std::scoped_lock lock(mutex_);
    return startup_config_.toolchain;
}

PackageJitBatchResult PackageJit::build(
    std::vector<IvPackageDeclaration> const& declarations)
{
    PackageJitBatchResult results;
    if (declarations.empty()) return results;
    auto& loader = ensure_loader();
#if IV_ENABLE_JUCE_VST
    warmup_juce_vst_scan_cache();
#endif
    std::vector<PackageBuildRequest> packages;
    packages.reserve(declarations.size());
    for (auto const& declaration : declarations) {
        packages.push_back(PackageBuildRequest{
            .package_root = declaration.package_root,
            .provenance = declaration.provenance,
            .artifact_root = declaration.artifact_root,
        });
    }
    auto batch = loader.load_packages(packages);

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
                .provenance = declaration.provenance,
                .compiler_artifact = std::move(loaded_package.compiler_artifact),
                .package_code = std::move(loaded_package.package_code),
                .node_code_records = std::move(loaded_package.node_code_records),
                .node_code_retained_globals =
                    std::move(loaded_package.node_code_retained_globals),
                .provider_definitions = std::move(loaded_package.provider_definitions),
                .node_compiler_records = std::move(loaded_package.node_compiler_records),
                .config_pointer_fields = std::move(loaded_package.config_pointer_fields),
                .retained_globals = std::move(loaded_package.retained_globals),
                .node_message_handlers =
                    std::move(loaded_package.node_message_handlers),
                .module_message_handlers =
                    std::move(loaded_package.module_message_handlers),
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
                .provenance = declaration.provenance,
                .message = describe_exception(std::current_exception()),
            });
        }
    }
    return results;
}

void PackageJit::handle_build_request(PackageJitBatchRequest& request)
{
    try {
        if (!request.removed_package_roots.empty()) {
            // Removing cached packages does not need a new build toolchain.
            std::scoped_lock lock(mutex_);
            if (loader_) {
                for (auto const& package_root : request.removed_package_roots) loader_->remove_package(package_root);
            }
        }
        request.result = build(request.declarations);
    } catch (std::exception const& error) {
        // Invalid accepted toolchain settings fail the requested builds rather
        // than changing the accepted settings or discarding loaded revisions.
        request.result = {};
        for (auto const& declaration : request.declarations) request.result.failed.push_back({
            .package_id = declaration.package_id, .package_root = declaration.package_root,
            .provenance = declaration.provenance, .message = error.what(),
        });
        if (request.declarations.empty()) throw;
    }
}

void PackageJit::handle_project_override_settings(
    ProjectOverrideSettingsRequest const& request)
{
    bool touched = false;
    auto toolchain = toolchain_config();
    auto const assign_path = [&](std::optional<std::filesystem::path> const& value,
                                 std::optional<std::filesystem::path>
                                     ModuleLoaderToolchainConfig::*field,
                                 ProjectSettingField key) {
        if ((!value.has_value() && !request.specifies(key)) || value == toolchain.*field) return;
        toolchain.*field = value;
        touched = true;
    };
    auto const assign_string = [&](std::optional<std::string> const& value,
                                   std::optional<std::string>
                                       ModuleLoaderToolchainConfig::*field,
                                   ProjectSettingField key) {
        if ((!value.has_value() && !request.specifies(key)) || value == toolchain.*field) return;
        toolchain.*field = value;
        touched = true;
    };

    assign_path(request.c_compiler, &ModuleLoaderToolchainConfig::c_compiler, ProjectSettingField::c_compiler);
    assign_path(request.cxx_compiler, &ModuleLoaderToolchainConfig::cxx_compiler, ProjectSettingField::cxx_compiler);
    assign_path(request.cmake_program, &ModuleLoaderToolchainConfig::cmake_program, ProjectSettingField::cmake_program);
    assign_string(request.cmake_generator, &ModuleLoaderToolchainConfig::cmake_generator, ProjectSettingField::cmake_generator);
    assign_path(request.make_program, &ModuleLoaderToolchainConfig::make_program, ProjectSettingField::make_program);
    assign_path(request.juce_dir, &ModuleLoaderToolchainConfig::juce_dir, ProjectSettingField::juce_dir);
    assign_path(request.iv_package_pch, &ModuleLoaderToolchainConfig::iv_package_pch, ProjectSettingField::iv_package_pch);
    if (!touched) return;

    // Only project replay overrides settings, and it restores the saved
    // project: no project state changes.
    set_toolchain_config(std::move(toolchain));
}

void PackageJit::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder& builder) const
{
    builder.add_project_toolchain_config(toolchain_config());
}
} // namespace iv
