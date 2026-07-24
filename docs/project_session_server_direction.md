# Project Session Server Direction

## Purpose

Intravenous should support a project-scoped runtime session that can be used by
multiple clients. This is an architectural direction for future work; it does
not prescribe an immediate implementation.

The goal is to avoid coupling the runtime to a particular editor, remote
development product, or collaboration extension. VS Code/VSCodium, Open Remote
- SSH, P2P Live Share, command-line tools, and future clients should be able to
connect to the same project session through a common protocol.

## Intended model

```text
VS Code client ─┐
CLI / test tool ┼──> project session server ──> project state and audio engine
shared client ──┘              │
                                ├─ ordered mutations
                                ├─ state snapshots
                                └─ notifications to subscribed clients
```

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

## Session protocol requirements

The server must be genuinely multi-client. The current single-client socket RPC
shape is not a sufficient foundation for this direction.

- Each client connection has an independent output queue so notifications never
  interleave on a transport.
- State-changing requests are executed through one ordered command executor.
- Read-only requests may run concurrently only against a safe snapshot.
- State changes produce notifications for subscribed clients.
- Snapshots and notifications carry a monotonically increasing session
  `revision`.
- Clients can subscribe from a known revision. If the server cannot replay the
  required history, the client requests a fresh snapshot.
- Mutations that replace or edit existing state may include an
  `expectedRevision`; stale commands fail deterministically rather than silently
  overwriting newer state.
- Clients have identities and capabilities appropriate to their role. This is
  especially important for controls that start audio, change devices, or expose
  a network stream.

Threading and atomicity belong at this server boundary. Extension-side locks
cannot make concurrent clients coherent.

## Local discovery and election

Live process data should not be committed or treated as durable project data.
Use a per-user ephemeral runtime directory keyed by a hash of the canonical
project root, for example:

```text
$XDG_RUNTIME_DIR/intravenous/<project-hash>/
  lock
  descriptor.json
  server.sock
```

`descriptor.json` should describe a protocol version, PID, startup time,
endpoint, and a randomly generated local capability. A lock protects the
discover-or-launch path. Stale descriptors are removed only after checking the
lock, endpoint, and process liveness; this avoids one client deleting another
client's newly created session.

Platform implementations can use a Unix-domain socket on Unix-like systems and
an equivalent local endpoint on other platforms.

## Cross-machine access is a transport concern

Sharing a project path does not reveal a running process on another machine. A
guest copy of a workspace cannot discover a host Unix socket without an explicit
route to it.

Keep the project-session protocol separate from endpoint transport:

```text
project session protocol
        ↓
endpoint descriptor: local socket, TCP URI, or forwarded URI
        ↓
transport resolver: local, SSH tunnel, collaboration port-forward, manual URI
```

An integration with P2P Live Share or another collaboration product may provide
a transport resolver or port forwarding. Intravenous should consume the generic
endpoint and protocol rather than depend on that product's internal APIs. SSH,
LAN, a manual connection, or another collaboration tool can then use the same
session server.

## Consequences for remote audio

Remote audio should be modelled as a server-owned resource with authorized
consumers, not as a side effect of one VS Code extension instance. A remote
extension may manage a PipeWire/GStreamer/WebRTC sidecar, but it does so as one
client of the project session server.

The intended media path is:

```text
Intravenous output device
  -> temporary PipeWire/Pulse virtual sink
  -> monitor source
  -> server-owned GStreamer/WebRTC/Opus relay
  -> authorized client browser/webview audio receiver
```

This deliberately leaves the Intravenous audio engine unchanged. The engine
selects an ordinary output device; a relay captures that device's monitor and
uses established media tooling for Opus encoding, jitter handling, encrypted
UDP transport, and local playback.

The relay is an optional sidecar, built separately from the core runtime when
PipeWire/PulseAudio and GStreamer WebRTC support are available. It should be
started and stopped by a server-side audio relay manager, not directly owned by
an individual editor process. Its signaling is carried through the project
session protocol; it does not need a public control port.

An audio consumer holds a lease. The first valid lease starts the virtual sink
and relay; the final released or expired lease tears them down. The temporary
device choice is a session-scoped override and must not silently replace the
project's persisted audio-device selection. The server records and restores the
previous output device when monitoring ends.

This permits later additions such as:

- multiple observers of the same project session;
- a local editor attaching to a remote project server;
- a collaboration guest receiving authorized state and audio;
- replacing the editor-specific audio transport without changing the audio
  engine; and
- clean resource ownership and teardown when individual clients disconnect.

## Staged delivery

### Stage 1: one Remote SSH user

The first useful experience is: open a workspace on a more capable remote
machine and immediately produce audio while monitoring it on the local machine.

1. Add local project-session discovery and create-or-connect election in the
   remote extension host.
2. Make the extension a client of the project session, rather than assuming a
   private child server is exclusively its own.
3. Add a server-side audio relay manager and an optional GStreamer relay
   executable.
4. Add a VS Code webview receiver using browser WebRTC and Web Audio.
5. Carry offer/answer/ICE signaling through ordinary extension-to-webview
   messages, which Remote SSH already transports as control traffic.

For this stage, one client and one audio lease are sufficient as a product
constraint, but the request/response shape must already identify the client and
use leases. Do not encode the assumption that the extension process owns the
audio relay or that it is the only future client.

The remote machine requires an allowed UDP path for WebRTC media. The local
machine initiates connectivity checks, which normally works through tethering
or NAT. Opus bitrate, rather than the choice of UDP versus TCP, controls mobile
data consumption; a 64--96 kb/s stream is the intended initial range.

### Stage 2: one shared session, N SSH users

Generalize the same project session so several clients attach to the server on
the host machine:

1. Replace any single-client socket state with a connection/session map and
   per-connection outbound queues.
2. Execute mutations through one ordered command executor.
3. Add revisioned notifications, snapshots, and reconnect/resynchronization.
4. Allow each client to acquire its own audio-monitor lease and WebRTC stream.
5. Make the session endpoint reachable through an SSH forward or other generic
   transport resolver.

There remains one authoritative audio engine and one authoritative project
state. Each client receives synchronized state notifications and, if permitted,
an individual audio stream. The server may share a captured monitor source and
encode separately per consumer when necessary; optimization to share an encoded
stream is a later concern.

Source-file synchronization is a separate concern from project-runtime state.
A collaboration tool, shared filesystem, or other mechanism keeps text files
consistent; the session server observes/reloads them. Graph edits, playback,
audio-device state, and other runtime mutations always go through the ordered
project session protocol.

### Stage 3: collaboration transport adapters

Export an explicit, capability-protected session endpoint for a host that wants
to invite another user. A transport adapter resolves that endpoint into the
same session protocol connection. Initial adapters can be local Unix sockets
and SSH TCP forwarding. A collaboration product's port forwarding may be added
later as another adapter, without making the core runtime dependent on that
product.

This ordering makes the single-user Remote SSH workflow valuable immediately
while preserving the exact server, protocol, audio lease, and state model
required for a shared live project.

New features should avoid assumptions that an extension is the only client,
that a workspace has only one editor, or that the process launching the server
will remain connected for the server's lifetime.
