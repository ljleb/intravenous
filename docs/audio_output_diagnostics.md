# Audio output diagnostics

The owner reports no audible graph output. The active device status alone does
not establish that a graph runs or that it produces a nonzero signal. These
temporary probes locate the first failing boundary before a targeted fix.

Rebuild the server and extension, then restart the Intravenous server. Open the
Intravenous output channel in VS Code. Search for `Intravenous audio probe:`.
The server also writes these lines to its diagnostic file, whose path appears
in the client startup log. Wait until the package rebuild finishes, then retain
several consecutive reports. No Command Palette recorder is required.

Reports arrive every two seconds. Each audio stage samples approximately one
block per second, rather than scanning every block. This is a diagnostic sample,
not a complete recording or a continuous meter. A transient between sampled
blocks can be missed. Set `IV_AUDIO_DIAGNOSTICS=0` in the server environment to
disable the probes. Restart the server after changing this variable.

| Field or stage | Meaning |
| --- | --- |
| `device` | The selected device ID, or `disabled` |
| Earlier `active_generation` / `pending_generation` fields | Retired service-thread reads; use the render-thread observation below |
| `render` | The output buffer after graph execution, before device submission |
| `render.generation` | The generation executed for that sampled block |
| `ticks` | Tick calls during the sampled device block, including project-block slices |
| `binding_calls` | Output binding requests during that sampled block |
| `matched` | Requests whose binding matches the selected device |
| `playback` | The final miniaudio application callback buffer, after the render response |
| `block` | Number of blocks seen by that stage since the device started |
| `samples` | Number of interleaved samples in the sampled buffer |
| `peak` | Largest finite absolute sample value |
| `rms` | Root mean square of finite sample values |
| `dc` | Mean of finite sample values, which reveals a constant offset |
| `nonfinite` | Number of NaN or infinite samples |

Fields such as `peak` occur inside each stage's log section. The table's
`render.generation` notation refers to `generation` inside the `render:` section.
The two stage observations are independent. Compare successive reports and block
counts, rather than assuming each line describes the exact same buffer.

If `render.generation` remains zero after a successful package build, investigate
graph compilation, publication, and activation. If Tick runs but binding calls remain zero, the graph
does not reach an output sink. Binding calls with no matches indicate a device
selection or binding problem. Matched bindings with zero peak place the silence
at or before graph output. A large DC offset with no variation can also be inaudible.

Nonzero render values that disappear at playback place the failure in the device
handoff. Nonzero playback values place it after the application callback, including
OS routing, mute state, or miniaudio's later processing. This probe cannot establish
that samples reached the speakers. Repeated unchanged block counts indicate a
stalled stage or an old observation. Device replacement restarts its block counts.

`no observation` means that the stage has not published a stable observation yet,
or that a read overlapped publication. The silent fallback clock has render probes
but no native playback callback. The existing startup device-status line identifies
hardware activation versus fallback.

The audio writers publish fixed-size observations using lock-free atomic fields.
A reader makes one attempt and never delays a writer. Formatting, stream output,
socket notifications, and RMS calculation run in `RuntimeExecutionService`.
Diagnostic storage outlives the devices. The default library path has no probe
owner, so tests and other users enable sampling explicitly when needed.

The cause of the owner's silence remains unconfirmed. Once the reports identify
the failure, add a regression for that behavior and remove or disable the temporary
application probes. The present tests cover diagnostic observations and the
existing graph-to-device binding handoff, not the unresolved audible failure.

Source inspection found one concrete project-level candidate: the saved
`iv.project.saw` input `detune` is zero. Its 17 identical-frequency oscillators
have gains that cancel when summed. An algebraic check gives approximately zero
gain in both stereo channels. The declared nonzero default is `2.5`. Compare the
probe readings at zero and nonzero detune before treating silence as a backend
failure. This finding does not establish the signal in the running graph, and
the project file was not changed by the agent.

## First owner report, 2026-11-14

The owner reports advancing render and playback block counts, with zero peak,
zero `render.generation`, zero Tick calls, and zero binding calls. This remains
true after “IV package build ready to apply.” The failure is before audio graph
execution. The detune cancellation candidate cannot explain the lack of Tick.

Source inspection found a reporting defect: `ProjectGraph::rebuild_locked()`
retained GraphJit failures in `ProjectGraphGeneration::graph_jit_diagnostics`
without including them in the root's publication. A package refresh could report
success while its configured graph had no compiled artifact and was never staged.
The exact compiler diagnostic in the owner's run is not yet available.

