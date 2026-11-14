# Vue Shift Before Shared Modifier State

_Status: historical implementation checkpoint, preserved on 2026-11-13._

Source: `docs/vue_client_implementation_stages.md`, before the owner reported
that entering a grouped port with Shift already held could make its chooser
flicker while the webview was unfocused. `NodePorts.enterPort` read each hover
event's modifier flags, and `pointerModifier` could close the chooser from a
separate event. Replacing the hovered row could therefore open and close the
chooser from separate observations of the same stationary pointer.

The replacement is in
[Connection Editing](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).

## Previous checkpoint

Keyboard events reach the focused webview. With focus elsewhere, a stationary
Shift press cannot open the chooser through its keydown handler. Pointer entry
and movement over a port also check `shiftKey`, so the next pointer movement
opens it if Shift is held. Pointer movement within the chooser checks the same
flag to recover a missed release. Hovering does not take keyboard focus.
