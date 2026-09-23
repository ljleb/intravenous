#include <intravenous/dsl.h>

#include <array>

struct ValidIndexedNode {
    static constexpr auto inputs()
    {
        return std::array {iv::random_access_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::tock_sample_output("output")};
    }

    static constexpr std::size_t num_inputs() { return inputs().size(); }
    static constexpr std::size_t num_outputs() { return outputs().size(); }

    void tick_block(iv::TickBlockContext<ValidIndexedNode> const& ctx) const
    {
        (void)ctx.template input<"input">();
    }
    void tock_coverage(iv::TockCoverageContext<ValidIndexedNode>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<ValidIndexedNode>& context) const
    {
        context.template output<"output">().publish_coverage(
            context.template input<"input">().coverage());
    }
};

IV_NODE("iv.test.valid_indexed_node", ValidIndexedNode);

struct ValidIndexedEventNode {
    static constexpr auto inputs()
    {
        return std::array {
            iv::random_access_event_input("input", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::tock_event_output("output", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<ValidIndexedEventNode> const&) const {}
    void tock_coverage(iv::TockCoverageContext<ValidIndexedEventNode>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<ValidIndexedEventNode>& context) const
    {
        context.template output<"output">().publish_coverage(
            context.template input<"input">().coverage());
    }
};

IV_NODE("iv.test.valid_indexed_event_node", ValidIndexedEventNode);

struct ValidIndexedInputOnlyNode {
    static constexpr auto inputs()
    {
        return std::array {
            iv::random_access_sample_input("samples"),
            iv::random_access_event_input("events", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<ValidIndexedInputOnlyNode> const&) const {}
};

IV_NODE("iv.test.valid_indexed_input_only_node", ValidIndexedInputOnlyNode);


struct ValidPersistedRealtimeNode {
    static constexpr auto inputs()
    {
        return std::array {iv::sequential_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::tick_sample_output(
            "recording", {}, {}, iv::OutputRetention::persisted)};
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
        return std::array {iv::sequential_sample_input("input")};
    }
    static constexpr auto outputs()
    {
        return std::array {iv::tick_sample_output("output")};
    }
    void tick(iv::TickSampleContext<ValidReplayableNode> const&) const {}
};

static_assert(iv::details::replay_declaration_is_valid_v<ValidReplayableNode>);
static_assert(iv::details::intrinsically_replayable_v<ValidReplayableNode>);
IV_NODE("iv.test.valid_replayable_node", ValidReplayableNode);
