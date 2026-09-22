#include <intravenous/dsl.h>

#include <array>

struct InvalidIndexedSampleTickOutput {
    static constexpr auto outputs()
    {
        return std::array {iv::indexed_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<InvalidIndexedSampleTickOutput> const& ctx) const
    {
        (void)ctx.template output<"output">();
    }

    void tock_coverage(iv::TockCoverageContext<InvalidIndexedSampleTickOutput>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<InvalidIndexedSampleTickOutput>&) const {}
};

IV_NODE("iv.test.invalid_indexed_sample_tick_output", InvalidIndexedSampleTickOutput);

struct InvalidIndexedEventTickOutput {
    static constexpr auto outputs()
    {
        return std::array {
            iv::indexed_event_output("events", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<InvalidIndexedEventTickOutput> const& ctx) const
    {
        (void)ctx.template output<"events">();
    }

    void tock_coverage(iv::TockCoverageContext<InvalidIndexedEventTickOutput>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<InvalidIndexedEventTickOutput>&) const {}
};

IV_NODE("iv.test.invalid_indexed_event_tick_output", InvalidIndexedEventTickOutput);

struct InvalidTickRecordTockOutput {
    static constexpr auto outputs()
    {
        return std::array {iv::indexed_sample_output(
            "recording", {},
            {.producer = iv::IndexedProducer::tick_record})};
    }

    void tick_block(
        iv::TickBlockContext<InvalidTickRecordTockOutput> const&) const {}

    void tock_coverage(
        iv::TockCoverageContext<InvalidTickRecordTockOutput>& context) const
    {
        (void)context.template output<"recording">();
    }
};

IV_NODE("iv.test.invalid_tick_record_tock_output", InvalidTickRecordTockOutput);
