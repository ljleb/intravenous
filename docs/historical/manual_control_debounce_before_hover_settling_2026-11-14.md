# Manual-control settling before the hover-leave decision

Historical context, preserved 2026-11-14 from the working-tree versions of
`docs/node_presentation_and_manual_controls_direction.md` and
`docs/graph_jit_direction.md`, before implementing the owner’s hover-leave request.

The owner replaced idle/debounce settling with explicit hover lifetime: mouse
release closes the undo gesture, and leaving the knob requests settled code.
The same request adds discrete power-of-two sensitivity, pointer lock, and
logical/normalized readouts. Current replacements are in
[Node Presentation and Manual Controls](../node_presentation_and_manual_controls_direction.md)
and [GraphJit Direction](../graph_jit_direction.md). The following excerpts retain
the superseded wording, including its earlier rationale and scheduling rules.

## node_presentation_and_manual_controls_direction.md — temporary override

### Temporary connected-input override

_Settled by the repository owner on 2026-11-14._

Only Sequential inputs receive tweakable manual controls. During a live
interaction, the manual value replaces the selected input's incoming signal,
including when its settled participation policy is disconnected-only. The knob
retains its independent manual value; it does not have to follow the connected
signal. Ending the interaction restores the authored connection when the
replacement activates. Neither the authored wiring nor the durable participation
policy changes. Hover prepares code and never activates the override.

## node_presentation_and_manual_controls_direction.md — knob gestures

Drag up to increase the value and down to decrease it. During the drag, scroll
down for finer adjustment or up for faster adjustment. Changing sensitivity
must not change the current value or reset the pointer origin. Wheel events
used by a knob must not scroll or zoom the timeline.

The exact authored default receives additional mouse travel. Preserve every
nearby numeric value; do not clamp a numeric interval to the default. The
extra travel has a fixed width in pointer pixels, independent of sensitivity.
Changing sensitivity inside that region preserves progress through it.

Right-click opens a numeric-value popover. Typing remains local until Enter
accepts one exact-value gesture. Escape, clicking outside, or leaving the
editor cancels the draft. Numeric entry bypasses drag snapping. Hovering a knob
and pressing R resets it to the authored default, provided the graph has keyboard
focus and no text field is being edited. Arrow keys adjust a focused knob.

A compact VS Code status item reports the hovered knob's shortcuts and the
active drag's value and sensitivity; its tooltip holds fuller help. Clear it
when the relevant context or focus is lost. Hover never steals keyboard focus
from another editor. Gesture and numeric-entry ownership belongs to the webview
root, so remounting a port widget does not silently end its interaction.

## node_presentation_and_manual_controls_direction.md — interpolation and scheduling

## Interpolation exists only during active scrubbing

A settled manual value is eligible for exact constant specialization. The stored
manual value is the latest value retained by the project and UI. The effective manual
value is the value used by the active executable graph. These values can differ while
a replacement graph compiles.

Hover requests compilation of dynamic-input code. Hover does not activate that code.
If scrubbing starts first, the UI updates the stored value while the active graph keeps
using its existing constant. No generic execution path applies the new value to code
that does not support a dynamic input.

The intended lifecycle is:

```text
settled:
    active graph uses the exact constant value

hover:
    request dynamic-input compilation
    keep the active graph unchanged

scrub begins before dynamic code is active:
    retain each latest desired value
    keep the active graph's constant effective value

dynamic graph becomes active during the scrub:
    make the latest desired value effective
    begin audio-thread interpolation from the prior effective value

for each audio callback while dynamic scrubbing is active:
    latch the newest UI target
    interpolate sample-by-sample from the previous effective value
    to the latched target over the callback

input updates become idle:
    stop interpolation
    keep the dynamic graph active during the debounce interval

debounce interval expires:
    capture the latest stored value
    request a constant-specialized replacement

constant-specialized graph becomes active:
    use the settled value as an exact constant
```

Pointer release is not the only idle trigger. Front 14 uses one explicit configurable
or fixed debounce policy instead of compiling after every input update.

The UI-to-audio target scalar requires defined C++ concurrency. The audio thread can
observe the latest coherent target that is available. A relaxed atomic is sufficient
for that scalar. The audio thread owns the prior value, effective value, and
interpolation state. A target update does not require an RPC or queue handshake.

## Predictive compilation and demand-driven activation

Settled controls are candidates for constant specialization under GraphJit. Hovering
over a participating control requests code whose `dynamic_inputs` set contains that
input. Hover alone does not activate the compiled code.

Front 14 initially keeps one active executable graph and one logically current
replacement request. Compiled code can overlap temporarily during safe activation and
reclamation. Front 14 does not keep a cache of old control specializations.

The required `dynamic_inputs` set contains every control with an ongoing interaction.
It can also contain the currently relevant hovered control for predictive compilation.
A later hover can replace the predictive part of the set. It must not remove a control
that an ongoing interaction still requires.

The initial scheduling policy is:

1. A settled control can be an exact constant in the active graph.
2. Hover requests a dynamic replacement but keeps the active graph unchanged.
3. An interaction makes its control a required member of `dynamic_inputs`.
4. If matching dynamic code is ready, `GraphExecutor` activates it safely.
5. If matching code is not ready, the UI retains the latest desired value.
6. The active graph continues to use the value representation that it supports.
7. If the interaction ends first, the scheduler can skip the obsolete dynamic result
   and request the settled constant replacement directly.
8. After the idle interval, the scheduler requests code with the latest settled
   constant values.

## node_presentation_and_manual_controls_direction.md — implementation order

7. Add predictive hover compilation, demand-driven activation, stale-result rejection,
   and the idle return to constant specialization.

## graph_jit_direction.md — dynamic inputs and settling

The `dynamic_inputs` set contains every control with an ongoing interaction. A later
hover can supersede predictive work, but it cannot remove an input that an ongoing
interaction requires. Each compilation carries exact request and project-revision
identity. The worker cancels obsolete work when possible and otherwise discards its
result.

After manual updates remain idle for the defined debounce interval, GraphJit captures
the latest settled values and compiles a constant-specialized replacement. The active
dynamic code continues during compilation. Pointer release is not the only idle
trigger.
