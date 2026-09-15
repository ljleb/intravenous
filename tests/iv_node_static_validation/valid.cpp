#include <intravenous/dsl.h>

#include <array>

struct ValidNode {
    static constexpr auto inputs()
    {
        return std::array {iv::sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<ValidNode> const& ctx) const
    {
        (void)ctx.template output<"output">();
    }
};

IV_NODE("iv.test.valid_node", ValidNode);
