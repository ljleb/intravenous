# Repository Collaboration Rules

Before changing architecture, implementation, tests, or design documentation, read
[`docs/development_workflow.md`](docs/development_workflow.md). It is the governing
process for collaborative work in this repository.

In particular:

- normative design documents describe the intended destination; source code describes
  the current implementation; neither may be silently rewritten to agree with the
  other;
- implement one dependency-ready feature as a coherent whole rather than preserving a
  compiling repository through small compatibility/scaffolding passes;
- temporary compiler errors and incomplete intermediate source are acceptable while a
  feature is being implemented;
- treat surprising time, memory, or dependency complexity as implementation friction;
  stop and report it before encoding a narrow workaround or revising the architecture;
- never present an implementation compromise as original design intent;
- documentation changes preserve information: semantic text that is removed or
  superseded must first be retained under `docs/historical/` with provenance and a link
  to its replacement; and
- write implementation tests after the feature's actual shape is known, then refactor
  the working bottom-up implementation without changing its semantics.

