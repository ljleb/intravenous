#include <intravenous/dsl.h>

#include <array>

struct InvalidTickWindowedInput {
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::coverage)};
    }
    void tick_block(iv::TickBlockContext<InvalidTickWindowedInput> const&) const {}
};

static_assert(iv::details::has_tick_family_v<InvalidTickWindowedInput>);
IV_NODE("iv.test.invalid_tick_windowed_input", InvalidTickWindowedInput, "Invalid tick windowed input");
