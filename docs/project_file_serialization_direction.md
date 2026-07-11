# Project File Serialization Direction

## Current understanding

Project persistence should store authored project state, not runtime object graphs and not execution-derived state.

That means we want to persist:

- iv-module instances that exist
- authored per-instance settings, such as `default_silence_ttl_samples`
- authored graph-input override state
- authored knob override values
- the authored timeline lane DAG
- authored lane-node parameters
- later, automation data as its own first-class persistent structure

That means we do **not** want to persist:

- `GraphInputLanes`-derived timeline lanes that are reconstructible from graph structure
- execution caches
- task graphs
- descriptor layouts
- other execution-owned derived state

## Command model

The important ingress surface is not JSON-RPC itself. The important surface is the existing typed linker-event command API that JSON-RPC already invokes.

JSON-RPC is currently best understood as:

- wire-format parsing
- request validation
- conversion into typed event invocations

This is only mostly true today. Some socket-RPC bridges still contain extra policy, such as directly persisting config updates. That logic should eventually move into proper app modules.

The useful architectural consequence is:

- project file replay does not need to go through the UI
- project file replay does not need to store JSON-RPC specifically
- project file replay can use any serialized format that can be decoded into the same typed request structs and `IV_INVOKE_LINKER_EVENT(...)` calls

The currently chosen persisted format is:

- JSON
- one command object per line
- each command object has exactly `command` and `args`

For `project.overrideSettings` specifically:

- recognized keys are parsed explicitly before dispatch
- unknown sibling keys are logged
- recognized keys must still apply even when unknown siblings are present
- after parsing/logging, the original args object is forwarded to owning modules

## Persistence shape

The project file should not store C++ binary objects.

It should store a reconstruction program for the current authored project state.

The reconstruction program should be:

- semantic
- deterministic
- normalized

Normalized here means:

- not an ever-growing raw edit history
- not every intermediate UI gesture
- instead, the compact set of commands needed to recreate the current project

So the persistence shape is closer to:

- create this instance
- set this instance parameter
- create this lane
- set this lane parameter
- connect these lanes
- set this input override

than to:

- store every transient edit event forever

## Dedicated persistent-state module

We want a dedicated app module for persistent project state.

Its responsibilities should be:

- load the project file
- hold the normalized reconstruction program
- replay that program by invoking the same typed command events used by the live app
- write the project file through a server-owned autosave service

It rebuilds normalized save output from live module state when autosave is due.
Project replay must not schedule a save; autosave is enabled only after the
project-loaded event fires.

This module should not depend on the UI.

## Save policy

Project file writes should not happen on every state change. The server coalesces
ordinary authored mutations with a short debounce and writes atomically. The UI
can explicitly save immediately, or disable and re-enable server-owned autosave.

The intended save flow is:

- a project mutation is observed after project load
- autosave coalesces mutations and requests persistence when due
- project persistence raises its state-collection event into contributors
- contributors fill the structured builder
- the builder lowers to the normalized command list
- project persistence writes `project.intravenous`

This keeps serialization cost under control, keeps persistence independent of a
particular client, and lets app modules mutate live state freely without forcing
an immediate disk write.

## Open questions

- Which currently existing linker events are already suitable as canonical reconstruction commands, and which authored-state commands still need to be introduced.
- How lane-view 2D layout should be represented and restored from the VS Code side once that deferred work starts.
