# Channel Type Registry Direction

## Goal

Channel types have one canonical declaration.  That declaration defines their
named members and is also the closed registry from which static and runtime
channel information is derived.

The system must support both static authored DSP syntax and runtime lane,
conversion, persistence, and RPC state.  Static code therefore uses a channel
descriptor type while runtime state uses its stable `ChannelTypeId`.

## Canonical registry

The registry is an X-macro list.  Each line declares one type and every member
of that type exactly once:

```cpp
#define IV_CHANNEL_TYPES(X) \
    X(mono, "mono", center) \
    X(stereo, "stereo", left, right)
```

The registry generates:

- `mono` and `stereo` descriptor types;
- typed member values such as `stereo::left`;
- `type::channel_names` and derived `type::channel_count`;
- the closed `SupportedChannelTypes` type list;
- stable runtime `ChannelTypeId` values and wire names.

The stable wire name is explicit so persisted data does not change meaning if a
C++ namespace is renamed.

## Authored syntax

`ChannelMember<Type, Tag>` encodes both its owning channel type and member at
compile time.  This supports the named-family form:

```cpp
g.outputs(
    "main"_P[stereo::left] = left,
    "main"_P[stereo::right] = right);
```

and the convenience form for a module's sole public sample output:

```cpp
g.outputs(stereo::left = left, stereo::right = right);
```

The latter lowers to one public family named `main`.  It may not be mixed with
explicitly named output families in the same `outputs(...)` call.

## Runtime and conversion boundary

`ChannelLayout` continues to carry `ChannelTypeId` because lanes, persistence,
RPC, devices, and runtime-discovered nodes are dynamically shaped.  The ID is
derived from the canonical registry and maps back to a descriptor through the
closed registry.

Adding a type should require no incidental switches or parallel count/name
traits.  It must, however, require deliberate conversion definitions for every
supported semantic pair.  Registry-generated validation should make a missing
non-identity conversion a compile-time error rather than inventing a channel
order or reduction.
