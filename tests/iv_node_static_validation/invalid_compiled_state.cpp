#include <intravenous/dsl.h>

#include <array>

struct InvalidConstCompiledState {
    using CompiledState = int const;

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void access_block(iv::AccessBlockContext<InvalidConstCompiledState>&) const {}
};

IV_NODE("iv.test.invalid_const_compiled_state", InvalidConstCompiledState);
