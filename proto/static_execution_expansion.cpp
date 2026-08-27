#include <array>
#include <cstddef>
#include <utility>

namespace {
template<class T>
struct StaticSpan {
    T const* data = nullptr;
    std::size_t size = 0;

    constexpr T const& operator[](std::size_t index) const
    {
        return data[index];
    }
};

using Operation = void (*)(int&);

constexpr void add_one(int& value)
{
    ++value;
}

constexpr void double_value(int& value)
{
    value *= 2;
}

struct Node {
    Operation operation = nullptr;
};

struct Scc {
    StaticSpan<Node> nodes {};
};

struct Graph {
    StaticSpan<Scc> sccs {};
};

constexpr std::array scc0_nodes {
    Node { .operation = &add_one },
    Node { .operation = &double_value },
};
constexpr std::array scc1_nodes {
    Node { .operation = &add_one },
};
constexpr std::array sccs {
    Scc { .nodes = { scc0_nodes.data(), scc0_nodes.size() } },
    Scc { .nodes = { scc1_nodes.data(), scc1_nodes.size() } },
};
constexpr Graph graph {
    .sccs = { sccs.data(), sccs.size() },
};

template<auto GraphValue, std::size_t SccIndex, std::size_t NodeIndex>
constexpr void tick_node(int& value)
{
    GraphValue.sccs[SccIndex].nodes[NodeIndex].operation(value);
}

template<auto GraphValue, std::size_t SccIndex, std::size_t... NodeIndices>
constexpr void tick_scc(int& value, std::index_sequence<NodeIndices...>)
{
    (tick_node<GraphValue, SccIndex, NodeIndices>(value), ...);
}

template<auto GraphValue, std::size_t... SccIndices>
constexpr void tick_graph(int& value, std::index_sequence<SccIndices...>)
{
    (tick_scc<GraphValue, SccIndices>(
         value,
         std::make_index_sequence<GraphValue.sccs[SccIndices].nodes.size> {}),
     ...);
}

template<auto GraphValue>
constexpr int run_graph()
{
    int value = 1;
    tick_graph<GraphValue>(
        value,
        std::make_index_sequence<GraphValue.sccs.size> {});
    return value;
}

static_assert(run_graph<graph>() == 5);
}

int static_execution_expansion_prototype()
{
    return run_graph<graph>();
}
