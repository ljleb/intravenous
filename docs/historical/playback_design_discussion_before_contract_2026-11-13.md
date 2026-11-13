# Playback Discussion Before The Initial Contract

_Status: historical coordination text, not normative design._

Preserved on 2026-11-13 from the section "Ruler labels and playback-position
investigation" in `docs/vue_client_implementation_stages.md`. The owner agreed
to settle the playback contract and proposed grids from node outputs.
The replacement is [Project Playback And Presentation Grids](../project_playback_and_presentation_grids_direction.md).

## Preserved text

**Proposed next design discussion:** define the playback-position owner and
play/pause/seek behavior before exposing a moving playhead. Seconds and project
sample indices can display the same position using the project sample rate;
they do not require separate clocks. Beats remain undecided. An explicit node
that produces periodic beat events does not require a built-in tempo map.
No playback API, control gesture, or tempo policy is introduced by this pass.
After this bounded discussion, resume Stage 2's remaining scope/port and editing
work rather than treating ruler navigation as a replacement for those steps.

## Initial contract draft before correcting the clip example

The following draft passages came from
`docs/project_playback_and_presentation_grids_direction.md` on 2026-11-13.
The owner challenged the clip-reader example. It overlooked the existing
pause rules for background data in `graph_jit_direction.md` and
`coverage_and_background_evaluation.md`.
The owner also preferred a builtin callback structure over an explicit
playback-source node. These passages do not describe the accepted design.

> The distinction matters for a paused project with both a live synthesizer and
> an audio-clip reader. The synthesizer can continue processing input and
> producing sound. The clip reader needs playback state if it must stop reading
> the clip. Freezing a UI marker alone does not supply that behavior.

> The selected position and the current Tick callback's global interval are
> different facts while paused. The implementation must not treat the count
> of rendered device frames as the paused project position.
>
> The recommended public interface is a builtin node with position and playback
> state outputs. A clip reader receives those values through ordinary graph
> inputs. Nodes that do not depend on playback state need no new callback API.
> This makes playback dependencies visible to GraphJit. The alternative is
> implicit access through callback contexts.
>
> The owner is reviewing this choice. No node identifier, port schema, or
> callback-context member is fixed by this document. This question must be
> resolved before implementing pause behavior for timeline-dependent nodes.
