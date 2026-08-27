#include <intravenous/node/layout.h>

#include <cstddef>
#include <cstdint>
#include <meta>
#include <string_view>
#include <type_traits>

namespace sm = std::meta;

struct ReflectedState {
    std::uint32_t cursor = 0;
    float phase = 0.0f;
    unsigned active : 3 = 0;
};

struct StateBase {};

struct DerivedState : StateBase {
    int value = 0;
};

struct ReflectedNode {
    using State = ReflectedState;
};

template<class State>
consteval bool valid_node_state_structure()
{
    return std::is_default_constructible_v<State>
        && sm::bases_of(^^State, sm::access_context::unchecked()).empty();
}

consteval bool reflects_node_state_structure()
{
    auto const members = sm::nonstatic_data_members_of(
        ^^ReflectedState,
        sm::access_context::unchecked());
    return members.size() == 3
        && sm::identifier_of(members[0]) == "cursor"
        && sm::type_of(members[0]) == (^^unsigned int)
        && sm::offset_of(members[0]).total_bits()
            == offsetof(ReflectedState, cursor) * 8
        && sm::size_of(sm::type_of(members[0])) == sizeof(std::uint32_t)
        && sm::alignment_of(sm::type_of(members[0]))
            == alignof(std::uint32_t)
        && !sm::display_string_of(sm::type_of(members[0])).empty()
        && sm::identifier_of(members[1]) == "phase"
        && sm::type_of(members[1]) == (^^float)
        && sm::offset_of(members[1]).total_bits()
            == offsetof(ReflectedState, phase) * 8
        && sm::identifier_of(members[2]) == "active"
        && sm::type_of(members[2]) == (^^unsigned int)
        && sm::bit_size_of(members[2]) == 3;
}

static_assert(valid_node_state_structure<ReflectedState>());
static_assert(!valid_node_state_structure<DerivedState>());
static_assert(reflects_node_state_structure());

iv::NodeStateStructure production_node_state_structure()
{
    return iv::details::reflect_node_state_structure<ReflectedNode>();
}
