# Optional port display names

These excerpts preserve the 2026-11-25 design before requiring names on
authored ports and adding short names.

From [LLVM nodes](../api/reference/llvm_nodes.md):

> Use `iv::with_display_name(config, "Dry signal")` around an input or output
> declaration to give it an editor label. Callbacks and connections use the
> port identifier supplied to the original declaration.
>
> For a module boundary, use `g.input(iv::PortDisplayName{"Dry signal"})` or
> `g.outputs("wet"_P.with_display_name("Wet signal") = wet_signal)`.

From [Node presentation and manual controls](../node_presentation_and_manual_controls_direction.md):

> The editor shows the port's display name when one is provided and falls back
> to the identifier or the port number.
