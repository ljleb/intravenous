#include <intravenous/graph_jit/port_realization.h>

#include <intravenous/graph/configured_graph.hpp>

#include <optional>
#include <sstream>
#include <stdexcept>

namespace iv::graph_jit {
namespace detail {

struct Variable {
    std::size_t parent = 0;
    ContinuousValueTypeSet domain {};
};

struct BundleVariables {
    std::vector<std::size_t> inputs {};
    std::vector<std::size_t> outputs {};
    NodePortConstraints const* authored_constraints = nullptr;
};

[[nodiscard]] std::string describe(ContinuousValueTypeSet domain)
{
    std::string result = "{";
    bool first = true;
    for (auto const& type : continuous_value_type_registry) {
        if (!domain.contains(type.id)) continue;
        if (!first) result += ", ";
        result.append(type.name);
        first = false;
    }
    result += '}';
    return result;
}

class PortRealizationSolver {
    ConfiguredGraph const& graph_;
    std::vector<Variable> variables_ {};
    std::vector<BundleVariables> bundles_ {};

    [[nodiscard]] std::size_t root(std::size_t variable)
    {
        auto& candidate = variables_.at(variable);
        if (candidate.parent != variable) candidate.parent = root(candidate.parent);
        return candidate.parent;
    }

    [[nodiscard]] std::string port_name(
        NodeBundleHandle bundle,
        PortConstraintDirection direction,
        std::size_t port) const
    {
        auto const& bundles = graph_.node_bundles;
        std::string name;
        if (direction == PortConstraintDirection::input) {
            name = bundles.resolve_sample_input({bundle, PortKind::sample, port})
                .config.name;
        } else {
            name = bundles.resolve_sample_output({bundle, PortKind::sample, port})
                .config.name;
        }
        std::ostringstream message;
        message << "bundle " << bundle << ' '
                << (direction == PortConstraintDirection::input
                        ? "input" : "output")
                << ' ' << port;
        if (!name.empty()) message << " ('" << name << "')";
        return message.str();
    }

    [[nodiscard]] std::expected<std::size_t, std::string> variable(
        NodeBundleHandle bundle,
        PortConstraintDirection direction,
        std::size_t port) const
    {
        if (bundle >= bundles_.size()) {
            return std::unexpected(
                "continuous port constraint references missing bundle "
                + std::to_string(bundle));
        }
        auto const& ports = direction == PortConstraintDirection::input
            ? bundles_[bundle].inputs
            : bundles_[bundle].outputs;
        if (port >= ports.size()) {
            return std::unexpected(
                "continuous port constraint references missing bundle "
                + std::to_string(bundle) + ' '
                + (direction == PortConstraintDirection::input
                    ? "input " : "output ")
                + std::to_string(port));
        }
        return ports[port];
    }

