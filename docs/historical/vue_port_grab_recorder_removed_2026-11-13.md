# Removed Port Grab Recorder

_Historical, non-normative. Preserved on 2026-11-13 from
[Vue Client Implementation Stages](../vue_client_implementation_stages.md).
The owner confirmed that the focus fix worked, then requested removal of the
debug recording action. The native-window focus fix and regression tests remain.
The following paragraphs and manual check describe the removed diagnostic._

The opt-in **Intravenous: Record First Port Grab** command records input in the
actual webview. `GraphEditor` reports focus/blur, pointer and compatibility mouse
events, pointer capture, accepted gesture starts, first drag motion, and
cancellation reasons. No recording listeners are installed until the command
runs. `PortGrabRecorder` keeps at most 256 events, removes repetitive motion,
and retains the initial 128 events when the limit is reached. The extension
opens the report in an unsaved JSON editor after the first primary release;
running the command again exports a recording if no release arrived. It does
not write a project file or change the graph.

`portGrabRecording.test.ts` checks the report classification, reset, and bounded
storage. It does **not** reproduce native activation or verify browser event
delivery. The event-order regression described above uses the owner's subsequent
recording. `pointerdown-not-observed` means no primary pointer press
reached the recorder despite held movement or release; `gesture-not-started`
means the press reached a port but no gesture began. `gesture-cancelled` retains
the reason that ended an accepted gesture. `grab-observed` requires accepted
start and drag motion without unexpected cancellation; it does not prove that
a connection was committed. Focus flags describe what this webview observed,
not events delivered elsewhere in VS Code or the operating system.

21. Run **Intravenous: Record First Port Grab** from the Command Palette, then
    switch to another application or native window. Return with one press on
    a port, drag into empty editor space without Shift, and release. Do not
    make a second attempt before saving the JSON report. If no report opens,
    run the command again to export it. Save both a failing attempt and a
    working attempt for comparison. Releasing a new connection in empty space
    leaves the graph unchanged; do not use Shift for this diagnostic, since
    releasing an existing connection in empty space disconnects it.
