# Front 13 backend TypeScript generation before the language-neutral protocol

**Status:** historical, non-normative.

**Provenance:** this file preserves the Front 13 wording in
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md) and
[`node_interaction_and_presentation_communication_direction.md`](../node_interaction_and_presentation_communication_direction.md)
immediately before the 2026-11-03 decision that the backend publishes a
language-neutral semantic event protocol. The replacement design is in those two
normative documents.

The superseded Front 13 ownership list included:

> - generated event serializers/deserializers and frontend type metadata;

The superseded Front 13 status ended with:

> This makes both leaf and class-module handler sets package metadata. The remaining
> frontend-schema work is limited to ownership, update lifetime, output location, Vue
> import semantics, and the exact strongly typed event-construction API. Do not expose
> the raw nominal-ID/fingerprint/JSON transport as the Vue programming model.

The superseded frontend-generation direction was:

> ## Events are strongly typed in TypeScript
>
> Generated frontend event APIs must not degrade to a string event name plus an
> untyped/raw JSON object such as:
>
> ```ts
> // not the intended API
> node.send("SetCutoff", { cutoff: 800 })
> ```
>
> The source-introspection layer should generate concrete strongly typed TypeScript
> event values/types from the authoritative C++ event definitions. `node.send(...)`
> accepts those typed values (the exact generated construction syntax remains open).
> For example, possible generated ergonomics could look like:
>
> ```ts
> const event: SetCutoff = { cutoff: 800 }
> node.send(event)
> ```
>
> or an equivalently strongly typed generated constructor/API.
>
> JSON is a transport representation, not the programming model. JSON conversion
> functions/codecs for each generated event type should be generated automatically
> from the same authoritative C++ schema. Vue authors should neither select event
> types with raw strings nor manually serialize/deserialize their payload objects.
>
> The same principle applies in the reverse direction to generated presentation
> schemas.
>
> ## TypeScript generation comes from authoritative C++ definitions
>
> The mandatory source-introspection/compiler layer should generate the frontend
> protocol artifacts rather than requiring parallel hand-maintained C++ and
> TypeScript schemas.
>
> The generated surface is expected to cover, where relevant:
>
> - concrete strongly typed event/message types;
> - generated JSON encode/decode functions or equivalent codecs for those types;
> - `NodeAddress` and its codec;
> - node `PresentationState` types and codecs;
> - static information describing which event types a statically known node type
>   supports;
> - future live-subscription schema/capability types once that model is settled.
>
> Generated frontend protocol code must not expose backend ABI information such as
> state-field offsets, packed `NodeStorage` layout, concrete node indices, migration
> metadata, or compiled realization details.
>
> The source-introspection layer may know both sets of facts, but backend ABI metadata
> and frontend semantic protocol metadata are different generated artifacts.

The superseded unresolved list included:

> - exact generated TypeScript construction syntax for event values, beyond the hard
>   requirement that it remain strongly typed rather than string/raw-JSON based;
> - whether frontend event artifacts are emitted at package build time, derived from an
>   active `ProjectGraphGeneration`, or combine package-generated type libraries with a
>   runtime supported-event catalog;
> - ownership, update lifetime, output location, and Vue import semantics for those
>   generated frontend artifacts;

The replacement keeps authoritative C++ discovery but moves language-specific binding
generation out of the backend. Finalized packages publish neutral event schemas, and the
runtime publishes generation-specific `NodeAddress` support. A TypeScript/Vue adapter,
a future C++/Vulkan frontend, or supplementary plugin tooling may independently generate
its own types, codecs, factories, and UI integration from that same protocol.
