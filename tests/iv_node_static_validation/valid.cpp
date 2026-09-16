#include <intravenous/dsl.h>

#include <array>

struct ValidCompiledNode {
    static constexpr auto inputs()
    {
        return std::array {iv::compiled_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
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
            iv::compiled_event_input("input", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::compiled_event_output("output", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<ValidCompiledEventNode> const&) const {}
    void access_block_batch(
        iv::AccessBlockBatchContext<ValidCompiledEventNode>&) const {}
};

IV_NODE("iv.test.valid_compiled_event_node", ValidCompiledEventNode);

struct ValidCompiledInputOnlyNode {
    static constexpr auto inputs()
    {
        return std::array {
            iv::compiled_sample_input("samples"),
            iv::compiled_event_input("events", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<ValidCompiledInputOnlyNode> const&) const {}
};

IV_NODE("iv.test.valid_compiled_input_only_node", ValidCompiledInputOnlyNode);
