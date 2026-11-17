# Vue visual tuning

This is a source-editing guide for presentation sizes and motion. It does not
change the node or audio contracts. The files below are the places to start for
small visual adjustments; no backend rebuild is needed for these values.

| Adjust | File and value |
| --- | --- |
| Editable / mini / nano height boundaries | [`displayMode.ts`](../vscode/client/webview/presentation/displayMode.ts): `editableHeightPx` (48), `nanoHeightPx` (16) |
| Smallest / initial / largest lane height | [`timelineViewport.ts`](../vscode/client/src/timelineViewport.ts): `minimumLaneHeight` (12), `initialTimelineViewport.laneHeight` (240), `maximumLaneHeight` (1600) |
| Header changes from stacked to inline | [`NodeLane.vue`](../vscode/client/webview/presentation/NodeLane.vue): `@container node-lane (max-height: 56px)`; independent of the presentation mode |
| Lane name size and header width | `NodeLane.vue`: `.lane-name` and `--node-label-width` |
| Scrollbar and selection marker | [`TimelineScopePresentation.vue`](../vscode/client/webview/graph/TimelineScopePresentation.vue): native `::-webkit-scrollbar` parts use VS Code theme colours; the track's gradient marks the selected lane |
| Content spacing at small heights | `NodeLane.vue`: `.lane-content` padding grows from 0 px at height 16 to 4 px at height 48 for specialized content; `.meter-content` removes it for default meters |
| Port circle size / stroke | [`NodePorts.vue`](../vscode/client/webview/presentation/NodePorts.vue): `.port-circle`; both direct and folded ports use it |
| Port mouse target size | `NodePorts.vue`: `.port-handle` and `.port-slot`; these are larger than the visible circles |
| Folded circle placement | `NodePorts.vue`: `.stacked-first`, `.stacked-second`; keep vertical centering at `50%` |
| Ports that fit before folding | [`lanePortLayout.ts`](../vscode/client/src/lanePortLayout.ts), plus `NodePorts.vue`'s `portHeight()` |
| Meter spacing and visualization context menu | [`LiveOutputMeters.vue`](../vscode/client/webview/presentation/LiveOutputMeters.vue): `.live-outputs`, `.output-channels`, `.visualization-menu`, `visualizationModes` |
| Meter colours, fill height, marker width | [`SampleMeter.vue`](../vscode/client/webview/presentation/SampleMeter.vue): `.peak-fill`, `.reading-mark` |
| Meter scale bounds | [`meterPresentation.ts`](../vscode/client/src/meterPresentation.ts): `levelDomain`, `valueDomain` |
| Peak hold and fall speed | [`meterBallistics.ts`](../vscode/client/src/meterBallistics.ts): `holdMilliseconds`, `relativeAcceleration`; only the held marker is animated, while the fill uses the latest received peak |
| Curve colour, stroke, point size | [`AutomationCurvePresentation.vue`](../vscode/client/webview/presentation/AutomationCurvePresentation.vue): `.curve-line`, `.curve-point`, circle `r`; the same curve path is scaled at every height |

CSS is in each Vue file's `<style scoped>` block. Theme colours come from the
`--vscode-*` variables. Prefer changing the visible shape separately from its mouse
target so a smaller icon does not become harder to grab.

After editing, run this from the repository root to rebuild and install just the
extension, then reopen the graph view:

```sh
./scripts/install_client.sh "$PWD/build-release/src/intravenous"
```

To check the client, run `npm test` and `npm run typecheck` in `vscode/client`.

Useful visual checks are the smallest lane height, each mode boundary in both
zoom directions, mono and stereo outputs, and direct and folded ports. Keep the
same colours and geometry for corresponding controls at different heights.
