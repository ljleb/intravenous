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
    void propagate_block_access_batch(
        iv::PropagateBlockAccessBatchContext<ValidCompiledNode>&) const {}
};

IV_NODE("iv.test.valid_compiled_node", ValidCompiledNode);
