# GCC reflection prototype

This standalone CMake project probes the C++26 reflection implementation in
the GCC version pinned by the repository's Nix development shell. It does not
change the compiler used by the main Intravenous build.

```sh
nix develop
cmake -S experiments -B build-reflection -G Ninja \
  -DCMAKE_CXX_COMPILER="$IV_REFLECTION_CXX" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-reflection -j16
ctest --test-dir build-reflection --output-on-failure -j16
```

Observed with GCC 16.2.0, `-std=c++26`, and `-freflection`:

- `members_of`, member identifiers, types, offsets, sizes, and alignments work.
- `reflect_constant` and expression splicing preserve an exact structural
  node configuration value.
- A transient `std::vector` can be promoted with `define_static_array` and the
  resulting span can escape constant evaluation.
- `define_aggregate` can generate a layout from a transient vector of member
  specifications.
- Expansion statements over the promoted node sequence become straight-line
  code. The release build of `expanded_tick(float)` contains two scalar
  multiplies and a return, with no loop or indirect dispatch.
- GCC 16.2 rejects `reflect_constant` for a non-structural class even when it
  is a literal, copyable type. The negative test verifies the compiler's exact
  structural-type diagnostic.
- Parenthesizing a type reflection, for example `(^^int)`, avoids a GCC parser
  ambiguity when it is immediately followed by `&&`.

The main project also configures and builds with GCC 16.2 after qualifying one
dependent-base member access in `TypedNodeRef`. Its current hot-module rewrite
path does not yet work under a globally selected GCC compiler: the Clang
rewriter expects to expand a Clang driver command into `-cc1` arguments. Keep
the main project on Clang and use `IV_REFLECTION_CXX` for reflection compilation
until that compiler boundary is made explicit in the module build pipeline.
