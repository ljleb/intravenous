# Restructure Notes

- `dsl.h` is the module DSL surface. It should reject inclusion from internal TUs via `IV_INTERNAL_TRANSLATION_UNIT`.
- `module/module.h` must not import `dsl.h`. Intended layering: graph core -> `module/module.h` -> `dsl.h`.
- Module-authored code should include `dsl.h` explicitly. Internal/runtime code should include graph or module headers directly.

## Graph Pipeline

- `graph_builder.h`: authoring-time graph construction (`GraphBuilder`, refs, `embed_subgraph`, public I/O, `build()` entrypoint).
- `graph_build.h`: graph lowering/compiler pass (hyperedge expansion, validation, execution planning, dormancy compilation, artifact build).
- `graph_node.h`: executable graph node assembled from the compiled artifact.

## Target Tree

```text
intravenous/
  dsl.h

  compat.h
  sample.h
  ports.h
  block_rate_buffer.h
  fast_bitset.h
  note_number_lookup_table.h
  wav.h

  graph/
    builder.h
    compiler.h
    node.h
    build_types.h
    types.h
    wiring.h
    runtime.h
    node_wrapper.h
    scc_wrapper.h
    port_data_node.h
    event_port_data_node.h

  node/
    executor.h
    layout.h
    lifecycle.h
    resources.h
    tick.h
    traits.h

  juce/
    midi_input.h
    vst_wrapper.h
    vst_runtime.h
    vst_runtime.cpp

  module/
  devices/
  orchestrator/
  basic_nodes/
  math/
  runtime/
  third_party/
```
