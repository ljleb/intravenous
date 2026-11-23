# Project-Defined Port Types and Value Boundaries

_Status: deferred proposal, recorded 2026-11-21._

Revisit this design after the planned UI work. That work includes WebAssembly
presentation plugins, the VS Code side panel, project-authored modules,
C++-managed modules, and the UI refinements that follow. This note records the
working model for a future review. The API and storage formats are open.
[Project presentation plugins](./project_presentation_plugins.md) ·
[Module hierarchy](./module_interoperability_and_graph_macros_direction.md#nested-module-presentation-and-project-owned-scopes)

## Project-defined types and operations

Today, `IV_CONTINUOUS_VALUE_TYPES` generates the continuous type registry,
`ContinuousValueTypeSet` uses a fixed mask, and the port metadata uses closed
C++ variants. For events, `TimedEvent` embeds the closed `Event` variant.
Adding a descriptor to a table at startup leaves those compiled layouts in
place. An open registry also needs representations that can describe types
unknown when the server is built.
[Continuous values](../src/intravenous/continuous_value.h) ·
[Event ports](../src/intravenous/ports.h)

The proposed model keeps authored C++ node code statically typed. A package
exports its type descriptors and compiled operations. The Clang source
introspection plugin can check declarations and emit metadata. An explicit C++
registration declaration can also supply that metadata. Project loading can
assign dense indices once. Use stable names in saved data and RPC, and retain
the package revision as long as compiled code or data refers to it.

For an operation defined by a source and a target type, look up the pair during
graph preparation. An indexed function table supports an indirect call when
the types are known only at runtime. When GraphJit knows the pair, it can emit a
direct call. If the implementation is available as bitcode, LLVM can inline it.
Measure any dispatch that remains on the playback path.

A generated project header and a project-specific server build can also extend
the closed C++ lists. That option requires compiling the server and each package
that shares the affected layouts against the same definitions. Prefer a shared
server with package-owned operations for the first investigation. For events,
one candidate is preallocated payload storage described by the buffer type,
with separate timestamps. Typed callbacks still receive their C++ payloads.

Other candidates from the discussion include channel conventions, conversions,
observation operations, and codecs and editors for construction arguments.
A package can extend the operations of a type it imports. The graph continues
to define the Sequential, Region, and Coverage contracts.
[Observation operations](./vue_output_observation_direction.md#2-one-operation-catalog-type-owned-capabilities)

## Tensor axes and port matching

A tensor is a value whose axes define its shape. A scalar has zero axes.
An axis can have a plain extent, such as 512 bins, or a convention that gives
its positions meaning. A stereo speaker axis has two named members. A stereo
spectrum can have speaker and frequency axes. An image can have height, width,
and color axes.

Keep three properties explicit in port descriptions.

- The logical schema gives the element type, axis identities, extents, and
  conventions. A port can leave a property open for the graph to infer.
- The convention states which members can be routed or tiled independently.
  Speakers can be exposed for named routing. The components of a color pixel
  can stay together even when storage gives each color its own plane.
- The representation gives storage order and strides. A stride is the distance
  in storage between adjacent positions along an axis. Planar and interleaved
  stereo transpose the storage order of the frame and speaker axes. A consumer
  that accepts the resulting strides can read a view. A consumer that requires
  contiguous data in a specified order needs a reorder.

Require a logical match unless an applicable conversion is declared. A
representation change can be automatic when the logical mapping is
unambiguous. Mixing stereo to mono, resizing an image, or changing its color
encoding requires a defined operation. If the map is missing or ambiguous,
reject the connection or require an explicit conversion selected by the author.
Channel counts alone do not determine a map. In an open registry, each package
can supply the conversion pairs it supports.

## Value boundaries and execution batches

Ports expose the values the producer emits, including their boundaries. A
flat sequence is a view that a type such as `Sample` can offer when joining
batches preserves the positions and meaning of its samples. The timeline
positions must be adjacent, and the storage must support the view.

An `FFTBlock` is a complete spectrum at a timeline position. Its analysis
window can overlap the next spectrum and can be wider than the time between
spectra. Its frequency bins are positions within a value. If a call receives
eight spectra, the node sees eight complete values. GraphJit can divide that
batch between calls when the graph allows the cut. Selecting some frequency
bins is a separate operation with its own semantics.

The data type or axis convention defines legal cuts and how the pieces
reassemble. The node can impose a minimum processing size or alignment and
carry state between calls. After resolving types, shapes, and port pace,
graph preparation can validate these requirements when it schedules feedback.
A strongly connected component is a set of nodes that can reach each other
through graph connections. If a feedback loop needs one-sample steps and a
node requires a whole frame, report the conflicting requirements before
lowering the graph to executable code.

When work resumes, use samples, overlapping spectra, and image frames to
review the same port model. Resolve how data conventions declare legal cuts,
which conversions are eligible for automatic insertion, and how package
revisions affect type identity and stored data.
