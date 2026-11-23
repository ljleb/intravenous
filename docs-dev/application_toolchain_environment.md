# Application-owned compilation environment

Accepted direction, 2026-11-14. This document supplements the project toolchain
settings contract. The application needs the compilation environment associated
with its own build, including the environment consumed by Nix compiler wrappers.
A project's `.envrc` or current flake does not select that environment.

## Ownership and portability

The application build records its compilation environment as an installation
artifact. Runtime startup, package compiler subprocesses, and editor tooling use
that artifact. It is local to an application installation and is never written
into the shared project file. Runtime session information, audio-device access,
and user credentials remain part of the launching environment.

Project toolchain settings express explicit overrides. An unset field uses the
current installation's default. Installation defaults must not become explicit
project settings merely because a project is saved. Thus the same project can use
different local installation paths on different computers.

Compiler overrides must satisfy the actual package format. The current host
requires Clang and LLVM 23. IV packages require Clang-generated LLVM bitcode,
the application's Clang metadata plugin, its DSL PCH, and compatible C++ ABI.
GCC is not an IV-package C++ compiler. An arbitrary different Clang version is
not promised to work with a PCH and plugin from this installation. Compatibility
must be checked before package builds, with a useful error. A PCH override may
relocate an unchanged copy of this application build's PCH; it cannot substitute
a different DSL or C++ ABI. Alternative compatible
compiler paths remain useful; CMake, generators, build tools, and dependency
locations remain configurable within the application toolchain contract.

## Implementation sequence

1. Capture the application build's compiler environment without capturing the
   user's whole environment. Make the recorded environment available to direct
   server startup and child tooling, including editor integration.
2. Enforce compiler/PCH/plugin compatibility at the package-toolchain boundary.
   Check once for an effective toolchain, rather than once per graph edit.
3. Keep installation defaults separate from explicit project overrides and
   preserve project-history semantics when overrides are set or cleared.
4. Add focused regressions for environment restoration, incompatible tooling,
   clearing overrides, and saving a project without local default paths.

Builds and test execution remain with the owner. This is an implementation plan,
not a claim that the mechanisms above have landed.

## Source implementation status

The source changes above are ready for owner verification. The agent has not run
builds or tests.

`ApplicationEnvironment.cmake` records selected compiler and build-tool variables
in `iv-toolchain-environment` beside the server executable. The first configure
captures them. Later automatic regeneration retains them. Changing the selected
compiler recaptures them; deliberate recapture with the same compiler uses
`-DIV_REFRESH_APPLICATION_ENVIRONMENT=ON` from the desired application build
shell. The option resets after that configure. This does not evaluate a project's
flake or `.envrc`.

CMake compiler/linker launchers, the DSL PCH command, standalone server startup,
and ModuleLoader's build subprocesses activate this environment. Server startup
re-execs before creating threads and preserves the inherited RPC descriptor.
Unrelated runtime variables, including audio-session variables, remain inherited.
The package cache signature includes the environment artifact identity.

C++ compiler and PCH overrides are checked against the actual application PCH
and metadata plugin in a separate compiler process. Successful checks are cached
by compiler/artifact identities. Package CMake requires the host's current LLVM
major, 23. This allows compatible alternate compiler paths without promising
arbitrary compiler versions or GCC support. The C compiler override is independent
of the C++ compiler override; dependency artifacts must still satisfy their
existing native-library or LLVM-bitcode contracts.

Persistence already omits effective settings equal to startup defaults. That
behavior is retained and its regression now includes installation paths. Clearing
an override restores the installation's selected defaults, rather than relying
only on ModuleLoader's compiled fallback paths. Explicit project overrides remain
undoable and persistent.

The generated `intravenous-clangd` launcher uses the same environment for editor
tooling. The separate clangd extension owns its process and executable selection;
users can select this launcher through their local `clangd.path` setting (and
`clangd.useScriptAsExecutable` where required). Intravenous does not write local
installation paths into shared project settings or change global editor settings.
Its server activation is automatic from either the extension or the CLI.

Regressions cover environment restoration and removal of foreign compiler flags,
literal shell characters, preservation of runtime variables and fd 3, automatic
regeneration versus explicit recapture, compatible and incompatible C++ overrides,
clearing overrides, and omission of installation defaults from project saves.