    [[nodiscard]] std::expected<void, std::string> restrict_domain(
        std::size_t variable,
        ContinuousValueTypeSet required,
        std::string context)
    {
        auto const representative = root(variable);
        auto const previous = variables_[representative].domain;
        variables_[representative].domain &= required;
        if (variables_[representative].domain.empty()) {
            return std::unexpected(
                std::move(context)
                + " intersects " + describe(previous) + " with "
                + describe(required)
                + " and leaves no compatible continuous value type");
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> unite(
        std::size_t first,
        std::size_t second,
        std::string context)
    {
        auto first_root = root(first);
        auto second_root = root(second);
        if (first_root == second_root) return {};
        auto const intersection = variables_[first_root].domain
            & variables_[second_root].domain;
        if (intersection.empty()) {
            return std::unexpected(
                std::move(context)
                + " equates incompatible continuous value-type domains "
                + describe(variables_[first_root].domain) + " and "
                + describe(variables_[second_root].domain));
        }
        variables_[second_root].parent = first_root;
        variables_[first_root].domain = intersection;
        return {};
    }

    std::size_t add_variable(ContinuousValueTypeSet domain)
    {
        auto const index = variables_.size();
        variables_.push_back({.parent = index, .domain = domain});
        return index;
    }

    [[nodiscard]] std::expected<void, std::string> initialize()
    {
        bundles_.resize(graph_.node_bundles.size());
        std::size_t visited = 0;
        graph_.node_bundles.for_each_configured_bundle(
            [&](ConfiguredNodeBundleView const& view) {
                auto const handle = visited++;
                auto& result = bundles_[handle];
                result.authored_constraints = view.port_constraints;
                auto const& bundle = graph_.node_bundles.bundle(handle);
                result.inputs.reserve(bundle.sample_input_count());
                result.outputs.reserve(bundle.sample_output_count());
                for (std::size_t port = 0;
                     port < bundle.sample_input_count(); ++port) {
                    auto const config = graph_.node_bundles.resolve_sample_input(
                        {handle, PortKind::sample, port}).config;
                    result.inputs.push_back(add_variable(config.value_types));
                }
                for (std::size_t port = 0;
                     port < bundle.sample_output_count(); ++port) {
                    auto const config = graph_.node_bundles.resolve_sample_output(
                        {handle, PortKind::sample, port}).config;
                    result.outputs.push_back(add_variable(config.value_types));
                }
            });
        if (visited != bundles_.size()) {
            return std::unexpected(
                "configured bundle traversal did not visit every bundle");
        }
        for (std::size_t variable = 0; variable < variables_.size(); ++variable) {
            if (variables_[variable].domain.empty()) {
                return std::unexpected(
                    "continuous port declares an empty value-type domain");
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_authored_constraints()
    {
        for (NodeBundleHandle bundle = 0; bundle < bundles_.size(); ++bundle) {
            auto const* constraints = bundles_[bundle].authored_constraints;
            if (!constraints) continue;
            for (auto const& equality : constraints->value_type_equalities) {
                auto first = variable(
                    bundle, equality.first.direction, equality.first.port_index);
                if (!first) return std::unexpected(std::move(first.error()));
                auto second = variable(
                    bundle, equality.second.direction, equality.second.port_index);
                if (!second) return std::unexpected(std::move(second.error()));
                auto applied = unite(
                    *first,
                    *second,
                    "authored constraint in bundle " + std::to_string(bundle));
                if (!applied) return applied;
            }
            for (auto const& anchor : constraints->value_type_anchors) {
                if (!is_registered_continuous_value_type(anchor.value_type)) {
                    return std::unexpected(
                        "bundle " + std::to_string(bundle)
                        + " anchors an unregistered continuous value type");
                }
                auto target = variable(
                    bundle, anchor.port.direction, anchor.port.port_index);
                if (!target) return std::unexpected(std::move(target.error()));
                auto applied = restrict_domain(
                    *target,
                    ContinuousValueTypeSet(anchor.value_type),
                    "authored constraint for "
                        + port_name(
                            bundle,
                            anchor.port.direction,
                            anchor.port.port_index));
                if (!applied) return applied;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_bundle_constraints()
    {
        for (NodeBundleHandle handle = 0; handle < bundles_.size(); ++handle) {
            auto const& bundle = graph_.node_bundles.bundle(handle);
            if (bundle.is_tiled()) {
                for (auto variable : bundles_[handle].inputs) {
                    auto restricted = restrict_domain(
                        variable,
                        ContinuousValueTypeSet::audio_capable(),
                        "tiling constraint on bundle " + std::to_string(handle));
                    if (!restricted) return restricted;
                }
                for (auto variable : bundles_[handle].outputs) {
                    auto restricted = restrict_domain(
                        variable,
                        ContinuousValueTypeSet::audio_capable(),
                        "tiling constraint on bundle " + std::to_string(handle));
                    if (!restricted) return restricted;
                }
                for (auto const member : bundle.tiled_members()) {
                    if (member >= bundles_.size()) {
                        return std::unexpected(
                            "tiled bundle references a missing member");
                    }
                    if (bundles_[member].inputs.size()
                            != bundles_[handle].inputs.size()
                        || bundles_[member].outputs.size()
                            != bundles_[handle].outputs.size()) {
                        return std::unexpected(
                            "tiled bundle member has incompatible continuous port shape");
                    }
                    for (std::size_t port = 0;
                         port < bundles_[handle].inputs.size(); ++port) {
                        auto applied = unite(
                            bundles_[handle].inputs[port],
                            bundles_[member].inputs[port],
                            "tiled input type propagation from bundle "
                                + std::to_string(handle));
                        if (!applied) return applied;
                    }
                    for (std::size_t port = 0;
                         port < bundles_[handle].outputs.size(); ++port) {
                        auto applied = unite(
                            bundles_[handle].outputs[port],
                            bundles_[member].outputs[port],
                            "tiled output type propagation from bundle "
                                + std::to_string(handle));
                        if (!applied) return applied;
                    }
                }
            }
            if (bundle.is_subgraph()) {
                auto const boundary = bundle.subgraph_boundary_handle();
                if (!boundary || *boundary >= bundles_.size()) {
                    return std::unexpected(
                        "subgraph bundle references a missing boundary");
                }
                // A graph boundary is viewed from inside its child graph:
                // public inputs are boundary outputs and public outputs are
                // boundary inputs.  The enclosing subgraph reverses those
                // directions back to its authored interface.
                if (bundles_[*boundary].outputs.size()
                        != bundles_[handle].inputs.size()
                    || bundles_[*boundary].inputs.size()
                        != bundles_[handle].outputs.size()) {
                    return std::unexpected(
                        "subgraph boundary has incompatible continuous port shape");
                }
                for (std::size_t port = 0;
                     port < bundles_[handle].inputs.size(); ++port) {
                    auto applied = unite(
                        bundles_[handle].inputs[port],
                        bundles_[*boundary].outputs[port],
                        "subgraph input type propagation from bundle "
                            + std::to_string(handle));
                    if (!applied) return applied;
                }
                for (std::size_t port = 0;
                     port < bundles_[handle].outputs.size(); ++port) {
                    auto applied = unite(
                        bundles_[handle].outputs[port],
                        bundles_[*boundary].inputs[port],
                        "subgraph output type propagation from bundle "
                            + std::to_string(handle));
                    if (!applied) return applied;
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_connection_constraints()
    {
        for (auto const& connection :
             graph_.connections.configured_sample_connections()) {
            std::optional<std::size_t> representative;
            auto connect = [&](std::size_t candidate, std::string context)
                -> std::expected<void, std::string> {
                if (!representative) {
                    representative = candidate;
                    return {};
                }
                return unite(*representative, candidate, std::move(context));
            };
            for (auto const& source : connection.source_channels) {
                auto candidate = variable(
                    source.bundle,
                    PortConstraintDirection::output,
                    source.port);
                if (!candidate) {
                    return std::unexpected(std::move(candidate.error()));
                }
                auto applied = connect(
                    *candidate,
                    "continuous connection from bundle "
                        + std::to_string(source.bundle));
                if (!applied) return applied;
            }
            for (auto const& target : connection.target_channels) {
                auto candidate = variable(
                    target.bundle,
                    PortConstraintDirection::input,
                    target.port);
                if (!candidate) {
                    return std::unexpected(std::move(candidate.error()));
                }
                auto applied = connect(
                    *candidate,
                    "continuous connection to bundle "
                        + std::to_string(target.bundle));
                if (!applied) return applied;
            }
            if (!representative) {
                return std::unexpected(
                    "configured continuous connection contains no ports");
            }
        }
        return {};
    }

public:
    explicit PortRealizationSolver(ConfiguredGraph const& graph)
        : graph_(graph)
    {}

    [[nodiscard]] std::expected<ResolvedPortRealization, std::string> solve()
    {
        if (auto result = initialize(); !result) {
            return std::unexpected(std::move(result.error()));
        }
        if (auto result = apply_authored_constraints(); !result) {
            return std::unexpected(std::move(result.error()));
        }
        if (auto result = apply_bundle_constraints(); !result) {
            return std::unexpected(std::move(result.error()));
        }
        if (auto result = apply_connection_constraints(); !result) {
            return std::unexpected(std::move(result.error()));
        }

        std::vector<ResolvedPortRealization::Bundle> resolved(bundles_.size());
        for (NodeBundleHandle bundle = 0; bundle < bundles_.size(); ++bundle) {
            auto resolve = [&](std::size_t variable,
                               PortConstraintDirection direction,
                               std::size_t port)
                -> std::expected<ResolvedContinuousPortRealization, std::string> {
                auto const domain = variables_[root(variable)].domain;
                auto const selected = domain.sole_type();
                if (!selected) {
                    return std::unexpected(
                        port_name(bundle, direction, port)
                        + " has unresolved continuous value-type alternatives "
                        + describe(domain));
                }
                return ResolvedContinuousPortRealization{
                    .value_type = *selected,
                };
            };
            for (std::size_t port = 0;
                 port < bundles_[bundle].inputs.size(); ++port) {
                auto value = resolve(
                    bundles_[bundle].inputs[port],
                    PortConstraintDirection::input,
                    port);
                if (!value) return std::unexpected(std::move(value.error()));
                resolved[bundle].inputs.push_back(*value);
            }
            for (std::size_t port = 0;
                 port < bundles_[bundle].outputs.size(); ++port) {
                auto value = resolve(
                    bundles_[bundle].outputs[port],
                    PortConstraintDirection::output,
                    port);
                if (!value) return std::unexpected(std::move(value.error()));
                resolved[bundle].outputs.push_back(*value);
            }
        }
        return ResolvedPortRealization(std::move(resolved));
    }
};

} // namespace detail

ResolvedContinuousPortRealization const& ResolvedPortRealization::input(
    NodeBundleHandle bundle,
    std::size_t port) const
{
    if (bundle >= bundles_.size() || port >= bundles_[bundle].inputs.size()) {
        throw std::out_of_range(
            "resolved continuous input port is out of bounds");
    }
    return bundles_[bundle].inputs[port];
}

ResolvedContinuousPortRealization const& ResolvedPortRealization::output(
    NodeBundleHandle bundle,
    std::size_t port) const
{
    if (bundle >= bundles_.size() || port >= bundles_[bundle].outputs.size()) {
        throw std::out_of_range(
            "resolved continuous output port is out of bounds");
    }
    return bundles_[bundle].outputs[port];
}

std::expected<ResolvedPortRealization, std::string>
resolve_port_realization(ConfiguredGraph const& graph)
{
    return detail::PortRealizationSolver(graph).solve();
}

} // namespace iv::graph_jit
