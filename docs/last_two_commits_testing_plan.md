# Last Two Commits Testing Plan

This note tracks the test work for the last two commits:

- `828ea17` project persistence, replay, UUID-backed identity, explicit
  `project.save`
- `75e4897` typed `ProjectOverrideSettingsRequest` cleanup

## 1. Override Request Parsing Matrix

- test each supported `project.overrideSettings` field individually
- test multiple supported fields together
- test supported fields mixed with unknown sibling keys
- test `default` normalization to no concrete override
- test invalid type for each supported key
- test invalid string and negative values for compiled sample cache chunk size
- test command with only unknown keys

## 2. Subscriber Semantics For Typed Overrides

- test timeline execution subscriber directly with typed requests
- test audio device lanes subscriber directly with typed requests
- test iv-module reload subscriber directly with typed requests
- test irrelevant fields are ignored by each subscriber
- test empty typed request produces no mutation

## 3. Best-Effort Replay Sequencing

- test first command fails and later command still succeeds
- test middle command fails and later unrelated command still succeeds
- test bad override command does not stop later commands
- test one bad lane connection does not stop later lane-view restore
- assert error and warning notification counts

## 4. Save/Load Round-Trip Stability

- save, load into a fresh runtime, save again
- compare normalized output
- cover iv-module instances
- cover graph input state
- cover lane views
- cover lane sample channel types
- cover lane connections
- cover project overrides

## 5. Save Normalization Edge Cases

- unchanged settings omitted from `project.overrideSettings`
- relative toolchain paths emitted relative to project root
- external paths remain absolute
- device ids omitted when matching startup defaults
- chunk size omitted when matching startup default
- lane, view, and connection ordering remain stable

## 6. Graph Input Authored-State Coverage

- sample input values
- sample input states
- event input states
- sample output states
- event output states
- member and non-member ports
- lane-bound states using UUID lane ids
- disconnected, default, logical, and timeline-lane variants

## 7. Timeline/Lane Identity And Connectivity

- lane UUID persistence through save/load
- connection replay using saved lane UUIDs
- missing source lane fails cleanly
- missing target lane fails cleanly
- invalid sample channel type target fails cleanly
- later connections still succeed after earlier failures

## 8. Explicit Save Surface

- `project.save` unbound
- `project.save` with empty contributor set
- `project.save` after mutations but before disk write
- repeated `project.save` idempotence
- write failure propagates as RPC error
- contributor exception propagates as RPC error

## 9. UUID / Interning / Identity Stress

- same string interns to equal identity repeatedly
- different strings remain distinct
- generated UUIDs match v4 formatting expectations
- large-sample uniqueness sanity check
- persistence-related objects preserve ids across save/load/save
- lane views and managed lanes preserve ids by value, not creation path
