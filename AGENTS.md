# Agent Instructions

Read [README.md](README.md) first for the project overview.

Read [DEVELOPERS.md](DEVELOPERS.md) before making code, build, test, driver,
or local runtime changes in this repository. It is the canonical developer
runbook for this project.

## Code documentation conventions

- Use `/** ... */` block comments for documentation that is meant to be
  extracted (file headers, and docstrings for types, functions/methods,
  constants and member variables). Do **not** use `///` for these.
- Use plain `//` comments for explanatory notes *inside* function bodies that
  label logical groups of statements. Prefix each such group comment with an
  empty line.
- Document the logical blocks of code inside a function body: for each block of
  statements, add a `//` comment (prefixed with an empty line, per the rule
  above) explaining in plain language what that block does. This applies even
  when the function already has a docstring — the docstring covers the contract,
  the block comments narrate the implementation.
- When documenting an implementation (`.cpp`/`.c`) file, do not repeat a
  docstring for a function that is already documented in its header; still add
  the file header and document file-local (e.g. anonymous-namespace / `static`)
  helpers that have no header declaration.
- Every function/method must have a docstring that documents **all** of its
  parameters and its return value:
  - Document each parameter with `@param <name> <description>`.
  - Document the return value with `@return <description>`. For functions
    returning `void`, omit `@return`.
  - A parameter that exists only to satisfy an interface/signature and is
    unused should still be mentioned (e.g. note that it is ignored).
