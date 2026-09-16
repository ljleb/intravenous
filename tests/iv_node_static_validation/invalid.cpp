#include <intravenous/dsl.h>

#include <array>

struct DynamicSamplePortNode {
    std::array<iv::InputConfig, 1> inputs() const { return {}; }
    void tick_block(iv::TickBlockContext<DynamicSamplePortNode> const&) const {}
};

struct DynamicEventPortNode {
    std::array<iv::InputConfig, 1> inputs() const
    {
        return {iv::realtime_event_input("trigger", iv::EventTypeId::trigger)};
    }
    void tick_block(iv::TickBlockContext<DynamicEventPortNode> const&) const {}
};

struct NonConstexprPortNode {
    static auto outputs()
    {
        return std::array {iv::realtime_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<NonConstexprPortNode> const&) const {}
};

struct DynamicPortCountNode {
    static constexpr auto inputs()
    {
        return std::array {iv::realtime_sample_input("input")};
    }

    std::size_t num_inputs() const { return 1; }
    void tick_block(iv::TickBlockContext<DynamicPortCountNode> const&) const {}
};

struct MissingCompiledAccessNode {
    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<MissingCompiledAccessNode> const&) const {}
};

struct ConflictingCompiledAccessNode {
    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<ConflictingCompiledAccessNode> const&) const {}
    void access_block(iv::AccessBlockContext<ConflictingCompiledAccessNode>&) const {}
    void access_block_batch(
        iv::AccessBlockBatchContext<ConflictingCompiledAccessNode>&) const {}
};

IV_NODE("iv.test.dynamic_sample_port_node", DynamicSamplePortNode);
IV_NODE("iv.test.dynamic_event_port_node", DynamicEventPortNode);
IV_NODE("iv.test.non_constexpr_port_node", NonConstexprPortNode);
IV_NODE("iv.test.dynamic_port_count_node", DynamicPortCountNode);
IV_NODE("iv.test.missing_compiled_access", MissingCompiledAccessNode);
IV_NODE("iv.test.conflicting_compiled_access", ConflictingCompiledAccessNode);
