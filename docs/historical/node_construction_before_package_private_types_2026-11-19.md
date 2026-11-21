# Node construction before package-private types

Archived on 2026-11-19 from [Node Definitions and Instances](../node_definitions_and_instances_direction.md)
and the [Node Ref DSL Contract](../node_ref_dsl_contract.md). The owner requested
type construction for private leaves and modules. Published definitions still
resolve by their registered IDs.

The previous DSL exposed `g.node<"stable.id">(...)` and
`g.node<"stable.id", ChannelType>(...)` to authors. Typed and tiled references
were implementation tools for providers, finalization, and lowering. Earlier
`g.node<Node, ChannelType>()` calls had been replaced by registered-ID calls.

The proposal reserved `IV_NODE` and `IV_MODULE` for public registration and
suggested `g.node<Type>(...)` for package-internal concrete leaves. It left
private module construction open between ordinary C++ functions or classes and
an addressable private definition. Removing registration was understood to
remove public lookup without imposing cross-package C++ access control.
