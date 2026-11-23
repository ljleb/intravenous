# Observation window delivery by body ID

This records the earlier observation delivery protocol from the Vue client
implementation stages on 2026-11-19.
[The current delivery protocol](../vue_client_implementation_stages.md#observation-window-delivery)
replaces it.

`Observations` assigns an ID to the Regions, samples, marks, and unavailable
Regions in each ready window. A refresh with the same body keeps that ID. Status
and execution versions travel with each window reference. The socket writer
compares the frame selected for transmission with the last frame it wrote and
includes definitions in `windowBodies` for IDs absent from the previous frame.
This keeps observation updates replaceable while they wait for the socket.

The extension keeps the bodies referenced by the latest socket update. It
forwards only the bodies needed by each graph panel and stops sending a body
after that webview acknowledges receipt. A reloaded panel starts a new receipt
stream. The webview resolves references before applying observation results.
