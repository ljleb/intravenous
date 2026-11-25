# Package Sandbox Security

_Owner prerequisite recorded 2026-11-25. This document specifies the boundary
and the evidence required before adding package installation or package listing._

## Trust boundary

Treat package contents, native libraries, dependency source, and VST plugins
as arbitrary code. Compile, configure, JIT-compile, and execute them in an
untrusted domain. This includes compiler and build-tool invocations, package
initializers, configuration callbacks, GraphBuilder, GraphJit, and GraphExecutor.
Built-in or signed code follows the same boundary. A graph's execution domain
must have no access to another graph's private memory or capabilities unless the
host explicitly authorizes an exchange. Separate JIT namespaces in one process
provide symbol lookup and linking.

The host owns sandbox launch and cleanup, project persistence, package acquisition,
resource authorization, audio device access, and client routing. Start package
work only after isolation and resource restrictions are in force. Expose the
required toolchain and explicit graph inputs through restricted mounts, handles,
or copies. Keep scratch writes private. The host alone publishes project files,
saved revisions, or installed package artifacts. Treat every sandbox result as
untrusted, including diagnostics, manifests, graph descriptions, compiled
artifacts, presentation messages, and data read from shared memory.

## Required properties

- **Confinement.** A sandbox can access its own memory and the specific regions
  and inputs granted to it. It cannot read or write other host files or memory,
  access the network, inspect or control unrelated processes, or acquire further
  privileges. Account for inherited handles and descendant processes.
- **Controlled effects.** The host checks the schema, size, permissions, and
  session context of each message before acting. A sandbox cannot choose an
  arbitrary host path, request a network destination through the package broker,
  overwrite a protected revision, or transfer a capability it was never granted.
  Authorized persistent writes are bounded and committed by the host.
- **Shared buffers.** Specify the writer, reader, bounds, lifetime, and generation
  of each buffer. Treat its writable contents as mutable even after a message has
  validated. Publish or copy data so the host uses the bytes it authorized.
  Revoke access and discard stale replies when a graph stops or restarts.
- **Resource containment.** Bound memory, CPU, processes, storage, and IPC, including
  host work induced by sandbox requests. Budget exhaustion, crash, and timeout have
  defined outcomes. A stalled graph cannot hold the audio or client routes open
  indefinitely.
- **Confidentiality.** Declare which inputs the sandbox may receive and which
  outputs may reach each recipient. Model audio, UI, logs, and export as explicit
  disclosures. A sandbox may encode its inputs in an authorized output. Define
  the timing and other side channels covered by the assurance claim.

## Installation and listing prerequisite

Apply this design before introducing a package installer, index, or user-facing
listing of installable packages. Treat metadata and artifact bytes from a source
or index as untrusted as soon as the host reads them. Listing uses declarative
metadata through a bounded decoder. If discovering definitions requires a build
or package load, do it inside the sandbox. A listing must never execute discovery
hooks in the host or install a package as a side effect. Installation is an
explicit host-authorized transaction. Verify the fetched artifact against the
selected identity before publication, with acquisition, unpacking, and activation
covered by the same boundary and resource policy. A package signature establishes
provenance under its signing policy. Isolation controls what the code can do.

`PackageDefinitions` already reports locally detected declarations and accepted
build revisions to the editor. That catalog reflects the current development
pipeline. The new installation and listing work depends on the security boundary
below before it can expose untrusted package sources to that pipeline.

## Verification and release evidence

1. Write the small transition model with a hostile sandbox and a trusted host.
   Define the initial state, granted capabilities, message effects, resource
   budgets, and authorized observers and disclosures. Make platform and hardware
   assumptions explicit. Capabilities include authority held by the host on the
   sandbox's behalf.
2. Check an invariant-preservation proof for every modeled transition. State
   confidentiality separately as a relation between executions with different
   secret inputs and the same authorized disclosures. Version the trusted
   statements and their assumptions. Review any change that weakens them as a
   security policy change. If Lean is used, build untrusted proof submissions
   in isolation and check them against host-owned theorem statements.
3. Connect the model to the executable launcher, host decoders and authorization,
   shared-memory manager, resource broker, persistence, and restart path. Include
   the client's package-derived presentation, drawing, and graph-edit consumers.
   For each invariant, record the implementation that enforces it, any remaining
   assumption, and the evidence for conformance. The abstract proof, executable
   conformance evidence, and platform assumptions each support a different part
   of the assurance claim.
4. Exercise the enforcement on the supported platform with denied resource
   accesses, malformed and stale messages, shared-buffer lifecycle changes,
   cross-graph requests, quota exhaustion, and interrupted acquisition and
   restart. Use the proof and these conformance checks as prerequisites for
   extending the package pipeline to installation or listing. Apply the same
   evidence requirement to new privileged effects and IPC or shared-memory
   extensions. Ordinary nodes can depend on the established boundary.

On Linux, an isolated worker using OS confinement and enforced quotas is a
candidate. Record how the launcher combines process, filesystem, and network
isolation with system-call restrictions, capabilities, and quotas before making
any platform claim. Measure the bounded audio handoff and recovery on the
selected implementation. The exact platform configuration and proof tooling
remain implementation choices.

[Linux kernel documentation on seccomp filters](https://docs.kernel.org/userspace-api/seccomp_filter.html) ·
[Lean documentation on checking proofs](https://lean-lang.org/doc/reference/latest/ValidatingProofs/)

## Current implementation and dependency order

`ModuleLoader::Impl::load_compiled_package()` loads dynamic libraries into the
server's shared ORC JIT and initializes package code before it calls the package
ABI version function. A package's constructors can therefore run with the server
process's privileges. The current package build worker and per-package JITDylib
organize work inside that process. [Current loader](../src/intravenous/module/loader.cpp#L1236)

Specify and prove the boundary model first. Then replace in-process package
loading, graph configuration, and execution with a confined domain and a bounded
host interface. Verify launch, teardown, and modeled host operations on that path.
Only then add package installation and listing. The sandbox and proof are pending.
