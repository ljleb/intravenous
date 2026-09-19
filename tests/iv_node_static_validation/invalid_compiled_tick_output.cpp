#include <intravenous/dsl.h>

#include <array>

struct InvalidCompiledSampleTickOutput {
    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<InvalidCompiledSampleTickOutput> const& ctx) const
    {
        (void)ctx.template output<"output">();
    }

    void access_block(iv::AccessBlockContext<InvalidCompiledSampleTickOutput>&) const {}
};

IV_NODE("iv.test.invalid_compiled_sample_tick_output", InvalidCompiledSampleTickOutput);

struct InvalidCompiledEventTickOutput {
    static constexpr auto outputs()
    {
        return std::array {
            iv::compiled_event_output("events", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<InvalidCompiledEventTickOutput> const& ctx) const
    {
        (void)ctx.template output<"events">();
    }

    void access_block(iv::AccessBlockContext<InvalidCompiledEventTickOutput>&) const {}
};

IV_NODE("iv.test.invalid_compiled_event_tick_output", InvalidCompiledEventTickOutput);
