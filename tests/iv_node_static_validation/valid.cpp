#include <intravenous/dsl.h>

#include <array>

struct ValidCompiledNode {
    static constexpr auto inputs()
    {
        return std::array {iv::sample_input("input", {}, true)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output", {}, true)};
    }

    static constexpr std::size_t num_inputs() { return inputs().size(); }
    static constexpr std::size_t num_outputs() { return outputs().size(); }

    void tick_block(iv::TickBlockContext<ValidCompiledNode> const& ctx) const
    {
        (void)ctx.template output<"output">();
    }
    void access_block_batch(iv::AccessBlockBatchContext<ValidCompiledNode>&) const {}
};

IV_NODE("iv.test.valid_compiled_node", ValidCompiledNode);

struct ValidCompiledEventNode {
    static constexpr auto inputs()
    {
        return std::array {
            iv::event_input("input", iv::EventTypeId::trigger, true),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::event_output("output", iv::EventTypeId::trigger, true),
        };
    }

    void tick_block(iv::TickBlockContext<ValidCompiledEventNode> const&) const {}
};

IV_NODE("iv.test.valid_compiled_event_node", ValidCompiledEventNode);
