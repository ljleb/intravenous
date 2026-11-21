struct ForwardFinalSample {
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>("in")}; }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>("out")}; }

    void tick(iv::TickSampleContext<ForwardFinalSample> const& ctx) const
    {
        ctx.output<"out">().push(ctx.input<"in">().get());
    }
};

void fan_in_with_undelayed_operand(iv::GraphBuilder& graph)
{
    auto fast = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto slow = graph.node<"iv.test.graph_jit.state_context.revising_sample_source">();
    auto forwarded = graph.node<ForwardFinalSample>();
    auto probe = graph.node<"iv.test.graph_jit.state_context.sample_sequence_probe">();
    forwarded(slow);
    probe(fast);
    probe(forwarded);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.fan_in_with_undelayed_operand",
    fan_in_with_undelayed_operand, "Fan-in with undelayed operand");
