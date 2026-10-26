#include <intravenous/dsl.h>

#include <array>
#include <span>

struct ValidBackgroundNode {
    static constexpr auto inputs()
    {
        return std::array {iv::input<iv::Sample>("input", iv::InputAccess::coverage)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::coverage)};
    }

    static constexpr std::size_t num_inputs()
    {
        return iv::port_config_count_v<decltype(inputs())>;
    }
    static constexpr std::size_t num_outputs()
    {
        return iv::port_config_count_v<decltype(outputs())>;
    }

    void tock_coverage(iv::TockCoverageContext<ValidBackgroundNode>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<ValidBackgroundNode>& context) const
    {
        context.template output<"output">().publish_regions(
            context.template input<"input">().regions());
    }
};

IV_NODE("iv.test.valid_background_node", ValidBackgroundNode);

struct ValidBackgroundEventNode {
    static constexpr auto inputs()
    {
        return std::array {
            iv::event_input("input", iv::EventTypeId::trigger, iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::event_output("output", iv::EventTypeId::trigger, iv::OutputProduction::coverage),
        };
    }

    void tock_coverage(iv::TockCoverageContext<ValidBackgroundEventNode>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<ValidBackgroundEventNode>& context) const
    {
        context.template output<"output">().publish_coverage(
            context.template input<"input">().coverage());
    }
};

IV_NODE("iv.test.valid_background_event_node", ValidBackgroundEventNode);

struct ValidBackgroundInputOnlyNode {
    static constexpr auto inputs()
    {
        return std::tuple {
            iv::input<iv::Sample>("samples", iv::InputAccess::coverage),
            iv::event_input("events", iv::EventTypeId::trigger, iv::InputAccess::coverage),
        };
    }

    void tick_block(iv::TickBlockContext<ValidBackgroundInputOnlyNode> const&) const {}
};

IV_NODE("iv.test.valid_background_input_only_node", ValidBackgroundInputOnlyNode);


struct ValidPersistedRealtimeNode {
    static constexpr auto inputs()
    {
        return std::array {iv::input<iv::Sample>("input", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("recording", iv::OutputProduction::tick, iv::OutputRetention::persisted)};
    }

    void tick_block(
        iv::TickBlockContext<ValidPersistedRealtimeNode> const& ctx) const
    {
        (void)ctx.template input<"input">();
        auto output = ctx.template output<"recording">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output[i] = 0.0f;
        }
    }
};

IV_NODE("iv.test.valid_persisted_realtime_node", ValidPersistedRealtimeNode);

// Intrinsic replayability is an opt-in Tick trait, not a Tock output mode.
struct ValidReplayableNode {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto inputs()
    {
        return std::array {iv::input<iv::Sample>("input", iv::InputAccess::sequential)};
    }
    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::tick)};
    }
    void tick(iv::TickSampleContext<ValidReplayableNode> const&) const {}
};

static_assert(iv::details::replay_declaration_is_valid_v<ValidReplayableNode>);
static_assert(iv::details::intrinsically_replayable_v<ValidReplayableNode>);
IV_NODE("iv.test.valid_replayable_node", ValidReplayableNode);

struct ValidBatchTickNode {
    static constexpr auto outputs()
    {
        return std::array {
            iv::output<iv::Sample>("output", iv::OutputProduction::tick),
        };
    }

    static void tick_block_batch(
        iv::TickBlockBatchContext<ValidBatchTickNode> const& context)
    {
        for (auto lane : context) (void)lane.template output<"output">();
    }

    static void skip_block_batch(
        iv::SkipBlockBatchContext<ValidBatchTickNode> const& context)
    {
        for (auto lane : context) (void)lane.context();
    }
};

IV_NODE("iv.test.valid_batch_tick_node", ValidBatchTickNode);

struct ValidBatchBackgroundNode {
    static constexpr auto inputs()
    {
        return std::array {
            iv::input<iv::Sample>("input", iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::output<iv::Sample>("output", iv::OutputProduction::coverage),
        };
    }

    static void tock_coverage_batch(
        iv::TockCoverageBatchContext<ValidBatchBackgroundNode>& context)
    {
        for (auto lane : context) (void)lane.template output<"output">();
    }

    static void propagate_forward_coverage_batch(
        iv::PropagateForwardCoverageBatchContext<
            ValidBatchBackgroundNode>& context)
    {
        for (auto lane : context) {
            lane.template output<"output">().publish_regions(
                std::span<iv::GlobalIndexRange const>{});
        }
    }

    static void propagate_reverse_coverage_batch(
        iv::PropagateReverseCoverageBatchContext<
            ValidBatchBackgroundNode>& context)
    {
        for (auto lane : context) {
            lane.template input<"input">().require(
                lane.template output<"output">().required());
        }
    }
};

IV_NODE("iv.test.valid_batch_background_node", ValidBatchBackgroundNode);

struct ValidReplayableBlockNode {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto outputs()
    {
        return std::array {
            iv::output<iv::Sample>("output", iv::OutputProduction::tick),
        };
    }
    void tick_block(
        iv::TickBlockContext<ValidReplayableBlockNode> const&) const {}
};

static_assert(iv::details::replay_declaration_is_valid_v<
    ValidReplayableBlockNode>);
IV_NODE("iv.test.valid_replayable_block_node", ValidReplayableBlockNode);
