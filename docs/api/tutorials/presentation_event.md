# Send a node event from a C++ presentation

Drag a preview while the server places an automation point. Follow the
reply to see when the edit has reached the project. Use the same scratch
project as [Change a C++ presentation](presentation_cpp.md).

1. On the Placement Demo lane, choose **Placement Event Example**. Drag
   the yellow line upward and release it. The line follows your pointer.
   The status changes from “Placement pending” to “Placement accepted”
   when the accepted project revision is available.
2. Open `placement.cpp` in the scratch package. Follow `read_input` and
   `submit`. On begin, `frame.begin_edit()` reserves the ID for one undo
   group. `frame.send(PlacePoint{…}, gesture_)` copies the event into the
   current frame. It returns an ID that the guest saves in `pending_`.
3. Follow `receive`. The host delivers the result to `frame.edit_replies()`
   in a later frame. While the request is pending, `read_input` updates
   `desired_` and `present` draws it. After the insert succeeds, `submit`
   sends the most recent value as a move if it differs from `sent_`.
   Both events use the same `EditGesture`. On release, after the final
   edit succeeds, `finish_edit` closes the group.
4. Switch the lane to Automation Curve and inspect its accepted points,
   or run the headless scenario below on a fresh scratch copy. One event
   places point 17 at exact sample `9223372036854775805`, changes the
   segment of point 12, and supplies the order of points 21 and 17 at
   their coincident sample. Undo removes the gesture's edits together.

The example places point 17 once per scratch project. Undo the edit or use
a fresh copy to repeat the exercise. The header is generated from the same
node-event protocol as the ordinary curve editor. The presentation also
draws the curve's observed output in blue where it has samples in view.
That trace uses the waveform binding in the package manifest.
[Placement source](../../../examples/wasm-presentation/packages/marker/presentations/placement.cpp) ·
[C++ event binding reference](../reference/presentation_cpp.md#node-events)

## Run the headless scenario

Build the scratch artifacts as in the first tutorial. Set `IV_TEST_SERVER`
to the matching server executable. The runner prints frames and the final
snapshot, including the new point's exact sample and accepted value.

```sh
node build-release/vscode/client/dist/headlessClient.js present \
  --server "$IV_TEST_SERVER" --project "$scratch" \
  --scenario "$scratch/placement-scenario.json"
```

[Headless command setup](../../development_run.md#replay-graph-editor-interactions) ·
[Placement scenario](../../../examples/wasm-presentation/placement-scenario.json)
