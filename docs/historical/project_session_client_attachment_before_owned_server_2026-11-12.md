# Project Session Client Attachment Before The Owned-Server Default (2026-11-12)

_Status: historical, non-normative. Verbatim passages preserved on 2026-11-12 from
`docs/project_session_server_direction.md` at revision `37d89a8a`. They were
superseded the same day by the repository owner's decision that, by default, a
client owns the server process it launches: reloading or closing the window stops
that server, and a crashed server is restarted by its client. Sharing one session
between clients becomes a future opt-in mode, not the default. The replacement text
is in [Project Session Server Direction](../project_session_server_direction.md)._

## Why these passages were superseded

With the server outliving the window, "Developer: Reload Window" reattached to the
old server instead of restarting the application, and a server whose package
pipeline had failed kept being reused. For one person running the application,
restarting the window must restart the server. The shared-session design below
remains a valid destination for an explicitly enabled multi-client mode.

## Superseded passage from "Intended model"

````text
The session server owns mutable project/runtime state. A client does not own a
server merely because it started it.

On activation, a client should:

1. derive a stable identity from the canonical project root;
2. discover an existing session server for that project;
3. connect to it when one is live;
4. otherwise participate in an atomic create-or-connect operation; and
5. reconnect and resynchronize if its connection is lost.

This is preferable to deciding behavior from the presence of a specific VS Code
extension. In a Remote SSH window, the extension host discovers a server on the
remote machine because that is where it runs. In a local window, it discovers a
local one by the same mechanism.
````

## Superseded passage from "Current core implementation"

````text
The VS Code client now uses create-or-connect behavior. It discovers an existing
descriptor before it starts a server. It authenticates with `session.hello`,
subscribes from its last revision, and requests a snapshot when replay is not
possible. If the connection closes, it discovers or starts a session again.
Extension shutdown closes only the client connection. It does not stop the
shared server.

The inherited `--rpc-fd` transport remains available for tests and compatible
launchers. It creates one trusted client and does not publish a project endpoint.
````
