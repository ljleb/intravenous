#include <intravenous/module/package_registration.h>

#include <intravenous/graph/builder.h>
#include <intravenous/module/builder_session.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace iv::details {
namespace {
class ModuleStackEntry {
    BuilderSession* session_ = nullptr;
public:
    ModuleStackEntry(BuilderSession* session, std::string_view id)
        : session_(session)
    {
        begin_builder_module(session_, id);
    }
    ModuleStackEntry(ModuleStackEntry const&) = delete;
    ModuleStackEntry& operator=(ModuleStackEntry const&) = delete;
    ~ModuleStackEntry() { end_builder_module(session_); }
};

class PackageSelection {
    BuilderSession* session_ = nullptr;
    std::size_t previous_ = static_cast<std::size_t>(-1);
public:
    PackageSelection(BuilderSession* session, std::size_t package_index)
        : session_(session), previous_(builder_selected_package(session))
    {
        select_builder_package(session_, package_index);
    }
    PackageSelection(PackageSelection const&) = delete;
    PackageSelection& operator=(PackageSelection const&) = delete;
    ~PackageSelection()
    {
        restore_builder_package(session_, previous_);
    }
};
}

NodeRef configure_registered_definition(
    GraphBuilder& builder,
    std::string_view id,
    std::span<ConfigurationArgument> arguments)
{
    if (!builder._session) {
        throw std::logic_error("registered IV definition requires a BuilderSession");
    }
    auto const found = find_builder_registration(builder._session, id);
    auto const& registration = found.registration;
    if (registration.kind == PackageRegistrationKind::node) {
        if (!arguments.empty()) {
            throw std::invalid_argument(
                "registered node types do not take graph configuration arguments");
        }
        PackageSelection const package(builder._session, found.package_index);
        return registration.node_build(builder);
    }

    ModuleStackEntry const stack_entry(builder._session, id);
    auto child_session = std::unique_ptr<BuilderSession,
        decltype(&iv_builder_session_destroy)>(
            iv_builder_child_session_create(builder._session, found.package_index),
            iv_builder_session_destroy);
    GraphBuilder child(child_session.get());
    registration.module_build(child, arguments);
    return builder.embed_child(child, "Registered IV module");
}
} // namespace iv::details
