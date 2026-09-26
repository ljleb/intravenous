# System Audio Devices Direction

_Status: current device-domain design after deletion of the old device-execution
integration._

This document supersedes the historical automatic device/execution integration.

## Responsibility

`SystemAudioDevices` owns system-audio concerns only:

- device enumeration and stable-id discovery;
- logical device bindings;
- hardware-device open/close/reopen lifetime;
- callback buffering;
- clock/synchronization/resampling policy;
- mapping the stable logical identity `default` to the implementation-defined
  current default device.

It does not own project graph nodes, project connections, node-instance ids, or
automatically managed graph structure.

## Device nodes are ordinary project nodes

System input/output node definitions are ordinary leaf node definitions. A user
creates them through the same `ProjectGraph` node-instantiation surface used by
any other node.

The node's immutable configuration contains a stable device-id value object,
for example:

```text
"default"
"backend-specific-stable-device-id"
```

`default` is a stable logical identity and may be persisted like any other
requested id. No caller outside `SystemAudioDevices` needs to branch on whether
an id resolves to hardware, is missing, or dynamically changes its backing hardware.

Automatic creation/removal of one graph node per detected device is an optional
future convenience service. It is explicitly not the foundation of device
support.

## Binding resolution never fails because hardware is absent

For every syntactically valid requested device id, `SystemAudioDevices` should
be able to provide a stable logical binding even if no hardware device currently
resolves to that id.

The unavailable behavior is deterministic:

- input binding: produce silence;
- output binding: accept and discard provided samples.

The binding object remains valid while a hardware device disappears, appears,
is reopened, or while the implementation's `default` mapping changes.

This lets a persisted project configure an unavailable device successfully and
start executing immediately. When hardware later becomes available, the same
binding may begin communicating with it without requiring project topology
mutation merely for availability.

## Node initialization

A system-audio leaf node may resolve the logical binding in `Node::initialize()`
using the stable id value object.

The resolution may be a direct service query or an indirect linker-set event,
but it must ultimately ask for the binding by stable id. The node must not retain
a fragile raw reference to a current miniaudio device object.

The binding interface itself must be safe if hardware availability changes at
any point during graph execution.

## Synchronization remains device-domain policy

The useful synchronization work from the historical audio-device design remains
applicable as an internal `SystemAudioDevices` concern:

- one or more device clocks may differ from the graph execution clock;
- buffering and adaptive resampling may be required;
- hardware callback sizes may not equal project graph block size;
- device disappearance/reappearance must not corrupt graph state.

How `GraphExecutor` chooses its pacing policy is an execution design detail, but
hardware-device synchronization should not leak into `ProjectGraph`,
`NodeInstances`, or `GraphConnections`.