`ProjectGraphPublication::execution_failure` now carries attempted graph-compilation
failures back through the existing cause's publication. `SocketRpcServer` publishes
this as `server.status` with code `graphCompilationFailed`, after the package-build
status. The client logs it as a rebuild failure and retains its diagnostic details.
The message includes node and package context when GraphJit supplied them. A library
configuration with no bound compiler is not reported as an attempted compile failure.

The first probe version also read `GraphExecutor::active_generation()` and
`pending_generation()` from the service thread. Those getters expose realtime-owned
pointers. These added reads were removed; `render.generation` is captured in the
render thread and published through the probe's atomic fields.

Added regressions for preserving a failed compile's diagnostic alongside configured
interfaces, and delivering its failure after the package-success notification.
Builds and tests remain owner-run. Rebuild and restart to obtain the actual GraphJit
diagnostic before selecting a compiler or graph fix.

## Compiler failure identified, 2026-11-14

The next owner report contains the error:

```text
GraphJit lowering has no resolved implementation for a concrete primitive
```

`GraphJit::capture_inputs()` collected only leaves with a public registration.
Module expressions such as `center - spread * 0.5` create internal arithmetic
leaves without that registration. The panner uses these expressions. The package
contains their executable code, but GraphJit omitted them from its implementation list.
The lowerer rejected the graph before any Tick could execute.

The fix exports every package compiler record through
`iv_package_node_compiler_records`. `ModuleLoader` copies the records into the
accepted `PackageRevision`. GraphJit resolves an internal leaf by both its
`NodeCodeKey` and its configuration declaration callback address. The address
distinguishes identical types compiled in different packages. Public registered
leaves retain their existing definition and provider validation.

This lookup runs during compilation. It adds no audio-thread operation or buffer.
The existing package ownership pins the callbacks and data. Missing internal
implementations now fail during input capture with the concrete type and bundle
index, instead of reaching the generic lowerer error.

Added regressions execute a module containing arithmetic and a private leaf,
reject a different package with the same code key, and report a missing private
record. Builds, tests, and audible output remain owner-validated. Rebuild and
restart the server, then compare the generation, Tick count, and output probes.
The saved detune value remains a separate possible source of a zero signal after
graph execution starts.

## Module sum routing identified, 2026-11-14

The next owner log gets past implementation capture and reports:

```text
GraphJit structural sample input has more than one source
```

`build_structural_sample_routes()` accepted only one incoming connection for a
module boundary channel. The oscillator module calls `g.outputs(...)` for each
of its 17 voices. These calls contribute to the same output. The router rejected
that sum before graph execution.

The router now combines every incoming contribution. Each connection retains
its conversion coefficients and detach settings. Module manual controls still
follow their connection participation and override policies.

The route also tried to assign one registered speaker layout to its entire list
of producer channels. Seventeen stereo voices contribute 34 channels to a stereo
sum. That list is not a 34-speaker output. Explicit matrix dimensions now describe
the list directly. The existing lowering and storage planners already accept
explicit matrix dimensions.

The native mix operation also used a temporary array sized for speaker channels.
Its compiler plan now reserves one reusable array for the largest producer list.
The array belongs to each root invocation. Small compositions keep their existing
fixed array. The audio thread performs no dynamic allocation.

New regressions cover repeated module outputs, multiple sources entering a
module input, conversion coefficients, and manual-control policy. A runtime
regression sums 17 stereo ramps and one mono ramp through nested boundaries.

The owner test run also exposed an error in the preceding regression fixture:
`graph.node<PrivateArithmeticProbe>()` is not a public construction overload.
The shared package failed to compile, so its dependent tests did not run.
The fixture now uses `details::configure_concrete_node`, the existing internal
construction function. This test exercises internal compiler-record lookup and
does not add a public leaf-construction interface.

The owner must rebuild, run the tests, and restart the server to validate these
changes. The latest live report still shows no executing generation.

## Audible output confirmed, 2026-11-14

After reconnecting the nodes, the owner reports audible audio. Generation 3
executes one Tick and one matched output binding in each sampled block.
Render and playback report identical nonzero peak, RMS, and DC values, with no
nonfinite samples. This confirms signal delivery through the application callback.

The saved signal path still connects the oscillator module to the panner and
then the system output. The connection IDs changed. The preceding generation-1
error, `GraphJit sample target channel has more than one source`, remains an
unresolved normalization case. Successful live rewiring does not establish that
the original saved graph failed because the owner connected it incorrectly.
The target-channel rejection was not removed or given different reduction semantics.

The latest shared test setup fails because the new arithmetic regression uses
scalar-literal shorthand. That shorthand requests the shipped `constant`
definition, while this fixture loads only its own package. The regression now
constructs the fixture's existing registered constants explicitly. Arithmetic
still creates internal leaves and exercises the intended compiler-record lookup.
The owner must rerun the test suite after rebuilding this correction.
