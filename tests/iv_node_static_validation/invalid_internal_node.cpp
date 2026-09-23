#include <intravenous/graph/reflected_node_description.h>

#include <array>

struct DynamicInternalNode {
    std::array<iv::InputConfig, 1> inputs() const
    {
        return {iv::realtime_sample_input("input")};
    }

    void tick_block(iv::TickBlockContext<DynamicInternalNode> const&) const {}
};

int main()
{
    DynamicInternalNode node;
    (void)iv::details::make_node_build_request(node);
}
