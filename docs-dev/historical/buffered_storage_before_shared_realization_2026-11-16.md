# Recording RAM-only wording before shared buffered realization

_Status: historical statement displaced by the 2026-11-16 shared buffered
storage decision. Source: `docs-dev/realtime_background_execution_and_queues.md`,
§13, preceding its update. For the normative replacement, see
[Output Capabilities, Buffer Access, And Background Demand](../output_capabilities_and_background_demand.md), §8._

Earlier wording (verbatim):

> Recording is RAM-backed, overwrite-at-addressed-position semantics. It does not append
> or offset subsequent data and has no generic recorder arm/start/stop policy.

The new contract preserves addressed overwrite and absence/void semantics but
uses one background-managed buffered-storage mechanism for recordings and
Tock/buffered outputs, with optional bounded Linux virtual mappings. It does
not require all authoritative recorded data to remain in one RAM mapping.

Earlier backend-selection wording (verbatim from
`docs-dev/random_access_port_data_direction.md`, §9):

> The executor can choose sparse allocations, ring buffers, file backing,
> `mmap`, copy-on-write, or recomputation. A Tock-produced full-circle
> `computed` Region requires no full-circle allocation; a Sequential consumer
> requires only prepared rolling windows. A `buffered` source can similarly
> remain partially uncomputed and discard recoverable cached data when unused.

The replacement specifies a shared Tick/Tock buffered-storage implementation
contract and configurable per-view/aggregate limits without making `mmap()`
mandatory or altering demand-scoped evaluation.
