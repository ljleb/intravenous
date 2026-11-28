# Authoring APIs

A project presentation draws a lane. A native node processes samples or
events. Both can live in one package.

1. **Tutorials** · [Change a C++ presentation](tutorials/presentation_cpp.md),
   [send a node event](tutorials/presentation_event.md),
   [write a C presentation](tutorials/presentation_c.md), and
   [build a native LLVM node](tutorials/llvm_node.md).
2. **Explanation** · [Presentation ownership and observations](explanation/presentation_execution.md)
   and [native package compilation and execution](explanation/llvm_execution.md).
3. **How-to guides** · [Build, reload, and test artifacts](how-to/presentation_packages.md)
   and [build and diagnose native nodes](how-to/llvm_packages.md).
4. **Reference** · [Presentation C ABI](reference/presentation_c_abi.md),
   [C++ presentation SDK](reference/presentation_cpp.md), and
   [native node API and LLVM package ABI](reference/llvm_nodes.md).

The APIs and their documentation evolve together. Use the
[documentation check](how-to/validate_docs.md) when editing an example or
an API contract. These guides follow [Diátaxis](https://diataxis.fr/).
