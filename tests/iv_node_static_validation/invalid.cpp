#include <intravenous/dsl.h>

#include <array>

struct DynamicSamplePortNode {
    std::array<iv::InputConfig, 1> inputs() const { return {}; }
    void tick_block(iv::TickBlockContext<DynamicSamplePortNode> const&) const {}
};

struct DynamicEventPortNode {
    std::array<iv::InputConfig, 1> inputs() const
    {
        return {iv::event_input("trigger", iv::EventTypeId::trigger)};
    }
    void tick_block(iv::TickBlockContext<DynamicEventPortNode> const&) const {}
};

struct NonConstexprPortNode {
    static auto outputs()
    {
        return std::array {iv::sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<NonConstexprPortNode> const&) const {}
};

struct DynamicPortCountNode {
    static constexpr auto inputs()
    {
        return std::array {iv::sample_input("input")};
    }

    std::size_t num_inputs() const { return 1; }
    void tick_block(iv::TickBlockContext<DynamicPortCountNode> const&) const {}
};

IV_NODE("iv.test.dynamic_sample_port_node", DynamicSamplePortNode);
IV_NODE("iv.test.dynamic_event_port_node", DynamicEventPortNode);
IV_NODE("iv.test.non_constexpr_port_node", NonConstexprPortNode);
IV_NODE("iv.test.dynamic_port_count_node", DynamicPortCountNode);
