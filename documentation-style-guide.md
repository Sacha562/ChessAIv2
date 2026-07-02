# ChessAIv2 Documentation Style Guide

> **Audience:** AI agents (and humans) working on the ChessAIv2 chess engine.
> **Purpose:** This document defines the exact documentation conventions for the project. Follow these rules precisely when reading, writing, or updating documentation. **Do not deviate. Do not make assumptions. Do not improvise.**
>
> **Lineage:** This guide is the C++ adaptation of the ChessCoach documentation style guide (a Python/TypeScript/SQL web app). The *philosophy* is identical — a colocated `.md` companion kept strictly in sync with the code. The *mechanics* differ because C++ splits a module across a header and a source file, has no SQL database or frontend, and is built with CMake/Make rather than Docker. Where this guide is silent, the ChessCoach original is **not** authoritative — **ask the user** instead of importing a Python-era rule.

---

## 0. Reading Protocol (docs first)

Every C++ **module** has one `.md` companion that is kept strictly in sync with the code, so the docs are the fast path for understanding:

- **To learn what code _does_** (purpose, inputs/outputs, side effects, how modules connect), **read the `.md` companion first** — e.g. read [`src/search.hpp.md`](src/search.hpp.md), not `src/search.cpp`. The companion is authoritative for behavior and documents the whole module (header interface **and** source implementation) in one place.
- **Read the actual source only when you need to know how it _works_** — when writing or modifying code, fixing bugs, reasoning about exact implementation details (bit tricks, SIMD, pruning gates), or verifying that the docs match the code.
- If you compare a doc against its code and find a mismatch, **fix the doc** as part of your change (per [Principle 4](#1-core-principles) and [Section 9](#9-enforcement-automated)).

Start from this guide and [`PLAN.md`](PLAN.md) (the component/roadmap design document) to orient before diving into source.

---

## 1. Core Principles

1. **Every C++ module has one colocated `.md` companion.** A *module* is a header (`.hpp`/`.h`/`.hxx`) plus its optional same-stem source (`.cpp`/`.cc`/`.cxx`) in the **same directory**. The companion is named after the module's **primary file** — the header if one exists, otherwise the source — with `.md` appended (see [Section 2](#2-file-placement-rules) for the exact rule and every case). Editing **either** the header or the source requires updating that one companion.
2. **Every `.md` file uses standard markdown links** — never Obsidian wikilinks. Example: `[eval module](eval.hpp.md)`, not `[[eval]]`.
3. **Cross-reference aggressively.** Whenever you mention another module, function, class, struct, enum, constant, or UCI command that has its own documentation, link to it. The goal is that a human or AI can navigate the entire documentation graph without searching.
4. **Documentation must stay in sync with code — every code change requires a matching documentation change, in the same turn.** When you create a module, create its `.md` companion. When you modify a header or source file, update its companion. When you delete or rename a module, delete or rename its companion. This is **not** deferrable and it is **not** optional — it is enforced automatically (see [Section 9](#9-enforcement-automated)). Never end a turn with code changes whose docs have not been updated. If a case genuinely is not covered here, **ask the user** instead of improvising.
5. **Be precise and explicit.** Do not write vague descriptions. State exactly what a function does, what its parameters mean (including `const`/reference/pointer/ownership semantics), what side effects it has, and when it should be used. This engine is performance-critical — where a function is on a hot path, say so.

---

## 2. File Placement Rules

### 2.1 The companion rule (header-keyed modules)

The companion `.md` file lives **next to** the code, and is named by appending `.md` to the module's **primary file**:

- **Primary file = the header** when the module has one. So a header + source pair shares **one** doc, named after the header:

```
src/
  eval.hpp
  eval.cpp
  eval.hpp.md          <- documents the WHOLE eval module (eval.hpp + eval.cpp)

  search.hpp
  search.cpp
  search.hpp.md        <- documents search.hpp + search.cpp

  uci.hpp
  uci.cpp
  uci.hpp.md           <- documents uci.hpp + uci.cpp
```

Editing `eval.hpp` **or** `eval.cpp` requires updating `eval.hpp.md`. There is no `eval.cpp.md`.

### 2.2 Source-only files (no header)

When a source file has **no** same-stem header (e.g. the entry point), the source **is** the primary file — append `.md` to it:

```
src/
  main.cpp
  main.cpp.md          <- documents main.cpp (there is no main.hpp)
```

### 2.3 Header-only files (no source)

When a header has **no** same-stem source (constants, templates, type aliases fully defined inline), the header is the primary file:

```
src/
  types.hpp
  types.hpp.md         <- documents types.hpp (there is no types.cpp)
```

### 2.4 Test Files

Test `.md` files follow the same header-keyed rule and live next to their test code. A test file usually has no header, so it is source-only:

```
tests/
  test_see.cpp
  test_see.cpp.md
```

### 2.5 Build & Toolchain (single file)

All build-system documentation lives in **one file** at the project root, analogous to how the ChessCoach guide used a single `infrastructure.md`:

```
build.md             <- covers CMakeLists.txt, Makefile, compiler flags, PGO, WSL2 build flow
```

This file documents [`CMakeLists.txt`](CMakeLists.txt), [`Makefile`](Makefile), the `-march=native` / `-DCHESS_USE_PEXT` / LTO / PGO flag strategy (see [`PLAN.md`](PLAN.md) §0.4), and any build/PGO scripts. Editing `CMakeLists.txt`, `Makefile`, or a `*.cmake` file requires updating `build.md`.

### 2.6 Vendored / third-party code (excluded)

The Disservin `chess-library` is vendored as a single header and is **not** first-party code. It gets **no** companion and is exempt from every rule in this guide:

```
include/
  chess.hpp            <- VENDORED. Do not document, do not require a companion.
```

Everything under `include/` is third-party. If you need to explain how the engine *uses* the library, do it in the consuming module's companion (e.g. how [`search.hpp.md`](src/search.hpp.md) uses `chess::Board`), not in a doc for `chess.hpp`.

### 2.7 Tooling scripts

Developer tooling scripts (e.g. the documentation sync checker, future Texel/SPSA tuners, NNUE data-gen helpers) are first-party code and **do** get colocated companions, per their own language's per-file convention:

```
scripts/
  check_docs_sync.py
  check_docs_sync.py.md
```

For Python and other non-C++ tooling, the companion is simply `<full filename>.md` (per-file, no header-keying — that only applies to C++ modules).

---

## 3. Link Format Rules

### 3.1 Always Use Standard Markdown Links

```markdown
<!-- CORRECT -->
See [uci.hpp.md](uci.hpp.md) for the command loop.
The search calls [`evaluate`](eval.hpp.md#evaluate) at leaf nodes.

<!-- INCORRECT — do NOT use wikilinks -->
See [[uci]] for the command loop.
```

### 3.2 Use Relative Paths

All links must be **relative** to the current `.md` file's location. Never use absolute paths.

```markdown
<!-- From src/search.hpp.md -->
Leaf evaluation is delegated to [`evaluate`](eval.hpp.md#evaluate).
Score and mate conventions come from [types.hpp.md](types.hpp.md).

<!-- From tests/test_see.cpp.md -->
This exercises the SEE routine documented in [../src/see.hpp.md](../src/see.hpp.md).
```

### 3.3 Link to Specific Sections with Anchors

When linking to a specific function, class, or section within a documentation file, use anchors:

```markdown
See [`Searcher::think`](search.hpp.md#searcherthink) for the iterative-deepening loop.
```

Anchor format: lowercase, hyphens replace spaces, strip special characters (`` ` ``, `:`, `()`, `<>`, `&`, `*`). This follows standard markdown anchor rules — see [Section 4.1](#overloaded-functions-and-anchor-collisions) for how to keep anchors unique across overloaded functions.

### 3.4 When to Link

Link **every time** you mention:
- Another documented module (its header or source)
- A class, struct, enum, union, concept, type alias, or named constant documented elsewhere
- A function in another module
- A UCI command or engine subcommand (`bench`, `perft`) documented in another module
- A test file
- A build concept documented in [`build.md`](build.md)

If something is mentioned more than once in the same section, link it at least on the **first mention** in that section.

---

## 4. Documentation File Structures

### 4.1 C++ Module Files

Every C++ module documentation `.md` file must contain the following sections, in this order. Omit a section only when it genuinely does not apply (noted per section).

````markdown
# Module: `eval`

<General description: 1-3 paragraphs explaining the purpose of this module,
what role it plays in the engine, and how it relates to other modules.
Include links to related modules. State where it sits on a search hot path.>

## Source Files

- **Header (interface):** [eval.hpp](eval.hpp)
- **Source (implementation):** [eval.cpp](eval.cpp)

<!-- Header-only modules list only the header; source-only files list only the source. -->

## Related Test Documentation

- [test_eval.cpp.md](tests/test_eval.cpp.md)

<!-- Omit if no tests cover this module yet. List every test file that does. -->

## How to Build & Run

<!-- Only include this section if the module produces or affects a runnable
     entry point or engine subcommand (e.g. main.cpp, the UCI loop, `bench`,
     `perft`). Omit for pure library modules like eval or types.
     Reference build.md for build details rather than duplicating them. -->

<Explain what command exercises this module and the expected behavior.
E.g. "`./chessai bench` invokes Bench::run; prints `<nodes> nodes <nps> nps`.">

## Namespace

- Public API declared in namespace `engine`.
- Internal-linkage helpers (e.g. `V_PAWN`, `diff`) live in an anonymous
  namespace in `eval.cpp` and are **not** part of the public interface.

<!-- Always state the enclosing namespace. Call out anonymous-namespace /
     `static` internal helpers so readers know what is public vs. private. -->

## Objects / Interfaces

<!-- Include this section if the module defines any class, struct, enum,
     enum class, union, concept, type alias (`using`/`typedef`), or named
     structured constant. Place BEFORE the Functions section. See rules below. -->

### `struct Limits`

<Description of what this type represents and when it is used.>

| Field | Type | Description |
|-------|------|-------------|
| `depth` | `int` | Fixed depth cap; `0` = no cap. |
| `movetime` | `int64_t` | Exact ms for this move; `0` = none. |

**Used by:** [`Searcher::think`](#searcherthink)

## Functions

### `evaluate`

<Description of what this function does.>

**When to use:** <Scenarios where this function should be called.>

**Template Parameters:** <Only if the function is a template.>
| Name | Kind | Description |
|------|------|-------------|
| `Color` | `enum` non-type | Side to evaluate for. |

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position to score. Not modified (read-only reference). |

**Returns:** <Exact type and meaning. E.g. "`Value` (centipawns) from the
side-to-move's perspective; positive = better for the side to move.">

**Side Effects:**
- <Database writes have no analog here — list mutation of out-params,
  global/TT state, I/O (`std::cout` for `info` lines), etc. "None (pure)"
  is a valid and useful answer.>

**Performance:**
- <Optional but encouraged for hot-path code. Note if this runs per-node in
  search, whether it is `constexpr`/`inline`, allocation behavior, etc.>

**Warnings:**
- <Caveats, gotchas, UB traps, negamax sign conventions, aliasing, etc.>

**Example:** (optional — include only when usage is non-obvious)
```cpp
Value v = engine::evaluate(board);
```

<!-- Repeat for each function in the module -->
````

**Rules for the Functions section:**
- Document **every** function that is exported/public (declared in the header), plus any internal (anonymous-namespace / `static`) helper that is meaningfully complex.
- For trivial one-liner helpers (e.g. `mate_in(int ply)`), a single-line description is sufficient — you don't need the full parameter table.
- Order functions in the same order they appear in the source (or header, for header-only modules).
- **Member functions:** document a class's methods in the Functions section using the same structure as free functions, with the heading prefixed by the class name: `### \`Searcher::think\``. Document the class itself (fields, purpose) in Objects / Interfaces.
- **Operator overloads:** document with the operator in the heading, e.g. `### \`operator==\` (Move, Move)`.
- **Parameter types must carry C++ semantics.** Write the real type as it appears in the signature: `const Board&`, `Movelist&` (out-param), `Move*`, `int64_t`. In the description, say whether a reference/pointer parameter is **in**, **out**, or **in/out**, and note ownership if a pointer is stored or freed.
- **Return values must be specific.** Do not write "Returns the score." State the exact type and, for aggregates, every field. If the return is a documented type (a struct/enum/alias), link to it. Note reference/pointer returns and their lifetime/ownership.
- **Cross-reference everything.** If a function accepts or returns a documented type, or calls a function in another module, link to it.

#### Documenting Objects, Classes, Interfaces, and Data Structures

Every C++ module doc must include an **Objects / Interfaces** section (placed **before** Functions) if the file defines any of:
- `class`, `struct`, `union`
- `enum`, `enum class`
- `concept` (C++20)
- Type aliases: `using X = ...;` or `typedef ... X;`
- Class/function **templates** whose *shape* (not just one instantiation) is part of the API
- Named structured constants / `constexpr` values that callers rely on (e.g. `VALUE_MATE`, `MAX_PLY`)

Use this structure:

````markdown
## Objects / Interfaces

### `class Searcher`

<Description: what it represents, its lifetime (one instance per search),
how it is driven (constructed with a shared stop flag, `think()` called once).>

| Field | Type | Description |
|-------|------|-------------|
| `stop_` | `std::atomic<bool>&` | Shared abort flag; the UCI thread sets it to stop the search. |
| `nodes_` | `uint64_t` | Nodes visited this search. |

**Methods:** [`Searcher::think`](#searcherthink), [`Searcher::nodes`](#searchernodes) (documented under Functions).

**Used by:** [uci.hpp.md](uci.hpp.md)

### `using Value = int`

Centipawn score type. See the mate-encoding constants below.

### Score constants (`types.hpp`)

| Constant | Value | Meaning |
|----------|-------|---------|
| `VALUE_MATE` | `32000` | Mate score at the root. |
| `VALUE_INFINITE` | `32001` | Aspiration/alpha-beta window bound. |
| `MAX_PLY` | `128` | Maximum search depth in plies. |
````

**Rules for the Objects / Interfaces section:**
- Document **every** exported/public class, struct, union, enum, concept, type alias, and named constant set.
- For `enum` / `enum class`, list every enumerator and what it means.
- For a `concept`, state the requirements it expresses and which templates are constrained by it.
- For a class **template** or function **template**, document the template parameters (name, kind — `typename`/`class`/non-type/`auto`/parameter pack — and constraints) in a table, in addition to the members.
- Every field/enumerator must have a meaningful description — not a restatement of its name. `"Nodes visited this search"` is good for `nodes_`; `"The nodes"` is not.
- If a field's type references another documented type, link to it.
- Cross-reference the functions that consume or produce the type from a **Used by** line.

#### Overloaded functions and anchor collisions

C++ allows several functions to share a name (overloading, plus template specializations). Markdown anchors are derived from headings, so identical headings collide. **Disambiguate every overload in its heading** by appending a short signature discriminator in parentheses:

```markdown
### `legalmoves` (all)
### `legalmoves` (captures)
### `makeMove` (Move)
### `makeMove` (null)
```

This yields distinct anchors (`#legalmoves-all`, `#legalmoves-captures`, …). When you link to a specific overload, link to the disambiguated anchor.

### 4.2 Test Files

Test documentation `.md` files describe **what is tested**, not signatures:

```markdown
# Test: `test_see.cpp`

<General description: what module/feature does this test cover? What strategy
(perft cross-check, known-answer positions, property test, fuzzing)?>

## Source File

- **Test Code:** [test_see.cpp](test_see.cpp)

## Related Code Documentation

- [../src/see.hpp.md](../src/see.hpp.md)

<!-- REVERSE link: the module doc links here under "Related Test Documentation",
     and this links back. -->

## How to Build & Run

<Exact command to build and run these tests, incl. prerequisites. See build.md.>

## Tests

### `see_winning_capture_is_positive`

<What is being tested? What position/edge case, and the expected outcome?>

<!-- Repeat for each test case -->
```

### 4.3 Build & Toolchain (`build.md`)

A **single file** at the project root covering the whole build:

```markdown
# Build & Toolchain

<Overview: C++20, Clang-primary release builds, single self-contained binary,
`-march=native` for this machine, WSL2 build flow. Point to PLAN.md §0 for the
rationale behind the toolchain choices.>

## Files Covered

| File | Purpose |
|------|---------|
| [CMakeLists.txt](CMakeLists.txt) | Primary build (IDE/CLion/VS Code); presets for Clang/GCC/MSVC. |
| [Makefile](Makefile) | Thin OpenBench-compatible build (`make EXE=<name>`). |

## Compiler & Flags

<`-O3 -march=native -flto -DCHESS_USE_PEXT`, PGO plan, MSVC equivalents. What
CHESS_USE_PEXT does and when to turn it off (older AMD). Warnings policy.>

## How to Build

### WSL2 / Ubuntu (canonical)

<Exact rsync + make/cmake commands, per README.>

### Native Windows / MSYS2

<Notes, incl. the Smart App Control caveat on running unsigned binaries.>

## How to Run

<`./chessai`, `./chessai bench [d]`, `./chessai perft test`.>

## Notes

<PGO workflow, ISA-variant builds for distribution, known issues.>
```

### 4.4 Config Files (optional, looser structure)

Most config in this project is build config and is covered by [`build.md`](build.md). If a **standalone** config file that is *not* a build file needs documentation (e.g. `.clang-format`, a future engine config), give it a colocated `<filename>.md` with a looser structure:

```markdown
# <Config File Name>

<What is this file for? When would someone edit it?>

## Source File

- **Config:** [filename](filename)

## Keys / Settings

### `KEY_NAME`

- **Type:** string / integer / boolean / ...
- **Default:** `value`
- **Description:** What this key controls.
- **Used by:** [module.hpp.md](path/to/module.hpp.md)

## Notes

<Warnings, machine-specific behavior.>
```

Never commit secret values into documentation (not that an engine has many — but the rule stands).

---

## 5. Naming Conventions

| Item | Convention | Example |
|------|-----------|---------|
| Doc for a header+source module | Header filename + `.md` | `eval.hpp.md` |
| Doc for a header-only module | Header filename + `.md` | `types.hpp.md` |
| Doc for a source-only file | Source filename + `.md` | `main.cpp.md` |
| Doc for a test file | Test filename + `.md` | `test_see.cpp.md` |
| Doc for a tooling script | Full filename + `.md` | `check_docs_sync.py.md` |
| Build / toolchain | Always `build.md` | `build.md` |
| Vendored library | (none — excluded) | `include/chess.hpp` |

**The one-line rule:** the companion is named after the module's **primary file** (header if present, else source) with `.md` appended. Header and source of the same module never get two docs.

---

## 6. Cross-Reference Checklist

When writing or updating any `.md` file, ensure these links exist:

### For Module Documentation:
- [ ] Link to the source header **and** source file
- [ ] Link to all related test documentation files
- [ ] Link to other modules whose functions this module calls
- [ ] Link to other modules that call functions in this module
- [ ] Link to every documented type (class/struct/enum/alias/constant) that functions accept or return
- [ ] Link to [`build.md`](build.md) if the module is runnable / affects the build
- [ ] Link to [`PLAN.md`](PLAN.md) for the component it implements, when useful

### For Test Documentation:
- [ ] Link to the test source file
- [ ] Link back to the module documentation being tested

### For Build Documentation (`build.md`):
- [ ] Link to every build file it covers (`CMakeLists.txt`, `Makefile`, scripts)
- [ ] Link to `PLAN.md` for the flag/toolchain rationale

---

## 7. Quality Standards

1. **No placeholder text.** Every section must contain real, accurate content. If information is unknown, investigate the source rather than writing "TODO" or "TBD".
2. **No stale documentation.** If the code changes, the docs must change. Period.
3. **Correct relative paths.** Test every link mentally. A link from `tests/test_see.cpp.md` to `src/see.hpp.md` must be `../src/see.hpp.md`.
4. **Consistent terminology.** Use the same term for the same concept everywhere. Prefer engine-standard vocabulary (see [`PLAN.md`](PLAN.md) and the Chess Programming Wiki):
   - "centipawn" / "cp" for scores; "Value" for the score type.
   - "ply" for a half-move (not "move" — a move is two plies).
   - "node" for a searched position; "leaf" for a horizon node.
   - "negamax" (side-relative), not "minimax".
   - "TT" / "transposition table" (not "cache" or "hash map").
   - "SEE" (Static Exchange Evaluation), "PVS", "NMP", "LMR", "RFP", "LMP", "NNUE" — spell out on first use in a doc, then abbreviate.
   - "make/unmake" (not "push/pop" or "do/undo").
   - "stand-pat" for the quiescence lower bound.
5. **Section anchors.** Use the function/class name as the heading (e.g. `### \`function_name\``, `### \`Class::method\``). Disambiguate overloads per [Section 4.1](#overloaded-functions-and-anchor-collisions) so anchors stay unique.
6. **Be specific, not vague.** Documentation must let someone unfamiliar with the code understand exactly what a function does, what it returns, and how it connects to the rest of the engine. Avoid "Returns the result."
7. **Return values must be fully specified.** For every function:
   - Primitive → state the type and what the value represents (units: centipawns? nodes? ms?).
   - Aggregate (struct/`std::pair`/tuple) → list every field with type and meaning, or link to the documented type.
   - Container (`Movelist`, `std::vector<...>`) → specify the element type and ordering guarantees.
   - Reference/pointer → state lifetime and ownership.
   - Different code paths returning different shapes → document each.
8. **Cross-reference aggressively.** Every mention of a documented entity is a markdown link on its first occurrence in each section.
9. **Tables must be properly formatted.** Every markdown table must have a header row, a separator row (`|---|---|`), consistent columns, leading/trailing pipes, and no broken separators. Escape literal pipes inside cells as `\|` (relevant for C++ `operator|`, `A | B`, bitboard expressions).
10. **Depth over brevity.** When in doubt, include more. Explain the algorithm (the swap loop in SEE, the pruning gates in LMR, the accumulator deltas in NNUE), document edge cases (zugzwang, mate-score TT storage, en-passant in SEE), and note non-obvious implementation details. For hot-path code, document the performance-relevant choices.

---

## 8. Procedures for Common Operations

### 8.1 Adding a New Module (header + source)

1. Create `foo.hpp` and `foo.cpp` in `src/`.
2. Create **one** companion `src/foo.hpp.md` following [Section 4.1](#41-c-module-files).
3. Add links **from** related module docs **to** the new companion (callers and callees).
4. If it wires into a runnable command, update the relevant doc (`main.cpp.md`, `uci.hpp.md`) and [`build.md`](build.md) if the build changes.

### 8.2 Adding a Source-Only or Header-Only File

1. Create the file.
2. Create its companion named after that file (`main.cpp.md`, `types.hpp.md`) per [Section 2.2](#22-source-only-files-no-header) / [2.3](#23-header-only-files-no-source).
3. Cross-link as in 8.1.

### 8.3 Adding a New Test File

1. Create the test file.
2. Create its `.md` companion following [Section 4.2](#42-test-files).
3. Update the module's doc to add a link under **Related Test Documentation** (the reverse link).

### 8.4 Modifying the Build

1. Change `CMakeLists.txt` / `Makefile` / build scripts.
2. Update [`build.md`](build.md) to match (flags, targets, commands).

### 8.5 Deleting a Module

Deleting code without cleaning up its docs is the most common way this system rots. When you delete a module:

1. Delete its `.md` companion in the same commit (an orphaned `.md` with no code is a violation). Remember: header + source share **one** doc — delete the doc only when **both** files are gone; if you delete just the `.cpp` but keep the `.hpp`, the doc stays (and is updated).
2. Remove **inbound links** to the deleted doc from any other `.md` files (search so you don't leave dangling links).

### 8.6 Renaming or Moving a Module

1. Rename/move the `.md` companion alongside the code so it stays colocated and keyed to the (new) primary file (`foo.hpp` → `foo.hpp.md`).
2. Update the `# Heading` and the **Source Files** links inside the doc to the new names.
3. Update all inbound links from other `.md` files to the new path.

---

## 9. Enforcement (Automated)

This style guide is not advisory. The rule "every code change requires a matching documentation change" is enforced by three layers so that keeping docs in sync does not depend on anyone (human or AI) remembering to do it.

### 9.1 Always-loaded rules

- **Claude Code** auto-loads [`CLAUDE.md`](CLAUDE.md) at the workspace root into every session. It states the no-code-change-without-a-doc-change rule and points here.
- **Cursor / Windsurf** (and other editors that read the convention) load [`.agents/rules/documentation.md`](.agents/rules/documentation.md), which is `trigger: always_on`.

Both point at this file as the single source of truth. If you edit the rule in one, mirror it in the other.

### 9.2 The sync checker

[`scripts/check_docs_sync.py`](scripts/check_docs_sync.py) (documented in [`scripts/check_docs_sync.py.md`](scripts/check_docs_sync.py.md)) is a dependency-free Python script that compares the git working tree against the placement rules in [Section 2](#2-file-placement-rules). It reports any changed code file whose companion was not changed with it, any missing companion, and any orphaned companion (code deleted, doc left behind). It understands the **header-keyed module rule**: editing `eval.hpp` *or* `eval.cpp` requires `eval.hpp.md`.

> **Why the checker is still Python in a C++ project:** it is developer tooling, not shipped code — it never links into the engine binary. A dependency-free Python script is the least-friction way to script git plumbing cross-platform (Windows + WSL2), and it matches the OpenBench-adjacent tooling ecosystem. It is itself first-party code, so it carries its own `.md` companion.

Run it any time:

```bash
python scripts/check_docs_sync.py --check
```

It exits non-zero when anything is out of sync, so it can also gate CI.

### 9.3 Claude Code hooks

[`.claude/settings.json`](.claude/settings.json) wires the checker into two harness-run hooks:

| Hook | When it runs | Effect |
|------|--------------|--------|
| **PostToolUse** (`Edit`/`Write`/`MultiEdit`) | Immediately after a code file is edited | Injects a reminder if that module's companion has not been updated yet. |
| **Stop** | When Claude tries to end its turn | **Blocks** the turn from ending while any code change lacks a matching doc change, listing exactly what to fix. |

The Stop hook is the hard guarantee: an agent cannot finish a turn with undocumented code changes. If the hook blocks, the correct response is to update the documentation per this guide — never to bypass the check.

> **Keeping the checker aligned:** the checker encodes the placement rules from Section 2 (which extensions need companions, the header-keyed module mapping, the vendored `include/` and build-file special cases). If you change those rules here, update `classify()` / `_cpp_companion()` and the constants in `scripts/check_docs_sync.py` to match, and vice-versa.

---

## Appendix A — What changed from the ChessCoach (Python) guide

For reviewers familiar with the original. This is the delta driven purely by the Python→C++ language and project difference.

**Removed (no analog in a chess engine):**
- The entire **database** documentation tree (`database.md`, `tables/`, `functions/`, `triggers/`, migrations) — the engine has no SQL database.
- **CSS / frontend** files and `.tsx` component docs — the engine has no UI.

**Changed / added for C++:**
- **Companion mapping is header-keyed.** A header + source pair shares **one** doc named after the header (`eval.hpp.md`), rather than one doc per file. Source-only and header-only files key off their single file.
- **Documented file extensions** are C++ (`.hpp .h .hxx .cpp .cc .cxx`) plus tooling (`.py`). The vendored `include/chess.hpp` is excluded.
- **Objects / Interfaces** covers C++ kinds — `class`, `struct`, `enum`/`enum class`, `union`, `concept`, `using`/`typedef` aliases, templates, and named `constexpr` constant sets — replacing Python `dataclass`/`TypedDict`/Pydantic and TS `interface`/`type`.
- **Functions** gained a **Template Parameters** subsection, **member-function** and **operator-overload** heading conventions, **overload/anchor-collision** disambiguation, and a **Performance** note for hot-path code. Parameter tables carry `const`/reference/pointer/ownership and in/out semantics; returns carry reference/pointer lifetime.
- **How to Run** became **How to Build & Run** (compile with CMake/Make, then `./chessai …`), not `python x.py`.
- **Infrastructure** (`infrastructure.md`, Docker) became **Build & Toolchain** (`build.md`, CMake/Make/flags/PGO/WSL2).
- **Terminology** is now the engine vocabulary (centipawn, ply, node, negamax, TT, SEE, PVS, NMP, LMR, NNUE, make/unmake, stand-pat).
- **Namespace** section added — C++ code lives in `engine`, with anonymous-namespace internal-linkage helpers called out as non-public.
- The **enforcement checker** stays Python but its `classify()` now implements the header-keyed C++ mapping, the vendored-`include/` exclusion, and the build-file → `build.md` mapping; the database/CSS branches are gone.
