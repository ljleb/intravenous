# Retired cursor return trace commands

_Historical record, 2026-11-19. The owner asked to remove the temporary
debugging commands after testing the fixes._

The following instructions came from the
[native client helper section](../vue_client_implementation_stages.md#native-client-helper-2026-11-18).
That section now describes the automatic error reporting.

> Run **Intravenous: Trace Next Automation Cursor Return** and drag one point
> to log the release coordinates and each decision that can prevent a warp.
> Run **Intravenous: Trace Next Middle-Button Cursor Return** and pan once to
> log pointer lock, the wrapped release position, and the X11 result.

> The middle-button trace checks the X11 position again after 35 ms.
