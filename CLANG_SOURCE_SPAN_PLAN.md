# Clang Source Span Plan

## Goal

Remove `NODE(...)` and all `std::source_location` usage. Replace them with a Clang source rewrite that wraps span-attributable ref expressions with a helper that records source spans in the builder.

`EventNodeRef` is out of scope for this pass.

## Cleanup First

1. Remove `NODE(...)`.
2. Remove all `std::source_location` parameters and plumbing.
3. Rewrite call sites back to plain `g.node<T>(...)`.

The Clang pass should work on ordinary C++, not on macro-expanded DSL syntax.

## Recording API

Add a value-returning helper:

```cpp
namespace iv {
    template<class Ref>
    Ref _add_node_source_span(Ref ref, uint32_t begin, uint32_t end);
}
```

`begin` and `end` are file-relative half-open offsets: `[begin, end)`.

Internally this forwards to builder storage/dedup and returns the same ref unchanged.

## What Gets Rewritten

Rewrite any expression whose resolved type is span-attributable:

- `SampleNodeRef`
- `SamplePortRef`
- `TypedNodeRef<...>`
- `StructuredNodeRef<...>`

Main rule:

```cpp
EXPR
```

becomes:

```cpp
iv::_add_node_source_span(EXPR, begin, end)
```

This should include:

- `g.node<T>(...)`
- `node.detach()`
- aliases like `osc`
- helper returns like `context.target_factory().sink(...)`
- `g.embed_subgraph(...)`
- any other expression semantically typed as one of the ref types above

## Extra Wrap For Named Bindings

For lvalue definitions and assignments, wrap the RHS one extra time using the bound identifier token span.

Example:

```cpp
auto osc = g.node<SawOscillator>();
```

becomes:

```cpp
auto osc =
    iv::_add_node_source_span(
        iv::_add_node_source_span(g.node<SawOscillator>(), expr_begin, expr_end),
        osc_begin, osc_end
    );
```

Likewise for:

```cpp
alias = osc;
```

The inner wrap attributes the source expression. The outer wrap attributes the named variable token.

## Span Extraction

For any `Expr* E`:

1. `beginLoc = SM.getSpellingLoc(E->getBeginLoc())`
2. `endLoc = Lexer::getLocForEndOfToken(E->getEndLoc(), 0, SM, LangOpts)`
3. `begin = SM.getFileOffset(beginLoc)`
4. `end = SM.getFileOffset(SM.getSpellingLoc(endLoc))`

This uses the expression's contiguous spelling range. Multi-line expressions are handled naturally by the single `[begin, end)` span.

## Pass Scope

Only rewrite user-authored sources. Skip:

- files under `intravenous/`
- macro expansions
- system headers

Ignore for now:

- `EventNodeRef`
- parameters
- returns
- members
- globals

Locals and assignments are enough for the first pass.

## Tooling Default

Build the Clang rewriter by default. It should be opt-out from CMake via:

```cmake
-DIV_BUILD_CLANG_SOURCE_SPAN_TOOL=OFF
```

When the rewriter target is enabled, LLVM/Clang is a hard dependency:

- use a manual install if `LLVM_DIR` / `Clang_DIR` or `CMAKE_PREFIX_PATH` already resolves it
- otherwise the repo bootstraps a pinned official LLVM archive automatically

## Generated Source Policy

The rewriter should not modify checked-in module sources.

- rewrite module sources into the module build directory
- compile the rewritten copies instead of the originals
- keep the original source tree as the single source of truth

## Implementation Order

1. Remove macro and `source_location`.
2. Add builder span storage/dedup plus `iv::_add_node_source_span(...)`.
3. Implement the Clang AST rewriter for span-attributable ref expressions.
4. Add the extra outer wrap for named bindings and assignments.
5. Run the pass only on user module code and inspect the diff.

## Intent

Keep the pass syntactic and local:

- no CFG work
- no block guards
- no alias analysis
- no attempt to understand implicit/internal helper-created nodes

The builder can deduplicate repeated `(node, span)` records.
