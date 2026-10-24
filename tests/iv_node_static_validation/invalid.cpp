#include <intravenous/dsl.h>

#include <array>

struct DynamicSamplePortNode {
    std::array<iv::SequentialInputConfig<iv::Sample>, 1> inputs() const
    {
        return {
            iv::input<iv::Sample>("input", iv::InputAccess::sequential),
        };
    }
    void tick_block(iv::TickBlockContext<DynamicSamplePortNode> const&) const {}
};

struct DynamicEventPortNode {
    std::array<iv::SequentialEventInputConfig, 1> inputs() const
    {
        return {
            iv::event_input(
                "trigger",
                iv::EventTypeId::trigger,
                iv::InputAccess::sequential),
        };
    }
    void tick_block(iv::TickBlockContext<DynamicEventPortNode> const&) const {}
};

struct NonConstexprPortNode {
    static auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::tick)};
    }

    void tick_block(iv::TickBlockContext<NonConstexprPortNode> const&) const {}
};

struct DynamicPortCountNode {
    static constexpr auto inputs()
    {
        return std::array {iv::input<iv::Sample>("input", iv::InputAccess::sequential)};
    }

    std::size_t num_inputs() const { return 1; }
    void tick_block(iv::TickBlockContext<DynamicPortCountNode> const&) const {}
};

struct MissingBackgroundTockNode {
    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::coverage)};
    }

    void tick_block(iv::TickBlockContext<MissingBackgroundTockNode> const&) const {}
};

struct MissingBackgroundEventTockNode {
    static constexpr auto outputs()
    {
        return std::array {
            iv::event_output("events", iv::EventTypeId::trigger, iv::OutputProduction::coverage),
        };
    }

    void tick_block(iv::TickBlockContext<MissingBackgroundEventTockNode> const&) const {}
};

struct InvalidBackgroundTockSignatureNode {
    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::coverage)};
    }

    void tick_block(iv::TickBlockContext<InvalidBackgroundTockSignatureNode> const&) const {}
    int tock_coverage(iv::TockCoverageContext<InvalidBackgroundTockSignatureNode>&) const
    {
        return 0;
    }
};

IV_NODE("iv.test.dynamic_sample_port_node", DynamicSamplePortNode);
IV_NODE("iv.test.dynamic_event_port_node", DynamicEventPortNode);
IV_NODE("iv.test.non_constexpr_port_node", NonConstexprPortNode);
IV_NODE("iv.test.dynamic_port_count_node", DynamicPortCountNode);
IV_NODE("iv.test.missing_background_tock", MissingBackgroundTockNode);
IV_NODE("iv.test.missing_background_event_tock", MissingBackgroundEventTockNode);
IV_NODE("iv.test.invalid_background_tock_signature", InvalidBackgroundTockSignatureNode);
