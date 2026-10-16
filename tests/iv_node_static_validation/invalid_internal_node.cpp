#include <intravenous/graph/reflected_node_description.h>

#include <array>

struct DynamicInternalNode {
    std::array<iv::SequentialInputConfig<iv::Sample>, 1> inputs() const
    {
        return {iv::input<iv::Sample>("input", iv::InputAccess::sequential)};
    }

    void tick_block(iv::TickBlockContext<DynamicInternalNode> const&) const {}
};

int main()
{
    DynamicInternalNode node;
    (void)iv::details::make_node_build_request(node);
}
