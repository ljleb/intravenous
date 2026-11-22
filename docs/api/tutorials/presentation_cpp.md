# Change a C++ presentation

In this tutorial, you will move a marker, change its color, and see the
lane reload. Use a development shell in the repository. Set up the
editor and its dependencies as described in [Run Intravenous for
development](../../development_run.md).

1. Make a scratch copy in the repository and open it with the development
   launcher. The copy includes the marker's built WebAssembly file.

   ```sh
   mkdir -p build-release
   scratch=$(mktemp -d "$PWD/build-release/iv-marker.XXXXXX")
   cp -R examples/wasm-presentation/. "$scratch/"
   scripts/run_dev.sh "$scratch"
   ```

2. Drag the blue marker toward the left side of the lane. It stays there
   when you release it. The marker saves its position as a fraction of
   the lane width.
3. Start the build watcher in another terminal. Reuse the scratch path
   from step 1.

   ```sh
   marker="$scratch/packages/marker/presentations"
   python3 scripts/build_presentation.py --watch "$marker/marker.cpp" "$marker/marker.wasm"
   ```

4. Open `marker.cpp` in the scratch package. Change `IV_INK_OUTPUT` to
   `IV_INK_HIGHLIGHT` and save. The watcher prints the published path.
   The lane changes color and keeps the marker where you left it.
5. Find `Marker::present` in the source. Its call to
   `frame.reserve_polylines(1, 2)` reserves one line and two vertices.
   Change both vertex x coordinates to `x + 5`. Save again. The line now
   lands five pixels to the right of the drag target.

You have changed the guest's drawing and reloaded it with its saved state.
The source in the example is also compiled by the
[documentation check](../how-to/validate_docs.md).
[Marker source](../../../examples/wasm-presentation/packages/marker/presentations/marker.cpp)
