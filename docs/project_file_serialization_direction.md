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
- update its stored authored state as project mutations happen
- write the project file only on explicit save

This module should not depend on the UI.

## Save policy

Project file writes should not happen on every state change.

They should happen only when an explicit save command is received from the UI.

This keeps serialization cost under control and lets app modules mutate live state freely without forcing immediate disk writes.

## Open questions

- Whether the persisted command file format should look JSON-RPC-like or use a simpler internal command syntax.
- Which currently existing linker events are already suitable as canonical reconstruction commands, and which authored-state commands still need to be introduced.
- How the persistent-state module should observe and normalize live authored mutations without pushing policy back into bridges.
