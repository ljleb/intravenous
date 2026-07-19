# IV module build profiling

Runtime-module diagnostics are always enabled and use two separate paths:

```text
build/iv_runtime_modules/<module-id>_<path-hash>/<Debug|Release>/build.trace.log
```

`build.trace.log` is the debug log. It records every command and its output,
step wall times, Clang `-ftime-report` totals, and the module pipeline time.
Clang `-ftime-trace` JSON files are emitted beside the relevant object files in
the same build workspace; they identify parsing, template-instantiation,
optimization, and code-generation work and can be opened in
`chrome://tracing` or a compatible trace viewer.

The normal application log remains separate. It receives only concise
`module build started` and `module build completed` messages; it does not
receive compiler commands or profiling output.
