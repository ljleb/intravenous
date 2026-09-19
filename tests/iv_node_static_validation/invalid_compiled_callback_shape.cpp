#include <intravenous/dsl.h>

#include <array>

struct InvalidInputOnlyAccessCallback {
    static constexpr auto inputs()
    {
        return std::array {iv::compiled_sample_input("input")};
    }

    void tick_block(iv::TickBlockContext<InvalidInputOnlyAccessCallback> const&) const {}
    void access_block(iv::AccessBlockContext<InvalidInputOnlyAccessCallback>&) const {}
};

IV_NODE("iv.test.invalid_input_only_access_callback", InvalidInputOnlyAccessCallback);

struct InvalidOutputOnlyPropagationCallback {
    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<InvalidOutputOnlyPropagationCallback> const&) const {}
    void access_block(iv::AccessBlockContext<InvalidOutputOnlyPropagationCallback>&) const {}
    void propagate_block_access(iv::PropagateBlockAccessContext<InvalidOutputOnlyPropagationCallback>&) const {}
};

IV_NODE("iv.test.invalid_output_only_propagation_callback", InvalidOutputOnlyPropagationCallback);
