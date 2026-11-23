# Vue Port Grab Before Native Window Focus Reporting

_Historical, non-normative. Preserved on 2026-11-13 from the uncommitted port
recorder status. Replaced by the focus handling and trace findings in
[Vue Client Implementation Stages](../vue_client_implementation_stages.md)._

The preceding status said:

> `GraphEditor.startConnection` captures source geometry, focuses the editor,
> and takes pointer capture before assigning `connectionDrag`. Blur handling
> ignores an event if the document currently has focus, and lost-capture handling
> ignores an event if the editor still owns that pointer. The owner confirmed
> that these changes did **not** fix the first grab after returning from another
> application or native window. The cause remains unconfirmed. The preceding
> status and muting rule are retained in
> [Vue Port Muting Before Destination-Side Feedback](./vue_port_muting_before_destination_side_only_2026-11-13.md).

> `portGrabRecording.test.ts` checks the report classification, reset, and bounded
> storage. It does **not** reproduce native activation or verify browser event
> delivery. A recorded failing attempt is still needed before adding a regression
> test for its cause.

The owner then provided a recording started at `2026-11-13T13:30:20.507Z`, from
VSCodium 1.116.02821, Electron 39.8.7, Chrome 142 on Linux. Relevant elapsed
times in milliseconds were:

| Time | Observed event |
| --- | --- |
| 3154.3 | Webview focus returned. |
| 3155.0 | Primary `pointerdown` reached the port circle. |
| 3155.6 | Gesture started with pointer capture. |
| 3156.7 | `GraphEditor.windowBlurred` cancelled the gesture with reason `window-blur`. |
| 3180.0 | Pointer movement still reported the primary button held. |
| 3206.3 | Webview focus returned again. |
| 3210.5 | Held pointer movement continued. |
| 4377.8 | Primary release arrived, with the gesture already cancelled. |

This established that the input was delivered and accepted, then cancelled by
the webview blur handler. It did not establish the internal cause of the brief
focus transfer inside VSCodium. Native-window focus reporting avoids treating
that transfer as an application switch. Manual confirmation of the resulting
fix remains necessary.
