# Typed node-event bindings

The backend publishes a neutral event protocol. The client owns TypeScript
generation and export names. `automation.node-events.json` is a compiler-produced
snapshot of the three automation events, extracted from the existing shared
GraphJit test package artifact on 2026-11-14. Its definition ID was subsequently
updated to `builtin.automation.curve` for builtin promotion; the compiler's
event identities, fingerprints, and payload schemas are unchanged. It contains
no TypeScript rules.
`beats.node-events.json` contains the `SetBeatGrid` identity and payload schema
from the builtin package compiler metadata.
`automation.names.json` selects nominal identities and assigns frontend export
names. Display names do not select or identify events.

From `vscode/client`, after installing its npm dependencies, regenerate the
checked-in binding with:

```sh
npm run generate-node-events -- bindings/automation.node-events.json bindings/automation.names.json src/generated/automationEvents.ts
```

The generator also accepts a fresh `<package>.ivpkg.bc.node-events.json` directly
as its first argument. Use the default builtin package's sidecar after changing its
C++ event definitions. An optional fourth argument sets the import path for the
generated file's runtime helpers. It defaults to `../nodeEvents`.

If the protocol contains several fingerprints for one nominal identity, specify
both `nominalId` and `definitionFingerprint` in the names file. Generation rejects
ambiguous selections, invalid export names, export collisions, and unsupported
payload schemas. Events shared by several definitions need only one factory.

A Vue presentation uses the generated factory:

```ts
import { MovePoint } from "../../src/generated/automationEvents";
import { useNode } from "./nodeContext";

const node = useNode();
await node.value.send(MovePoint({ id: 4, seconds: 2, value: 0.5 }));
```

Factories copy and validate their payloads. Callers cannot substitute raw JSON
for a typed event value. Integer arguments must be exactly representable as
JavaScript numbers. Values outside that range are rejected, including unsafe
64-bit integers. Enum payloads use the compiler's enumerator names. Converted
scalars still receive their provider-specific validation in the backend.

Before dispatch, the extension checks the current receiver and the exact event
fingerprint through `nodeMessages.getProtocol`. It captures the session revision
before that read and uses it for `nodeMessages.dispatch`. A concurrent edit can
reject the request. The client does not retry the event against another revision.
Presentations without a resolved stable `NodeAddress` receive an explicit error.

The webview root owns pending completions. Presentation remounts and display-mode
changes leave them intact. Disconnect rejects them. Errors are also reported by
the extension host, even if the originating component disappeared.

For several debounced updates that should undo together, create a gesture:

```ts
const drag = node.value.gesture();
await drag.send(MovePoint({ id: 4, seconds: 2, value: 0.5 }));
await drag.send(MovePoint({ id: 4, seconds: 3, value: 0.75 }));
await drag.finish();
```

The first send opens the server group. Each accepted message publishes normally;
finish closes its one history entry. Capture the handle outside the component
lifetime if the interaction can survive remounts. `PluginLane` owns the handle
and passes replies to `AutomationCurveEditing` through
`PresentationEditRequests`. Closing a view or disconnecting closes
accepted history. Conflicting edits end the old group; later updates reject rather
than retry against the new curve. Gesture closure does not cancel accepted data.

The bundled curve presentation reads accepted constructor values from
`graph.getScope`, independently of desired source text. Its source and new backend
routes await owner verification. The earlier status is preserved in
[historical documentation](../../../docs/historical/stage_3_before_automation_presentation_2026-11-14.md).
