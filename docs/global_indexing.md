# Circular Global Timeline Indexing

_Status: normative design for integer global coordinates, finite index ranges, and
positional coverage._

This document supersedes the zero-origin, saturating, begin/end timeline arithmetic
preserved in
[Historical Linear And Saturating Global Index Semantics](./historical/linear_global_index_semantics.md).

## Global coordinates

`GlobalIndex` is the integer coordinate of samples and events in the global DSP
timeline. It stores exactly one `uint64_t`. Advancement and retreat use unsigned
modulo-2^64 arithmetic, so the successor of bit pattern `UINT64_MAX` is bit pattern
zero.

The same bits have an explicit two's-complement `int64_t` interpretation. This lets a
clip place data before signed zero—for example, a filter impulse response may begin at
a negative coordinate to express required latency—without reducing the available
64-bit coordinate domain or introducing a separate negative-timeline representation.

The type exposes named construction and observation:

```cpp
GlobalIndex::from_bits(std::uint64_t)
GlobalIndex::from_signed(std::int64_t)
index.bits()
index.signed_value()
static_cast<std::int64_t>(index)
```

The signed conversion is explicit. `GlobalIndex` deliberately has no relational
operators and no implicit integer conversion. There is no context-free total ordering
on a circular domain, and expressions such as `a < b`, `a + offset`, or
`std::min(a, b)` must not silently select signed or unsigned linear semantics.

Modular movement and distance are operations of the type:

```cpp
index.advanced_by(extent)
index.retreated_by(extent)
index.forward_distance_to(other)
index.signed_distance_to(other)
```

`signed_distance_to()` is only meaningful when the caller's known finite horizon fits
inside `int64_t`. It is useful for classifying an event as before or after the origin
of a bounded callback window; it does not define global ordering.

Buffer offsets, frame counts, storage offsets, compiler execution positions, queue
cursors, capture sequences, generation numbers, and other non-timeline integers remain
ordinary linear integer types.

## Fractional global coordinates

`FractionalGlobalIndex` extends the same circular global domain with 64 binary
fractional bits. It is the Q64.64 coordinate used to address multirate execution and
continuous storage, and it may also be transported as ordinary graph data whose value
names a coordinate. Those are contextual roles of one number rather than distinct
coordinate concepts.

A resolved absolute pace exponent `+k` selects a lattice using `k` fractional bits; a
negative exponent selects an aligned subset of integer project positions. The initial
realizable exponent range is `[-64, +64]`, and graph compilation rejects a revision
outside it. Port pace constrains which `FractionalGlobalIndex` values belong to a
particular execution/storage lattice; it does not define another index type.

## Finite ranges

`GlobalIndexRange` is the half-open modular range:

```text
[begin, begin advanced by extent)
```

It stores `begin` plus a `uint64_t extent`, never a begin/end pair. A range may cross
bit-pattern zero without becoming invalid or requiring a split. `extent == 0` is the
only empty representation; equality of `begin` and `end()` does not determine
emptiness.

Containment and offsets are range-relative:

```cpp
range.contains(index)
range.offset_of(index)
```

Tick history/current-block/latency windows use this representation. For callback
origin `B`, history `H`, block size `N`, and latency `L`, the legal window is:

```text
begin  = B retreated by H
extent = H + N + L
```

It is not clamped at signed or unsigned zero.

## Explicit orderings

Algorithms that require a linear order select one explicitly:

- `SignedGlobalIndexOrder` orders coordinates by their two's-complement signed value.
  Persistence directories, human presentation, and canonical positional coverage use
  this order.
- `RangeRelativeIndexOrder` orders coordinates by forward distance from a stated
  origin. Event streams and work inside one finite callback/query range use this
  order.

Persisted binary data stores the raw 64 bits. Human-readable and JSON surfaces use
signed decimal strings unless a schema explicitly calls for raw bits; strings avoid
both ambiguity and JSON-number precision loss above 2^53.

## Positional coverage and sample coverage

`IndexCoverage` is canonical set algebra over `GlobalIndexRange`. It represents exact
existence, requests, changes, invalidation, and event availability. It removes overlap
and multiplicity internally and uses signed-canonical order only as a stable
representation choice. A modular input range may be split at the signed-order seam
without changing its set meaning.

Its canonical ranges are held in an immutable path-copied interval tree. Copying an
`IndexCoverage` shares that tree; including or excluding a finite range replaces only
the tree paths and canonical ranges touched by the operation. Ordered iteration uses a
bounded non-allocating iterator. Persisted roots, background transactions, and node
coverage views therefore use the same positional algebra without flattening a
persistent store-specific representation into a vector.

Data-bearing random-access samples use the distinct `SampleCoverage` algebra described
by [Random-Access Port Data Direction](./random_access_port_data_direction.md).
`SampleCoverage` preserves its `SampleRegion` objects, their contiguous backing,
overlap, multiplicity, and optional ordering contract. It must not be normalized as a
positional set merely because both concepts use the word coverage.

The integer indexing vocabulary is therefore:

```text
GlobalIndex
GlobalIndexRange
FractionalGlobalIndex
FractionalGlobalIndexRange
IndexCoverage
SampleRegion
SampleCoverage
```

`FractionalGlobalIndex` may itself locate execution/storage or be transported as a
continuous port value. When transported, its value may name a coordinate other than
the `FractionalGlobalIndex` position at which it is stored.

## Lowering, persistence, and reload

GraphJit lowers `GlobalIndex` as an i64 bit pattern. Index add/subtract operations are
wrapping operations and must not carry LLVM `nuw` or `nsw` promises. Generated and
runtime ABIs preserve the exact bits.

Event chunks are indexed and ordered relative to an explicit chunk/query origin.
Fixed-width sample pages are not part of the target sample representation; sample
persistence migrates directly to `SampleRegion`/`SampleCoverage` rather than inventing
wrap-aware page-number arithmetic.

Generation cutover and state migration describe surviving timeline data relative to a
cutover origin or by modular ranges. Reload logic must not classify inherited data by
raw unsigned comparison against zero or the cutover coordinate.
