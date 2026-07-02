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

## The C++ must be ultra-clean (READ THIS BEFORE WRITING CODE)

Every line of C++ in this project follows one strict style. The full rules live in
[cpp-style-guide.md](cpp-style-guide.md) — read it **in full** before writing or
modifying any `.hpp`/`.cpp`. It is the C++ equivalent of the documentation style
guide: authoritative, not advisory, and **not** to be improvised around.

**The standard, in one line:**

> **Ultra-clean, no spaghetti.** Small single-purpose functions, shallow control
> flow (early-return guard clauses, nesting ≤ ~3), const-correct, `constexpr` over
> macros, no magic numbers, no raw owning pointers, no `using namespace` in headers,
> and **zero heap allocation / I/O / locks on the per-node search path.** Match the
> existing conventions exactly: `#pragma once`, `namespace engine { … } // namespace`,
> anonymous-namespace internal helpers, PascalCase types, camelCase functions/locals,
> trailing-underscore members (`nodes_`), `SCREAMING_SNAKE` `constexpr` constants,
> 4-space K&R, West const, `static_cast` (never C casts).

**How it is enforced (three layers, same philosophy as the docs rule):**

- **Always-loaded rules.** This `CLAUDE.md` section (Claude Code) and
  [`.agents/rules/cpp-style.md`](.agents/rules/cpp-style.md) (`trigger: always_on`,
  for Cursor/Windsurf) inject the rule into every session.
- **Machine-enforced formatting.** [`.clang-format`](.clang-format) at the repo root
  defines the layout objectively. Run it on every file you touch before finishing:
  ```bash
  clang-format -i src/<file>.hpp src/<file>.cpp
  ```
- **Static analysis.** A curated [`.clang-tidy`](.clang-tidy) flags bug-proneness,
  performance traps, and Core-Guidelines issues (advisory; run `clang-tidy -p build
  src/*.cpp`).
- **An automated gate blocks you, like the docs one does.**
  [`scripts/check_cpp_style.py`](scripts/check_cpp_style.py) is wired into a
  **PostToolUse** nudge and a **Stop** hook that *blocks the turn from ending* on the
  unambiguous, clang-format-can't-fix violations (tab indentation, a header missing
  `#pragma once`, `using namespace` in a header). If it blocks, fix it — don't work
  around it. Check anytime: `python scripts/check_cpp_style.py --check`.
- **The design rules are on you.** No tool checks naming, single-responsibility,
  ownership, or hot-path allocation. Self-review against the guide's pre-finish
  checklist ([Appendix B](cpp-style-guide.md#appendix-b--pre-finish-checklist)) before
  ending a turn. When a rule seems wrong for a case, **ask the user** — do not invent
  an exception.

Remember: a C++ change is not done until (1) it is clean per
[cpp-style-guide.md](cpp-style-guide.md), **and** (2) its `.md` companion is updated in
the same turn (the Stop hook checks the second; you own the first).
