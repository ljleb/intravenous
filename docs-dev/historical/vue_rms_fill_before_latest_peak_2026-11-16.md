# RMS fill before latest-peak display

Non-normative history. This preserves the meter-motion paragraph from
`docs-dev/vue_presentation_refinement_direction.md` before the owner's 2026-11-16
clarification that the fill should show the instantaneous received peak versus
the momentum peak. The replacement is in
[Vue presentation refinement](../vue_presentation_refinement_direction.md).

> Use the same RMS fill and peak marker in both modes. A new peak snaps the
> marker up and resets its downward velocity. After a short hold, the marker
> falls with increasing speed toward zero, staying at least as high as the
> currently measured peak. Smooth the RMS fill with fast attack and slower
> release. Run this motion on the client at display refresh, independently of
> telemetry delivery. Missing/unavailable data remain unavailable rather than
> becoming silence. Reset obsolete-source motion on activation/invalidation and
> stop animation when hidden or settled. Hold and release tuning can be adjusted
> for feel without changing capture semantics.

The preceding implementation used a 15 ms attack and 100 ms release for the RMS
fill, independently of the held peak's 120 ms hold and accelerating fall. RMS
therefore represented a different measurement from the peak marker, not merely
an interpolated version of it. The new fill uses the latest received block peak
directly; RMS remains available numerically. Its obsolete animation is removed.

The source-tuning guide previously named `.rms-fill` and RMS attack/release in
`MeterBallistics.advance()`. It also described shared lane-content padding that
grew from 0 px at height 16 to 4 px at height 48. The default meter viewport now
bypasses that padding, and `LiveOutputMeters` no longer adds 8 px at either side.
Specialized lane content retains its existing padding rule.
