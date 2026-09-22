#include <intravenous/dsl.h>

#include <array>

struct ValidIndexedNode {
    static constexpr auto inputs()
    {
        return std::array {iv::indexed_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::indexed_sample_output("output")};
    }

    static constexpr std::size_t num_inputs() { return inputs().size(); }
    static constexpr std::size_t num_outputs() { return outputs().size(); }

    void tick_block(iv::TickBlockContext<ValidIndexedNode> const& ctx) const
    {
        (void)ctx.template input<"input">();
    }
    void tock_coverage(iv::TockCoverageContext<ValidIndexedNode>&) const {}
};

IV_NODE("iv.test.valid_indexed_node", ValidIndexedNode);

struct ValidIndexedEventNode {
    static constexpr auto inputs()
    {
        return std::array {
            iv::indexed_event_input("input", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::indexed_event_output("output", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<ValidIndexedEventNode> const&) const {}
    void tock_coverage(iv::TockCoverageContext<ValidIndexedEventNode>&) const {}
};

IV_NODE("iv.test.valid_indexed_event_node", ValidIndexedEventNode);

struct ValidIndexedInputOnlyNode {
    static constexpr auto inputs()
    {
        return std::array {
            iv::indexed_sample_input("samples"),
            iv::indexed_event_input("events", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<ValidIndexedInputOnlyNode> const&) const {}
};

IV_NODE("iv.test.valid_indexed_input_only_node", ValidIndexedInputOnlyNode);


struct ValidRecorderNode {
    struct IndexedState {
        int write_count = 0;
    };

    static constexpr auto inputs()
    {
        return std::array {iv::realtime_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::indexed_sample_output("recording")};
    }

    void tick_block(iv::TickBlockContext<ValidRecorderNode> const& ctx) const
    {
        (void)ctx.template input<"input">();
        ++ctx.indexed_state().write_count;
    }

    void tock_coverage(iv::TockCoverageContext<ValidRecorderNode>& ctx) const
    {
        ++ctx.indexed_state().write_count;
        (void)ctx.template output<"recording">();
    }
};

IV_NODE("iv.test.valid_recorder_node", ValidRecorderNode);
