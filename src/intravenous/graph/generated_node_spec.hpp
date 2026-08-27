#pragma once

#include <intravenous/graph/connection_node.hpp>
#include <intravenous/graph/reflected_node.hpp>
#include <intravenous/graph/runtime_binding_nodes.hpp>

#include <stdexcept>
#include <type_traits>
#include <variant>

namespace iv {
using GeneratedNodeSpec = std::variant<
    std::monostate,
    ConnectionNodeSpec,
    RuntimeSampleInputNodeSpec,
    RuntimeEventInputNodeSpec,
    RuntimeSampleOutputNodeSpec,
    RuntimeEventOutputNodeSpec,
    RuntimeSampleOutputFamilyNodeSpec,
    RuntimeEventOutputFamilyNodeSpec>;

namespace details {
consteval ReflectedNodeDescription reflect_generated_node(
    GeneratedNodeSpec const& spec)
{
    return std::visit(
        []<class Spec>(Spec const& concrete) -> ReflectedNodeDescription {
            if constexpr (std::same_as<Spec, std::monostate>) {
                throw std::logic_error("concrete node has no implementation");
            } else {
                return reflect_node(freeze_generated_node(concrete));
            }
        },
        spec);
}
} // namespace details
} // namespace iv
