# ChessAIv2 — Agent Instructions

A high-performance UCI chess engine in C++20 on the Disservin `chess-library`.
The component design, options, and staged HCE→NNUE roadmap live in
[PLAN.md](PLAN.md); build/run instructions in [README.md](README.md).

## How to read this codebase (docs first)

Every C++ **module** (a header + its optional same-stem source) has one colocated
`.md` companion, kept strictly in sync with the code (see enforcement below). Use it:

- **To learn what a module/function/type _does_** — its purpose, inputs, outputs,
  how it fits the engine — **read the `.md` companion first** (e.g. read
  `src/search.hpp.md`, not `src/search.cpp`). It is faster and authoritative for
  behavior, and one companion covers both the header interface and the source
  implementation. This answers most questions: "what does X do?", "where is Y
  handled?", "how do these pieces connect?".
- **Only read the actual source when you need to know how it _works_** — writing or
  modifying code, fixing a bug, reasoning about exact implementation (bit tricks,
  pruning gates, SIMD), or verifying the docs match the code. When you compare docs
  against code and find a mismatch, fix the docs as part of your change.

Orient from [documentation-style-guide.md](documentation-style-guide.md) and
[PLAN.md](PLAN.md) before diving into source. The vendored library
`include/chess.hpp` is third-party — do not document it.

## Documentation is not optional (READ THIS FIRST)

This project keeps a **colocated `.md` companion for every C++ module** and a strict
documentation style. The full rules live in
[documentation-style-guide.md](documentation-style-guide.md) — read it **in full**
before writing or updating any documentation.

**The one rule you must never break:**

> **Every code change requires a matching documentation change, in the same turn.**
> If you create a module, create its `.md` companion. If you edit a header or source
> file, update its companion. If you delete or rename a module, delete or rename its
> companion. A C++ header and its source share **one** header-keyed companion
> (`eval.hpp` + `eval.cpp` → `eval.hpp.md`); source-only and header-only files key
> off their own name (`main.cpp` → `main.cpp.md`, `types.hpp` → `types.hpp.md`).
> Build files (`CMakeLists.txt`, `Makefile`) map to the single [build.md](build.md).
> See the style guide for the exact placement and structure per file type.

Do **not** wait to be reminded. Do **not** batch docs "for later." Do **not**
improvise your own format or create exceptions. If a situation genuinely is not
covered by the style guide, **ask the user** rather than guessing.

### How this is enforced (so you cannot forget)

Two automated hooks back this rule up — the harness runs them, not you:

- **PostToolUse hook** nudges you the moment you edit a code file whose companion
  you have not yet updated.
- **Stop hook** runs [`scripts/check_docs_sync.py`](scripts/check_docs_sync.py) and
  **blocks you from ending the turn** while any code change lacks a matching doc
  change. You will be told exactly which files are out of sync.

If the Stop hook blocks you, the correct response is to update the documentation —
not to work around the check. You can run the same check yourself at any time:

```bash
python scripts/check_docs_sync.py --check
```

The always-on documentation rule also lives in
[`.agents/rules/documentation.md`](.agents/rules/documentation.md) for editors that
read that convention (Cursor/Windsurf); this `CLAUDE.md` is the equivalent for
Claude Code.
