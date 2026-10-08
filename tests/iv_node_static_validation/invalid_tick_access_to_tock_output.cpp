#include <intravenous/dsl.h>

#include <array>

struct InvalidBackgroundSampleTickOutput {
    static constexpr auto outputs()
    {
        return std::array {iv::tock_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<InvalidBackgroundSampleTickOutput> const& ctx) const
    {
        (void)ctx.template output<"output">();
    }

    void tock_coverage(iv::TockCoverageContext<InvalidBackgroundSampleTickOutput>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<InvalidBackgroundSampleTickOutput>&) const {}
};

IV_NODE("iv.test.invalid_background_sample_tick_output", InvalidBackgroundSampleTickOutput);

struct InvalidBackgroundEventTickOutput {
    static constexpr auto outputs()
    {
        return std::array {
            iv::tock_event_output("events", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<InvalidBackgroundEventTickOutput> const& ctx) const
    {
        (void)ctx.template output<"events">();
    }

    void tock_coverage(iv::TockCoverageContext<InvalidBackgroundEventTickOutput>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<InvalidBackgroundEventTickOutput>&) const {}
};

IV_NODE("iv.test.invalid_background_event_tick_output", InvalidBackgroundEventTickOutput);
