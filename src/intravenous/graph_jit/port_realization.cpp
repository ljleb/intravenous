#include <intravenous/graph_jit/port_realization.h>

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/global_index.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace iv::graph_jit {
namespace detail {

[[nodiscard]] std::expected<PortPace, std::string> add_exponents(
    PortPace first,
    PortPace second)
{
    using Exponent = PortPace::exponent_type;
    auto const result = static_cast<std::int64_t>(first.exponent())
        + static_cast<std::int64_t>(second.exponent());
    if (result < std::numeric_limits<Exponent>::min()
        || result > std::numeric_limits<Exponent>::max()) {
        return std::unexpected("dyadic port-pace exponent exceeds its representation");
    }
    return PortPace::from_exponent(static_cast<Exponent>(result));
}

[[nodiscard]] std::expected<PortPace, std::string> subtract_exponents(
    PortPace first,
    PortPace second)
{
    using Exponent = PortPace::exponent_type;
    auto const result = static_cast<std::int64_t>(first.exponent())
        - static_cast<std::int64_t>(second.exponent());
    if (result < std::numeric_limits<Exponent>::min()
        || result > std::numeric_limits<Exponent>::max()) {
        return std::unexpected("dyadic port-pace exponent exceeds its representation");
    }
    return PortPace::from_exponent(static_cast<Exponent>(result));
}

[[nodiscard]] std::expected<void, std::string> require_supported_exponent(
    PortPace pace,
    std::string context)
{
    if (pace.exponent() < FractionalGlobalIndexRange::minimum_pace_exponent
        || pace.exponent() > FractionalGlobalIndexRange::maximum_pace_exponent) {
        return std::unexpected(
            std::move(context) + " has dyadic pace exponent "
            + std::to_string(pace.exponent())
            + "; the Q64.64 execution coordinate supports exponents from -64 through +64");
    }
    return {};
}

struct TypeVariable {
    std::size_t parent = 0;
    ContinuousValueTypeSet domain {};
};

template<class T>
struct ScalarVariable {
    std::size_t parent = 0;
    std::optional<T> value {};
};

template<class T, bool JoinAnchors = false>
class ScalarEquivalenceSolver {
    std::vector<ScalarVariable<T>> variables_ {};

    [[nodiscard]] std::size_t root(std::size_t variable)
    {
        auto& candidate = variables_.at(variable);
        if (candidate.parent != variable) candidate.parent = root(candidate.parent);
        return candidate.parent;
    }

    [[nodiscard]] static T joined(T first, T second)
    {
        static_assert(JoinAnchors);
        return std::max(first, second);
    }

public:
    [[nodiscard]] std::size_t add()
    {
        auto const result = variables_.size();
        variables_.push_back({.parent = result});
        return result;
    }

    [[nodiscard]] std::expected<void, std::string> unite(
        std::size_t first,
        std::size_t second,
        std::string context)
    {
        auto first_root = root(first);
        auto second_root = root(second);
        if (first_root == second_root) return {};
        auto first_value = variables_[first_root].value;
        auto second_value = variables_[second_root].value;
        if constexpr (!JoinAnchors) {
            if (first_value && second_value && *first_value != *second_value) {
                return std::unexpected(std::move(context) + " equates conflicting constants");
            }
        }
        variables_[second_root].parent = first_root;
        if constexpr (JoinAnchors) {
            if (first_value && second_value)
                variables_[first_root].value = joined(*first_value, *second_value);
            else if (second_value)
                variables_[first_root].value = second_value;
        } else if (!first_value) {
            variables_[first_root].value = second_value;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> anchor(
        std::size_t variable,
        T value,
        std::string context)
    {
        auto const representative = root(variable);
        auto& existing = variables_[representative].value;
        if constexpr (JoinAnchors) {
            existing = existing ? joined(*existing, value) : value;
        } else if (existing && *existing != value) {
            return std::unexpected(std::move(context) + " supplies conflicting constants");
        } else {
            existing = value;
        }
        return {};
    }

    [[nodiscard]] std::optional<T> value(std::size_t variable)
    {
        return variables_[root(variable)].value;
    }
};

class WeightedRateSolver {
    struct Variable {
        std::size_t parent = 0;
        // exponent(this) - exponent(parent)
        PortPace parent_difference {};
        // Present only on a root. This is the absolute execution pace of the
        // root itself, not of an arbitrary member of the component.
        std::optional<PortPace> absolute_rate {};
    };
    std::vector<Variable> variables_ {};

    struct RootAndDifference {
        std::size_t root = 0;
        PortPace difference {};
    };

    [[nodiscard]] std::expected<RootAndDifference, std::string> find(
        std::size_t variable)
    {
        auto& current = variables_.at(variable);
        if (current.parent == variable)
            return RootAndDifference{variable, PortPace{}};
        auto parent = find(current.parent);
        if (!parent) return std::unexpected(std::move(parent.error()));
        auto difference = add_exponents(
            current.parent_difference, parent->difference);
        if (!difference) return std::unexpected(std::move(difference.error()));
        current.parent = parent->root;
        current.parent_difference = *difference;
        return RootAndDifference{parent->root, *difference};
    }

public:
    explicit WeightedRateSolver(std::size_t count)
    {
        variables_.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
            variables_.push_back({.parent = index});
    }

    // Constrain exponent(second) - exponent(first) == difference.
    [[nodiscard]] std::expected<void, std::string> unite(
        std::size_t first,
        std::size_t second,
        PortPace difference,
        std::string context)
    {
        auto first_found = find(first);
        if (!first_found) return std::unexpected(std::move(first_found.error()));
        auto second_found = find(second);
        if (!second_found) return std::unexpected(std::move(second_found.error()));
        if (first_found->root == second_found->root) {
            auto existing = subtract_exponents(
                second_found->difference, first_found->difference);
            if (!existing) return std::unexpected(std::move(existing.error()));
            if (*existing != difference)
                return std::unexpected(std::move(context) + " creates an inconsistent pace cycle");
            return {};
        }
        auto root_difference = add_exponents(difference, first_found->difference);
        if (!root_difference)
            return std::unexpected(std::move(root_difference.error()));
        root_difference = subtract_exponents(
            *root_difference, second_found->difference);
        if (!root_difference)
            return std::unexpected(std::move(root_difference.error()));
        auto& first_root = variables_[first_found->root];
        auto& second_root = variables_[second_found->root];
        if (second_root.absolute_rate) {
            auto translated = subtract_exponents(
                *second_root.absolute_rate, *root_difference);
            if (!translated)
                return std::unexpected(std::move(translated.error()));
            if (first_root.absolute_rate
                && *first_root.absolute_rate != *translated) {
                return std::unexpected(
                    std::move(context) + " conflicts with absolute pace anchors");
            }
            first_root.absolute_rate = *translated;
        }
        second_root.parent = first_found->root;
        second_root.parent_difference = *root_difference;
        second_root.absolute_rate.reset();
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> anchor(
        std::size_t variable,
        PortPace absolute_rate,
        std::string context)
    {
        auto found = find(variable);
        if (!found) return std::unexpected(std::move(found.error()));
        auto root_rate = subtract_exponents(absolute_rate, found->difference);
        if (!root_rate)
            return std::unexpected(std::move(root_rate.error()));
        auto& existing = variables_[found->root].absolute_rate;
        if (existing && *existing != *root_rate) {
            return std::unexpected(
                std::move(context) + " conflicts with another absolute pace anchor");
        }
        existing = *root_rate;
        return {};
    }

    [[nodiscard]] std::expected<void, std::string>
    default_unanchored_components()
    {
        // Union roots and bundle handles are construction details and therefore
        // cannot define graph semantics. Normalize every wholly unanchored
        // component so its slowest logical node executes at project pace one.
        // This fixes only the remaining uniform scale; every solved relative
        // difference remains unchanged.
        std::vector<std::optional<PortPace>> minimum_difference(
            variables_.size());
        for (std::size_t variable = 0; variable < variables_.size(); ++variable) {
            auto found = find(variable);
            if (!found) return std::unexpected(std::move(found.error()));
            auto& minimum = minimum_difference[found->root];
            if (!minimum
                || found->difference.exponent() < minimum->exponent()) {
                minimum = found->difference;
            }
        }
        for (std::size_t root = 0; root < variables_.size(); ++root) {
            if (!minimum_difference[root] || variables_[root].absolute_rate)
                continue;
            auto rate = subtract_exponents(
                PortPace{}, *minimum_difference[root]);
            if (!rate) return std::unexpected(std::move(rate.error()));
            variables_[root].absolute_rate = *rate;
        }
        return {};
    }

    [[nodiscard]] std::expected<PortPace, std::string> rate(std::size_t variable)
    {
        auto found = find(variable);
        if (!found) return std::unexpected(std::move(found.error()));
        return add_exponents(
            variables_[found->root].absolute_rate.value(),
            found->difference);
    }
};

struct PortVariables {
    std::optional<std::size_t> value_type {};
    std::optional<std::size_t> size {};
    std::optional<std::size_t> pace {};
    std::optional<std::size_t> history {};
    std::optional<std::size_t> latency {};
    std::optional<std::size_t> ordering {};
};

struct BundleVariables {
    std::vector<PortVariables> inputs {};
    std::vector<PortVariables> outputs {};
    std::vector<PortVariables> event_inputs {};
    std::vector<PortVariables> event_outputs {};
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
    std::vector<TypeVariable> type_variables_ {};
    ScalarEquivalenceSolver<std::size_t> sizes_ {};
    ScalarEquivalenceSolver<PortPace> paces_ {};
    ScalarEquivalenceSolver<std::size_t> histories_ {};
    ScalarEquivalenceSolver<std::size_t> latencies_ {};
    ScalarEquivalenceSolver<CoverageOrdering, true> orderings_ {};
    std::vector<BundleVariables> bundles_ {};

    [[nodiscard]] std::size_t type_root(std::size_t variable)
    {
        auto& candidate = type_variables_.at(variable);
        if (candidate.parent != variable) candidate.parent = type_root(candidate.parent);
        return candidate.parent;
    }

    [[nodiscard]] std::size_t add_type_variable(ContinuousValueTypeSet domain)
    {
        auto const index = type_variables_.size();
        type_variables_.push_back({.parent = index, .domain = domain});
        return index;
    }

    [[nodiscard]] std::expected<void, std::string> unite_types(
        std::size_t first,
        std::size_t second,
        std::string context)
    {
        auto first_root = type_root(first);
        auto second_root = type_root(second);
        if (first_root == second_root) return {};
        auto const intersection = type_variables_[first_root].domain
            & type_variables_[second_root].domain;
        if (intersection.empty()) {
            return std::unexpected(
                std::move(context) + " equates incompatible continuous value-type domains "
                + describe(type_variables_[first_root].domain) + " and "
                + describe(type_variables_[second_root].domain));
        }
        type_variables_[second_root].parent = first_root;
        type_variables_[first_root].domain = intersection;
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> restrict_type(
        std::size_t variable,
        ContinuousValueTypeSet required,
        std::string context)
    {
        auto const representative = type_root(variable);
        auto const previous = type_variables_[representative].domain;
        type_variables_[representative].domain &= required;
        if (type_variables_[representative].domain.empty()) {
            return std::unexpected(
                std::move(context) + " intersects " + describe(previous)
                + " with " + describe(required)
                + " and leaves no compatible continuous value type");
        }
        return {};
    }

    [[nodiscard]] std::expected<PortVariables*, std::string> port(
        NodeBundleHandle bundle,
        PortConstraintRef ref)
    {
        if (bundle >= bundles_.size())
            return std::unexpected("port constraint references a missing bundle");
        auto& b = bundles_[bundle];
        auto* ports = &b.inputs;
        if (ref.kind == PortKind::sample) {
            ports = ref.direction == PortConstraintDirection::input
                ? &b.inputs : &b.outputs;
        } else if (ref.kind == PortKind::event) {
            ports = ref.direction == PortConstraintDirection::input
                ? &b.event_inputs : &b.event_outputs;
        } else {
            return std::unexpected("port constraint references an invalid port kind");
        }
        if (ref.port_index >= ports->size())
            return std::unexpected("port constraint references a missing port");
        return &(*ports)[ref.port_index];
    }

    [[nodiscard]] std::string port_name(
        NodeBundleHandle bundle,
        PortConstraintRef ref) const
    {
        NodeBundlePortId const id{bundle, ref.kind, ref.port_index};
        std::string name;
        if (ref.kind == PortKind::sample) {
            name = ref.direction == PortConstraintDirection::input
                ? graph_.node_bundles.resolve_sample_input(id).config.name
                : graph_.node_bundles.resolve_sample_output(id).config.name;
        } else {
            name = ref.direction == PortConstraintDirection::input
                ? graph_.node_bundles.resolve_event_input(id).config.name
                : graph_.node_bundles.resolve_event_output(id).config.name;
        }
        std::ostringstream out;
        out << "bundle " << bundle << ' '
            << (ref.direction == PortConstraintDirection::input ? "input " : "output ")
            << ref.port_index;
        if (!name.empty()) out << " ('" << name << "')";
        return out.str();
    }

    template<class Constraint, class Getter, class Solver>
    [[nodiscard]] std::expected<void, std::string> apply_equalities(
        NodeBundleHandle bundle,
        std::vector<Constraint> const& constraints,
        Getter getter,
        Solver& solver,
        std::string_view property)
    {
        for (auto const& equality : constraints) {
            auto first_port = port(bundle, equality.first);
            if (!first_port) return std::unexpected(std::move(first_port.error()));
            auto second_port = port(bundle, equality.second);
            if (!second_port) return std::unexpected(std::move(second_port.error()));
            auto first = getter(**first_port);
            auto second = getter(**second_port);
            if (!first || !second)
                return std::unexpected("invalid " + std::string(property) + " constraint for this port contract");
            auto result = solver.unite(
                *first, *second,
                "authored " + std::string(property) + " constraint in bundle "
                    + std::to_string(bundle));
            if (!result) return result;
        }
        return {};
    }

    template<class Constraint, class Getter, class Solver>
    [[nodiscard]] std::expected<void, std::string> apply_anchors(
        NodeBundleHandle bundle,
        std::vector<Constraint> const& constraints,
        Getter getter,
        Solver& solver,
        std::string_view property)
    {
        for (auto const& anchor : constraints) {
            auto target_port = port(bundle, anchor.port);
            if (!target_port) return std::unexpected(std::move(target_port.error()));
            auto target = getter(**target_port);
            if (!target)
                return std::unexpected("invalid " + std::string(property) + " constraint for " + port_name(bundle, anchor.port));
            auto result = solver.anchor(
                *target, anchor.value,
                "authored " + std::string(property) + " constraint for "
                    + port_name(bundle, anchor.port));
            if (!result) return result;
        }
        return {};
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
                for (std::size_t index = 0; index < bundle.sample_input_count(); ++index) {
                    auto const config = graph_.node_bundles.resolve_sample_input(
                        {handle, PortKind::sample, index}).config;
                    result.inputs.push_back({
                        .value_type = add_type_variable(
                            continuous_value_type_domain(config)),
                        .size = sizes_.add(),
                        .pace = paces_.add(),
                        .history = is_sequential(config) ? std::optional(histories_.add()) : std::nullopt,
                        .ordering = input_access(config) == InputAccessKind::coverage
                            ? std::optional(orderings_.add()) : std::nullopt,
                    });
                }
                for (std::size_t index = 0; index < bundle.sample_output_count(); ++index) {
                    auto const config = graph_.node_bundles.resolve_sample_output(
                        {handle, PortKind::sample, index}).config;
                    result.outputs.push_back({
                        .value_type = add_type_variable(
                            continuous_value_type_domain(config)),
                        .size = sizes_.add(),
                        .pace = paces_.add(),
                        .history = is_tick(config) ? std::optional(histories_.add()) : std::nullopt,
                        .latency = is_tick(config) ? std::optional(latencies_.add()) : std::nullopt,
                        .ordering = output_production(config) == OutputProductionKind::coverage
                            ? std::optional(orderings_.add()) : std::nullopt,
                    });
                }
                for (std::size_t index = 0; index < bundle.event_input_count(); ++index) {
                    auto const config = graph_.node_bundles.resolve_event_input(
                        {handle, PortKind::event, index}).config;
                    result.event_inputs.push_back({
                        .history = is_sequential(config) ? std::optional(histories_.add()) : std::nullopt,
                        .ordering = input_access(config) == InputAccessKind::coverage
                            ? std::optional(orderings_.add()) : std::nullopt,
                    });
                }
                for (std::size_t index = 0; index < bundle.event_output_count(); ++index) {
                    auto const config = graph_.node_bundles.resolve_event_output(
                        {handle, PortKind::event, index}).config;
                    result.event_outputs.push_back({
                        .history = is_tick(config) ? std::optional(histories_.add()) : std::nullopt,
                        .latency = is_tick(config) ? std::optional(latencies_.add()) : std::nullopt,
                        .ordering = output_production(config) == OutputProductionKind::coverage
                            ? std::optional(orderings_.add()) : std::nullopt,
                    });
                }
            });
        if (visited != bundles_.size())
            return std::unexpected("configured bundle traversal did not visit every bundle");
        for (auto const& variable : type_variables_)
            if (variable.domain.empty())
                return std::unexpected("continuous port declares an empty value-type domain");
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_authored_constraints()
    {
        auto member = [](auto member_pointer) {
            return [=](PortVariables const& variables) { return variables.*member_pointer; };
        };
        for (NodeBundleHandle bundle = 0; bundle < bundles_.size(); ++bundle) {
            auto const* c = bundles_[bundle].authored_constraints;
            if (!c) continue;
            for (auto const& equality : c->value_type_equalities) {
                auto first_port = port(bundle, equality.first);
                if (!first_port) return std::unexpected(std::move(first_port.error()));
                auto second_port = port(bundle, equality.second);
                if (!second_port) return std::unexpected(std::move(second_port.error()));
                if (!(**first_port).value_type || !(**second_port).value_type)
                    return std::unexpected("value-type constraint requires continuous ports");
                auto result = unite_types(
                    *(**first_port).value_type, *(**second_port).value_type,
                    "authored value-type constraint in bundle " + std::to_string(bundle));
                if (!result) return result;
            }
            for (auto const& anchor : c->value_type_anchors) {
                if (!is_registered_continuous_value_type(anchor.value))
                    return std::unexpected("bundle anchors an unregistered continuous value type");
                auto target_port = port(bundle, anchor.port);
                if (!target_port) return std::unexpected(std::move(target_port.error()));
                if (!(**target_port).value_type)
                    return std::unexpected("value-type anchor requires a continuous port");
                auto result = restrict_type(
                    *(**target_port).value_type,
                    ContinuousValueTypeSet(anchor.value),
                    "authored value-type constraint for " + port_name(bundle, anchor.port));
                if (!result) return result;
            }
#define IV_APPLY_SCALAR(property, field, solver)                          \
            if (auto result = apply_equalities(                           \
                    bundle, c->property##_equalities, member(&PortVariables::field), solver, #property); !result) return result; \
            if (auto result = apply_anchors(                              \
                    bundle, c->property##_anchors, member(&PortVariables::field), solver, #property); !result) return result
            IV_APPLY_SCALAR(size, size, sizes_);
            IV_APPLY_SCALAR(pace, pace, paces_);
            IV_APPLY_SCALAR(history, history, histories_);
            IV_APPLY_SCALAR(latency, latency, latencies_);
            IV_APPLY_SCALAR(ordering, ordering, orderings_);
#undef IV_APPLY_SCALAR
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> equate_continuous_ports(
        PortVariables const& first,
        PortVariables const& second,
        std::string context,
        bool include_local_pace)
    {
        if (!first.value_type || !second.value_type || !first.size || !second.size)
            return std::unexpected(std::move(context) + " references a non-continuous port");
        if (auto result = unite_types(*first.value_type, *second.value_type, context); !result)
            return result;
        if (auto result = sizes_.unite(*first.size, *second.size, context); !result)
            return result;
        if (include_local_pace) {
            if (!first.pace || !second.pace)
                return std::unexpected(std::move(context) + " has no pace variable");
            if (auto result = paces_.unite(*first.pace, *second.pace, context); !result)
                return result;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> equate_matching_contracts(
        PortVariables const& first,
        PortVariables const& second,
        std::string const& context,
        bool include_latency)
    {
        auto unite_optional = [&](auto& solver, auto first_value, auto second_value,
                                  std::string_view property)
            -> std::expected<void, std::string> {
            if (!first_value || !second_value) return {};
            return solver.unite(
                *first_value, *second_value,
                context + " " + std::string(property));
        };
        if (auto result = unite_optional(
                histories_, first.history, second.history, "history"); !result)
            return result;
        if (include_latency) {
            if (auto result = unite_optional(
                    latencies_, first.latency, second.latency, "latency"); !result)
                return result;
        }
        if (auto result = unite_optional(
                orderings_, first.ordering, second.ordering, "ordering"); !result)
            return result;
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_bundle_constraints()
    {
        for (NodeBundleHandle handle = 0; handle < bundles_.size(); ++handle) {
            auto const& bundle = graph_.node_bundles.bundle(handle);
            if (bundle.is_tiled()) {
                for (auto const& input : bundles_[handle].inputs) {
                    auto result = restrict_type(
                        *input.value_type, ContinuousValueTypeSet::audio_capable(),
                        "tiling constraint on bundle " + std::to_string(handle));
                    if (!result) return result;
                }
                for (auto const& output : bundles_[handle].outputs) {
                    auto result = restrict_type(
                        *output.value_type, ContinuousValueTypeSet::audio_capable(),
                        "tiling constraint on bundle " + std::to_string(handle));
                    if (!result) return result;
                }
                for (auto const member : bundle.tiled_members()) {
                    if (member >= bundles_.size()
                        || bundles_[member].inputs.size() != bundles_[handle].inputs.size()
                        || bundles_[member].outputs.size() != bundles_[handle].outputs.size())
                        return std::unexpected("tiled bundle member has incompatible continuous port shape");
                    for (std::size_t port = 0; port < bundles_[handle].inputs.size(); ++port) {
                        auto result = equate_continuous_ports(
                            bundles_[handle].inputs[port], bundles_[member].inputs[port],
                            "tiled input propagation", true);
                        if (!result) return result;
                        result = equate_matching_contracts(
                            bundles_[handle].inputs[port], bundles_[member].inputs[port],
                            "tiled input", true);
                        if (!result) return result;
                    }
                    for (std::size_t port = 0; port < bundles_[handle].outputs.size(); ++port) {
                        auto result = equate_continuous_ports(
                            bundles_[handle].outputs[port], bundles_[member].outputs[port],
                            "tiled output propagation", true);
                        if (!result) return result;
                        result = equate_matching_contracts(
                            bundles_[handle].outputs[port], bundles_[member].outputs[port],
                            "tiled output", true);
                        if (!result) return result;
                    }
                    if (bundles_[member].event_inputs.size()
                            != bundles_[handle].event_inputs.size()
                        || bundles_[member].event_outputs.size()
                            != bundles_[handle].event_outputs.size())
                        return std::unexpected("tiled bundle member has incompatible event port shape");
                    for (std::size_t port = 0; port < bundles_[handle].event_inputs.size(); ++port) {
                        auto result = equate_matching_contracts(
                            bundles_[handle].event_inputs[port], bundles_[member].event_inputs[port],
                            "tiled event input", true);
                        if (!result) return result;
                    }
                    for (std::size_t port = 0; port < bundles_[handle].event_outputs.size(); ++port) {
                        auto result = equate_matching_contracts(
                            bundles_[handle].event_outputs[port], bundles_[member].event_outputs[port],
                            "tiled event output", true);
                        if (!result) return result;
                    }
                }
            }
            if (bundle.is_subgraph()) {
                auto const boundary = bundle.subgraph_boundary_handle();
                if (!boundary || *boundary >= bundles_.size()
                    || bundles_[*boundary].outputs.size() != bundles_[handle].inputs.size()
                    || bundles_[*boundary].inputs.size() != bundles_[handle].outputs.size())
                    return std::unexpected("subgraph boundary has incompatible continuous port shape");
                for (std::size_t port = 0; port < bundles_[handle].inputs.size(); ++port) {
                    auto result = equate_continuous_ports(
                        bundles_[handle].inputs[port], bundles_[*boundary].outputs[port],
                        "subgraph input propagation", true);
                    if (!result) return result;
                    result = equate_matching_contracts(
                        bundles_[handle].inputs[port], bundles_[*boundary].outputs[port],
                        "subgraph input", false);
                    if (!result) return result;
                }
                for (std::size_t port = 0; port < bundles_[handle].outputs.size(); ++port) {
                    auto result = equate_continuous_ports(
                        bundles_[handle].outputs[port], bundles_[*boundary].inputs[port],
                        "subgraph output propagation", true);
                    if (!result) return result;
                    result = equate_matching_contracts(
                        bundles_[handle].outputs[port], bundles_[*boundary].inputs[port],
                        "subgraph output", false);
                    if (!result) return result;
                }
                if (bundles_[*boundary].event_outputs.size()
                        != bundles_[handle].event_inputs.size()
                    || bundles_[*boundary].event_inputs.size()
                        != bundles_[handle].event_outputs.size())
                    return std::unexpected("subgraph boundary has incompatible event port shape");
                for (std::size_t port = 0; port < bundles_[handle].event_inputs.size(); ++port) {
                    auto result = equate_matching_contracts(
                        bundles_[handle].event_inputs[port], bundles_[*boundary].event_outputs[port],
                        "subgraph event input", false);
                    if (!result) return result;
                }
                for (std::size_t port = 0; port < bundles_[handle].event_outputs.size(); ++port) {
                    auto result = equate_matching_contracts(
                        bundles_[handle].event_outputs[port], bundles_[*boundary].event_inputs[port],
                        "subgraph event output", false);
                    if (!result) return result;
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_connection_constraints()
    {
        for (auto const& connection : graph_.connections.configured_sample_connections()) {
            std::optional<PortVariables const*> representative;
            std::optional<std::size_t> ordering_representative;
            auto connect = [&](PortVariables const& candidate) -> std::expected<void, std::string> {
                if (!representative) {
                    representative = &candidate;
                } else {
                    auto result = equate_continuous_ports(
                        **representative, candidate,
                        "continuous connection", false);
                    if (!result) return result;
                }
                if (candidate.ordering) {
                    if (ordering_representative) {
                        return orderings_.unite(
                            *ordering_representative, *candidate.ordering,
                            "continuous connection ordering");
                    }
                    ordering_representative = *candidate.ordering;
                }
                return {};
            };
            for (auto const& source : connection.source_channels) {
                auto result = connect(
                    bundles_.at(source.channel.bundle)
                        .outputs.at(source.channel.port));
                if (!result) return result;
            }
            for (auto const& target : connection.target_channels) {
                auto result = connect(bundles_.at(target.bundle).inputs.at(target.port));
                if (!result) return result;
            }
            if (!representative)
                return std::unexpected("configured continuous connection contains no ports");
        }
        for (auto const& connection : graph_.connections.configured_event_connections()) {
            for (auto const& source : connection.sources) {
                auto const& source_variables =
                    bundles_.at(source.port.bundle).event_outputs.at(source.port.port);
                if (!source_variables.ordering) continue;
                for (auto const& target : connection.targets) {
                    auto const& target_variables =
                        bundles_.at(target.bundle).event_inputs.at(target.port);
                    if (!target_variables.ordering) continue;
                    auto result = orderings_.unite(
                        *source_variables.ordering, *target_variables.ordering,
                        "event connection ordering");
                    if (!result) return result;
                }
            }
        }
        return {};
    }

    [[nodiscard]] PortPace resolved_local_pace(PortVariables const& port)
    {
        return port.pace ? paces_.value(*port.pace).value_or(PortPace{}) : PortPace{};
    }

    [[nodiscard]] std::expected<std::vector<PortPace>, std::string> solve_rates()
    {
        WeightedRateSolver rates(bundles_.size());
        for (NodeBundleHandle handle = 0; handle < bundles_.size(); ++handle) {
            auto const& bundle = graph_.node_bundles.bundle(handle);
            if (bundle.is_tiled()) {
                for (auto member : bundle.tiled_members()) {
                    auto result = rates.unite(handle, member, PortPace{}, "tiled node rate");
                    if (!result) return std::unexpected(std::move(result.error()));
                }
            }
            if (bundle.is_subgraph()) {
                auto result = rates.unite(
                    handle, *bundle.subgraph_boundary_handle(), PortPace{}, "subgraph boundary rate");
                if (!result) return std::unexpected(std::move(result.error()));
            }
        }
        for (auto const& connection : graph_.connections.configured_sample_connections()) {
            for (auto const& source : connection.source_channels) {
                auto const source_pace = resolved_local_pace(
                    bundles_.at(source.channel.bundle)
                        .outputs.at(source.channel.port));
                for (auto const& target : connection.target_channels) {
                    auto const target_pace = resolved_local_pace(
                        bundles_.at(target.bundle).inputs.at(target.port));
                    auto difference = subtract_exponents(source_pace, target_pace);
                    if (!difference)
                        return std::unexpected(std::move(difference.error()));
                    auto result = rates.unite(
                        source.channel.bundle, target.bundle, *difference,
                        "continuous connection pace");
                    if (!result) return std::unexpected(std::move(result.error()));
                }
            }
        }
        for (NodeBundleHandle bundle = 0; bundle < bundles_.size(); ++bundle) {
            auto const* constraints = bundles_[bundle].authored_constraints;
            if (!constraints) continue;
            for (auto const& anchor : constraints->absolute_pace_anchors) {
                auto target = port(bundle, anchor.port);
                if (!target)
                    return std::unexpected(std::move(target.error()));
                if (!(**target).pace) {
                    return std::unexpected(
                        "absolute pace constraint requires a continuous port: "
                        + port_name(bundle, anchor.port));
                }
                auto execution_rate = subtract_exponents(
                    anchor.value, resolved_local_pace(**target));
                if (!execution_rate)
                    return std::unexpected(std::move(execution_rate.error()));
                auto result = rates.anchor(
                    bundle,
                    *execution_rate,
                    "authored absolute pace constraint for "
                        + port_name(bundle, anchor.port));
                if (!result) return std::unexpected(std::move(result.error()));
            }
        }
        if (auto result = rates.default_unanchored_components(); !result)
            return std::unexpected(std::move(result.error()));
        std::vector<PortPace> result;
        result.reserve(bundles_.size());
        for (NodeBundleHandle bundle = 0; bundle < bundles_.size(); ++bundle) {
            auto rate = rates.rate(bundle);
            if (!rate) return std::unexpected(std::move(rate.error()));
            auto supported = require_supported_exponent(
                *rate, "resolved node execution domain");
            if (!supported)
                return std::unexpected(std::move(supported.error()));
            result.push_back(*rate);
        }
        return result;
    }

    [[nodiscard]] std::expected<ResolvedContinuousPortRealization, std::string>
    resolve_continuous(
        NodeBundleHandle bundle,
        PortVariables const& variables,
        PortConstraintDirection direction,
        std::size_t port,
        PortPace execution_rate)
    {
        auto const domain = type_variables_[type_root(*variables.value_type)].domain;
        auto const selected = domain.sole_type();
        PortConstraintRef const ref{direction, PortKind::sample, port};
        if (!selected)
            return std::unexpected(port_name(bundle, ref)
                + " has unresolved continuous value-type alternatives " + describe(domain));
        auto const& value_type = continuous_value_type_info(*selected);
        auto const rank = value_type.rank;
        auto size = sizes_.value(*variables.size);
        if (rank != 0 && !size)
            return std::unexpected(port_name(bundle, ref) + " has an unresolved value size");
        if (rank == 0 && size)
            return std::unexpected(port_name(bundle, ref) + " constrains size for a scalar value type");
        if (!value_type.valid_size || !value_type.valid_size(size)) {
            return std::unexpected(
                port_name(bundle, ref) + " has an invalid size for "
                + std::string(value_type.name));
        }
        auto const sample_support = value_type.sample_support
            ? value_type.sample_support(size)
            : std::nullopt;
        if (value_type.channel_capability
                == ContinuousValueChannelCapability::audio
            && !sample_support) {
            return std::unexpected(
                port_name(bundle, ref) + " has unrepresentable audio sample support for "
                + std::string(value_type.name));
        }
        auto const local_pace = resolved_local_pace(variables);
        auto absolute_pace = add_exponents(execution_rate, local_pace);
        if (!absolute_pace) return std::unexpected(std::move(absolute_pace.error()));
        auto supported = require_supported_exponent(
            *absolute_pace, port_name(bundle, ref));
        if (!supported) return std::unexpected(std::move(supported.error()));
        return ResolvedContinuousPortRealization{
            .value_type = *selected,
            .size = size,
            .sample_support = sample_support,
            .local_pace = local_pace,
            .absolute_pace = *absolute_pace,
            .history = variables.history ? histories_.value(*variables.history).value_or(0) : 0,
            .latency = variables.latency ? latencies_.value(*variables.latency).value_or(0) : 0,
            .ordering = variables.ordering ? orderings_.value(*variables.ordering).value_or(CoverageOrdering::unspecified) : CoverageOrdering::unspecified,
        };
    }

    [[nodiscard]] ResolvedEventPortRealization resolve_event(
        PortVariables const& variables)
    {
        return {
            .history = variables.history ? histories_.value(*variables.history).value_or(0) : 0,
            .latency = variables.latency ? latencies_.value(*variables.latency).value_or(0) : 0,
            .ordering = variables.ordering ? orderings_.value(*variables.ordering).value_or(CoverageOrdering::unspecified) : CoverageOrdering::unspecified,
        };
    }

public:
    explicit PortRealizationSolver(ConfiguredGraph const& graph) : graph_(graph) {}

    [[nodiscard]] std::expected<ResolvedPortRealization, std::string> solve()
    {
        if (auto result = initialize(); !result) return std::unexpected(std::move(result.error()));
        if (auto result = apply_authored_constraints(); !result) return std::unexpected(std::move(result.error()));
        if (auto result = apply_bundle_constraints(); !result) return std::unexpected(std::move(result.error()));
        if (auto result = apply_connection_constraints(); !result) return std::unexpected(std::move(result.error()));
        auto execution_rates = solve_rates();
        if (!execution_rates) return std::unexpected(std::move(execution_rates.error()));

        std::vector<ResolvedPortRealization::Bundle> resolved(bundles_.size());
        for (NodeBundleHandle bundle = 0; bundle < bundles_.size(); ++bundle) {
            resolved[bundle].execution_rate = (*execution_rates)[bundle];
            for (std::size_t port = 0; port < bundles_[bundle].inputs.size(); ++port) {
                auto value = resolve_continuous(
                    bundle, bundles_[bundle].inputs[port],
                    PortConstraintDirection::input, port, (*execution_rates)[bundle]);
                if (!value) return std::unexpected(std::move(value.error()));
                resolved[bundle].inputs.push_back(*value);
            }
            for (std::size_t port = 0; port < bundles_[bundle].outputs.size(); ++port) {
                auto value = resolve_continuous(
                    bundle, bundles_[bundle].outputs[port],
                    PortConstraintDirection::output, port, (*execution_rates)[bundle]);
                if (!value) return std::unexpected(std::move(value.error()));
                resolved[bundle].outputs.push_back(*value);
            }
            for (auto const& variables : bundles_[bundle].event_inputs)
                resolved[bundle].event_inputs.push_back(resolve_event(variables));
            for (auto const& variables : bundles_[bundle].event_outputs)
                resolved[bundle].event_outputs.push_back(resolve_event(variables));
        }
        return ResolvedPortRealization(std::move(resolved));
    }
};

} // namespace detail

ResolvedContinuousPortRealization const& ResolvedPortRealization::input(
    NodeBundleHandle bundle, std::size_t port) const
{
    if (bundle >= bundles_.size() || port >= bundles_[bundle].inputs.size())
        throw std::out_of_range("resolved continuous input port is out of bounds");
    return bundles_[bundle].inputs[port];
}

ResolvedContinuousPortRealization const& ResolvedPortRealization::output(
    NodeBundleHandle bundle, std::size_t port) const
{
    if (bundle >= bundles_.size() || port >= bundles_[bundle].outputs.size())
        throw std::out_of_range("resolved continuous output port is out of bounds");
    return bundles_[bundle].outputs[port];
}

ResolvedEventPortRealization const& ResolvedPortRealization::event_input(
    NodeBundleHandle bundle, std::size_t port) const
{
    if (bundle >= bundles_.size() || port >= bundles_[bundle].event_inputs.size())
        throw std::out_of_range("resolved event input port is out of bounds");
    return bundles_[bundle].event_inputs[port];
}

ResolvedEventPortRealization const& ResolvedPortRealization::event_output(
    NodeBundleHandle bundle, std::size_t port) const
{
    if (bundle >= bundles_.size() || port >= bundles_[bundle].event_outputs.size())
        throw std::out_of_range("resolved event output port is out of bounds");
    return bundles_[bundle].event_outputs[port];
}

PortPace ResolvedPortRealization::execution_rate(NodeBundleHandle bundle) const
{
    if (bundle >= bundles_.size())
        throw std::out_of_range("resolved node execution rate is out of bounds");
    return bundles_[bundle].execution_rate;
}

std::expected<ResolvedPortRealization, std::string>
resolve_port_realization(ConfiguredGraph const& graph)
{
    return detail::PortRealizationSolver(graph).solve();
}

} // namespace iv::graph_jit
