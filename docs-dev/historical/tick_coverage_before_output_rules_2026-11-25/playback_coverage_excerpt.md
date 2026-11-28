> Historical excerpt from [Background selection, coverage and pause](../../project_playback_and_presentation_grids_direction.md#background-selection-coverage-and-pause) before the 2026-11-25 Tick coverage change. Follow that link for the replacement.

Coverage derived by `TickReplayMode::input_union` uses the actual coverages of
the values selected for that evaluation. A paused live invocation can select
background data at playhead position `P` while a pure pointwise Tick replay
requested at global position `I` selects data at `I`. Their output values and
coverage may differ even when the same node implementation is intrinsically
replayable. This is not permission to weaken pointwise replay or change its
existing purity/state constraints. Do not mix live transport-selected
coverage with background position-addressed replay coverage, or silently
turn held covered inputs into uncovered inputs. [Tick Input-Union Coverage](./tick_input_union_and_feedback_direction.md)
remains authoritative about fallback, `input_union`, `full`, and feedback SCCs.
