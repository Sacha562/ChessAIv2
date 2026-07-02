# ChessAIv2 C++ Style Guide

> **Audience:** AI agents (and humans) writing or modifying C++ in the ChessAIv2 chess engine.
> **Purpose:** This document defines the exact C++ coding conventions for the project. The
> code must be **ultra-clean**: small, single-purpose, const-correct, allocation-free on hot
> paths, and free of spaghetti. Follow these rules precisely. **Do not deviate. Do not
> improvise. Do not invent your own conventions.** If a situation genuinely is not covered
> here, **ask the user** rather than guessing.
>
> **Scope boundary.** This guide governs the **C++ source** (`.hpp`/`.cpp` and friends). The
> colocated `.md` companions that *describe* each module are governed by a separate document,
> [`documentation-style-guide.md`](documentation-style-guide.md). The two are complementary
> and **both** are mandatory: **every code change requires a matching documentation change in
> the same turn** (see [§16 Enforcement](#16-enforcement-how-this-is-kept-honest)). Component
> design and the engine roadmap live in [`PLAN.md`](PLAN.md); build/run in
> [`README.md`](README.md) and [`build.md`](build.md).

---

## Table of Contents

- [0. The one rule: ultra-clean, no spaghetti](#0-the-one-rule-ultra-clean-no-spaghetti)
- [1. Language standard & platform](#1-language-standard--platform)
- [2. File & module structure](#2-file--module-structure)
- [3. Namespaces](#3-namespaces)
- [4. Includes & dependencies](#4-includes--dependencies)
- [5. Naming conventions](#5-naming-conventions)
- [6. Formatting & layout](#6-formatting--layout)
- [7. Types, constants & literals](#7-types-constants--literals)
- [8. Functions](#8-functions)
- [9. Classes & structs](#9-classes--structs)
- [10. Ownership, memory & lifetime](#10-ownership-memory--lifetime)
- [11. Const-correctness & immutability](#11-const-correctness--immutability)
- [12. Error handling](#12-error-handling)
- [13. Concurrency](#13-concurrency)
- [14. Performance discipline (hot paths)](#14-performance-discipline-hot-paths)
- [15. Comments](#15-comments)
- [16. Enforcement (how this is kept honest)](#16-enforcement-how-this-is-kept-honest)
- [17. Anti-patterns — the "no spaghetti" list](#17-anti-patterns--the-no-spaghetti-list)
- [18. Using the vendored chess-library](#18-using-the-vendored-chess-library)
- [Appendix A — Golden examples](#appendix-a--golden-examples)
- [Appendix B — Pre-finish checklist](#appendix-b--pre-finish-checklist)

---

## 0. The one rule: ultra-clean, no spaghetti

Everything below is in service of one goal: **code that a reader understands on the first
pass.** Concretely, that means:

1. **Small, single-purpose units.** One function does one thing; one class has one
   responsibility. If you cannot describe a function in a single sentence without "and", split it.
2. **Shallow control flow.** Prefer early-return guard clauses over nesting. Aim for a maximum
   nesting depth of **3**; deeper is a smell that the function should be decomposed.
3. **No duplication.** Say each thing once. Extract a helper before you copy a second time.
4. **No cleverness without payoff.** Bit tricks, SIMD, and manual unrolling are welcome on
   *measured* hot paths (see [§14](#14-performance-discipline-hot-paths)) and nowhere else.
   Cold code is written for clarity.
5. **Correctness is never traded for unmeasured speed.** The only proof of a speed win is
   `bench` (nodes/nps) and, for strength, SPRT — see [`PLAN.md`](PLAN.md) §29.

The existing modules [`eval.hpp`](src/eval.hpp)/[`eval.cpp`](src/eval.cpp) and
[`types.hpp`](src/types.hpp) are the reference for "clean" — study them before writing new code.

---

## 1. Language standard & platform

- **Standard: C++20.** Locked in [`PLAN.md`](PLAN.md) §0.2. Do not use C++23-only features.
- **Prefer standard `<bit>` over compiler intrinsics** for bit manipulation: `std::popcount`,
  `std::countr_zero`, `std::countl_zero`, `std::has_single_bit`, `std::bit_cast`. Reach for
  `__builtin_*` / `_pext_u64` / `_mm_*` intrinsics **only** inside a measured hot path where
  the standard form does not compile to the intended instruction, and isolate them behind a
  named helper with a comment naming the instruction.
- **Compiler: Clang-primary** release builds (GCC and MSVC must also compile cleanly). Write
  portable standard C++; do not rely on a single compiler's extensions. Guard any unavoidable
  intrinsic on the appropriate feature macro.
- **Single self-contained binary, `-march=native` for this machine.** No runtime plugin
  loading, no dynamic ISA dispatch in Phase 1 (that is a distribution concern, see
  [`build.md`](build.md)).
- **Warnings are errors in spirit.** Code must compile clean under `-Wall -Wextra`. Never
  silence a warning with a cast or pragma when the fix is to correct the code.

**Encouraged C++20 features:** `constexpr`/`consteval` for compile-time work, `concepts` to
constrain templates, `enum class`, `[[nodiscard]]`/`[[likely]]`/`[[unlikely]]`, structured
bindings, `std::optional`, `<bit>`, designated initializers for aggregates.

**Forbidden or strongly discouraged:**

| Never | Instead |
|-------|---------|
| C-style casts `(int)x` | `static_cast<int>(x)` (or `reinterpret_cast`/`bit_cast` when truly needed) |
| Owning raw `new`/`delete` | `std::unique_ptr`, `std::vector`, value types, stack storage |
| `#define` for constants or functions | `constexpr` values, `inline`/`constexpr` functions |
| `using namespace` in a header | qualified names or targeted `using chess::Board;` |
| `NULL` / `0` for pointers | `nullptr` |
| Exceptions on the search/eval hot path | sentinel returns / `std::optional` (see [§12](#12-error-handling)) |
| RTTI (`dynamic_cast`, `typeid`) in engine core | static polymorphism / templates |
| Naked magic numbers | a named `constexpr` (see [§7](#7-types-constants--literals)) |

---

## 2. File & module structure

A **module** is a header (`.hpp`) plus its optional same-stem source (`.cpp`) in the same
directory — the exact unit the [documentation guide](documentation-style-guide.md) keys its
companion to (`eval.hpp` + `eval.cpp` → `eval.hpp.md`).

- **Header guard: `#pragma once`.** Always, as the very first line. No `#ifndef` include guards.
- **File extensions:** headers `.hpp`, sources `.cpp`. Tests `test_<thing>.cpp`.
- **One module, one responsibility.** [`eval`](src/eval.hpp) evaluates, [`search`](src/search.hpp)
  searches, [`uci`](src/uci.hpp) talks the protocol, [`perft`](src/perft.hpp) counts. Do not
  let responsibilities bleed across modules.
- **Header holds the interface; source holds the implementation.** Declare the public API and
  the types callers need in the header. Put implementation, internal helpers, and file-local
  constants in the `.cpp` (in an anonymous namespace — see [§3](#3-namespaces)). Header-only is
  fine for pure `constexpr`/`inline` modules like [`types.hpp`](src/types.hpp).
- **Headers are self-contained.** A header must include (or forward-declare) everything it
  uses; a `.cpp` that includes only that header must compile. Never rely on transitive includes.
- **Keep headers light.** Do not include a heavy header just for a type used by reference or
  pointer — forward-declare instead. (The vendored `chess.hpp` is the one large header we do
  pull in where `Board`/`Move` are needed by value.)

**Canonical file skeletons**

Header (`foo.hpp`):

```cpp
#pragma once
#include <cstdint>          // std headers this interface needs
#include "types.hpp"        // project headers this interface needs

namespace engine {

// One-line intent. Detailed behavior lives in foo.hpp.md.
Value foo(const Board& board);

} // namespace engine
```

Source (`foo.cpp`):

```cpp
#include "foo.hpp"          // OWN header first — proves it is self-contained
#include "chess.hpp"        // then other project headers

#include <algorithm>        // then std headers, grouped and blank-line separated
#include <string>

using namespace chess;      // permitted in a .cpp only (see §3/§4)

namespace engine {

namespace {
// File-local (internal-linkage) constants and helpers live here.
constexpr Value V_SOMETHING = 42;
} // namespace

Value foo(const Board& board) {
    // ...
}

} // namespace engine
```

---

## 3. Namespaces

- **All first-party code lives in `namespace engine`.** No exceptions for new code.
- **Close every namespace with a matching comment:** `} // namespace engine`,
  `} // namespace`. This is mandatory and clang-format maintains it.
- **Internal-linkage helpers go in an anonymous namespace** in the `.cpp` — not `static`
  functions, not header-private tricks. See the `scoreToUci` helper in
  [`search.cpp`](src/search.cpp) and the `V_*` / `diff` helpers in [`eval.cpp`](src/eval.cpp).
  Everything in an anonymous namespace is private to that translation unit and is documented as
  non-public in the companion.
- **Namespace names are lowercase:** `engine`, `uci`. Do not indent inside a namespace body
  (`NamespaceIndentation: None`).
  > **Grandfathered exception:** the `Perft` and `Bench` module-namespaces
  > ([`perft.hpp`](src/perft.hpp), [`bench.hpp`](src/bench.hpp)) predate this guide and use
  > PascalCase. Leave them as-is; do **not** churn them just to rename. New namespaces are
  > lowercase.
- **`using namespace` is banned in headers** (it leaks into every includer). In a `.cpp`, a
  single `using namespace chess;` at file scope is allowed for ergonomics, as the existing
  sources do. Prefer targeted `using chess::Board;` aliases (as [`types.hpp`](src/types.hpp)
  does) when only a few names are needed.

---

## 4. Includes & dependencies

**Order**, top to bottom, blank-line-separated groups:

1. The unit's **own header** first (in a `.cpp`), so a broken/incomplete header fails loudly.
2. Other **project headers** (`"search.hpp"`, `"chess.hpp"`).
3. **Standard library** headers (`<atomic>`, `<string>`), alphabetized within the group.

```cpp
#include "search.hpp"
#include "eval.hpp"
#include "chess.hpp"

#include <algorithm>
#include <iostream>
#include <string>
```

- **Include what you use.** If you name `std::string`, include `<string>` — do not lean on it
  arriving via another header.
- **`SortIncludes` is off** in [`.clang-format`](.clang-format) so this hand-ordered grouping is
  preserved; keep the groups tidy yourself.
- **The vendored [`include/chess.hpp`](include/chess.hpp) is a hard dependency boundary.** It is
  third-party (see [§18](#18-using-the-vendored-chess-library)); include it where needed but do
  not modify it and do not document it.

---

## 5. Naming conventions

| Kind | Convention | Examples (from the codebase) |
|------|-----------|------------------------------|
| Class / struct / union | `PascalCase` | `Searcher`, `Limits`, `Engine`, `Case` |
| Enum / `enum class` | `PascalCase` type, `PascalCase` enumerators | `enum class Bound { Exact, Lower, Upper }` |
| `concept` | `PascalCase` | `template <class T> concept Scorable` |
| Type alias (`using`) | `PascalCase` | `using Value = int;` |
| Function / method | `camelCase` | `think`, `checkStop`, `elapsedMs`, `evaluate`, `moveToUci` |
| Namespace | `lowercase` | `engine`, `uci` |
| Local variable / parameter | `camelCase` | `maxDepth`, `nodeLimit`, `printBest` |
| Member variable | `camelCase_` (trailing underscore) | `stop_`, `nodes_`, `board_`, `rootBest_` |
| Named constant (global / file-local `constexpr`) | `SCREAMING_SNAKE_CASE` | `MAX_PLY`, `VALUE_MATE`, `V_PAWN`, `MOVE_OVERHEAD_MS` |
| Template parameter | `PascalCase` or `T`, `U` | `template <class Color>` |
| File | `lowercase`, module stem | `search.hpp`, `perft.cpp`, `test_see.cpp` |
| Macro (avoid; build-only) | `SCREAMING_SNAKE_CASE` | `CHESS_USE_PEXT` (build flag) |

**The single sanctioned exception:** a small set of `constexpr` idiomatic helpers in
[`types.hpp`](src/types.hpp) — `mate_in`, `mated_in`, `is_mate` — and the C-style entry shim
`run_uci` use `snake_case`, deliberately mirroring the C++ standard library and Stockfish/Chess
Programming Wiki idiom (`std::is_same`, Stockfish's `mate_in`/`mated_in`). Do not extend
`snake_case` to other functions; new engine functions use `camelCase`.

**Additional naming rules:**

- Names describe **meaning**, not type: `nodeLimit`, not `intLimit`; `rootBestCompleted_`, not
  `move2_`. Short single-purpose math/loop locals may be terse (`m`, `v`, `d`, `ms`, `inc`) —
  this is idiomatic in tight engine loops and is *only* acceptable when scope is a few lines.
- Boolean names read as predicates: `useTime_`, `timeUp_`, `printBest`, `infinite`.
- Do not abbreviate beyond established engine vocabulary (`pv`, `tt`, `nps`, `ply`, `cp`,
  `see`, `nmp`, `lmr` are fine and expected; invented abbreviations are not).
- Match the [documentation glossary](documentation-style-guide.md) terminology exactly:
  "ply" (half-move), "node", "negamax", "TT", "stand-pat", "make/unmake".

---

## 6. Formatting & layout

**Formatting is mechanically defined by [`.clang-format`](.clang-format) and is not a matter of
taste.** Run it before you finish; the rules below explain what it produces so you can write in
that shape from the start.

- **Indent: 4 spaces. Never tabs** (`UseTab: Never`).
- **Column limit: 100.** Break long expressions readably; clang-format will not shorten string
  literals (long FEN data lines in [`perft.cpp`](src/perft.cpp) may exceed 100 and that is fine).
- **Braces: attached (K&R / Egyptian).** Opening brace on the same line as the
  function/class/control statement; `else`/`catch` on the same line as the closing brace.

```cpp
if (score > best) {
    best = score;
    if (ply == 0) rootBest_ = m;
}
```

- **Single-statement bodies:** a guard may omit braces **only** when the whole statement fits on
  the same line as the condition (`if (printBest) std::cout << ...;`) or on the immediately
  following single line. Anything multi-line gets braces. When in doubt, use braces.
- **Pointer/reference bind to the type:** `const Board&`, `Board*`, `std::atomic<bool>&`
  (`PointerAlignment: Left`).
- **Const placement: West const** — `const Board&`, `const int mtg` (write `const` first, as the
  codebase does). Do not reorder existing qualifiers.
- **Space before parens for control statements, not calls:** `if (x)`, `for (...)`,
  `while (...)`; but `evaluate(board)`, `moves.size()`.
- **One statement per line.** The one blessed exception is a short guard block like
  `if (cond) { flag = true; return true; }` (see [`Searcher::checkStop`](src/search.cpp)).
- **Vertical alignment is on** (`AlignConsecutiveAssignments`/`Declarations`,
  `AlignTrailingComments`). Grouped declarations, struct fields, and constant blocks line up —
  see [`Limits`](src/search.hpp) and the `VALUE_*` block in [`types.hpp`](src/types.hpp). Let
  clang-format do this; do not fight it by hand.
- **Blank lines:** at most one consecutive blank line; none at the start of a block. Use single
  blank lines to separate logical paragraphs within a function.
- **Inline short accessors** on one line: `uint64_t nodes() const { return nodes_; }`.

---

## 7. Types, constants & literals

- **Fixed-width integers from `<cstdint>`** for anything with a size contract: `int64_t`,
  `uint64_t`, `uint8_t`. Bitboards, hashes, node counts, and clock values are `uint64_t`/`int64_t`.
  Plain `int` is fine for small in-register quantities (depth, ply, centipawn `Value`).
- **`Value` is the score type** (`using Value = int;`, centipawns, negamax perspective). Use it
  for all evaluation/search scores — never a bare `int` for a score.
- **Constants are `constexpr`, never `#define`.** Group and name them (see the `V_*` piece
  values in [`eval.cpp`](src/eval.cpp), the score constants in [`types.hpp`](src/types.hpp)).
- **No magic numbers.** Any number with meaning gets a named `constexpr` with a comment giving
  units/rationale: `MOVE_OVERHEAD_MS`, `TIME_CHECK_MASK` (`// check clock every 2048 nodes`).
  Loop-local literals with obvious meaning (`depth - 1`, `ply + 1`, `* 2`) are fine.
- **`enum class`, not plain `enum`,** for new enumerations (scoped, no implicit int conversion).
  List every enumerator with a one-line meaning in the companion.
- **Prefer `using` over `typedef`.**
- **`auto` with judgment.** Use it to avoid repeating an obvious or unspeakable type
  (iterators, `const auto& m : moves`). Do **not** use it when it hides a meaningful type from
  the reader (a function's return type in a header, a score, a `Value`). Spell out the type when
  the type *is* the information.
- **Braced initialization** to prevent narrowing where practical; default-initialize members
  in-class (`uint64_t nodes_ = 0;`).
- **`ull`/`u`/`ll` suffixes** on literals that must carry a width (`119060324ull`, `1000ull`).

---

## 8. Functions

- **Small and single-purpose.** Soft ceiling ~**60 lines**; past that, decompose. `think` and
  `search` in [`search.cpp`](src/search.cpp) are near the ceiling *because* they are the core
  loops — most functions are much shorter.
- **Guard-clause early returns** over nested `if`. [`Searcher::search`](src/search.cpp) returns
  early for time-up, draws, and leaves before the move loop — do that.
- **Parameter passing:**
  - Cheap/trivially-copyable (`int`, `Value`, `Move`, `bool`, small enums) → **by value**.
  - Non-trivial read-only input → **by `const&`** (`const Board&`, `const Limits&`).
  - Output or in/out → by **reference** (`Board&`, `Movelist&`); document it as *out* or *in/out*
    in the companion. Use a pointer only when the argument is genuinely optional/nullable.
  - **Deliberate copy** (e.g. [`Searcher::think(Board board, ...)`](src/search.hpp) copies so the
    search can mutate its own board via make/unmake) is allowed — comment *why* it is by value.
- **Return, don't out-param, when you can.** A single result is a return value. Reserve
  out-params for the cases where the library hands results back that way (`Movelist& moves`).
- **`[[nodiscard]]`** on any function whose return must not be ignored (a computed score, a
  success flag like [`Perft::test`](src/perft.hpp)).
- **`const` on every method that does not mutate** the object (`nodes() const`,
  `elapsedMs() const`).
- **`noexcept`** on functions that genuinely cannot throw and benefit from it (move
  constructors, small leaf helpers) — do not sprinkle it blindly.
- **Name boolean arguments at the call site** with a comment when the meaning is not obvious
  from context: `searcher.think(snapshot, limits, /*printBest=*/true, /*printInfo=*/true);`
  (see [`uci.cpp`](src/uci.cpp)). Better still, prefer an `enum class` or a small options struct
  when a call takes several bools.
- **No long parameter lists.** Group related parameters into a struct — this is exactly why
  [`Limits`](src/search.hpp) exists instead of eight `go` arguments.
- **One level of abstraction per function.** Do not mix high-level orchestration and low-level
  bit-twiddling in the same body; extract the low-level part.

---

## 9. Classes & structs

- **`struct` for passive aggregates** with public data and no invariant to protect
  ([`Limits`](src/search.hpp), the local `Case` in [`perft.cpp`](src/perft.cpp)). **`class` when
  there is an invariant, state, or behavior** to encapsulate ([`Searcher`](src/search.hpp),
  `Engine` in [`uci.cpp`](src/uci.cpp)).
- **`public` interface first, then `private`.** Keep data members `private` in a `class`.
- **Default member initializers** for every member that has a sensible default
  (`uint64_t nodes_ = 0;`, `bool useTime_ = false;`); the constructor then only sets what needs
  arguments.
- **Constructor initializer lists in declaration order**, and mark single-argument constructors
  **`explicit`** (`explicit Searcher(std::atomic<bool>& stopFlag)`).
- **Rule of zero.** Do not declare destructors/copy/move unless the class manages a resource. If
  you must declare one, declare the full set consistently (rule of five). `Engine` legitimately
  declares a destructor because it must **join its search thread** — that is RAII, see
  [§13](#13-concurrency).
- **A reference member makes the class non-copyable/assignable** (`std::atomic<bool>& stop_` in
  `Searcher`). That is intentional here; be aware of it and do not add reference members
  casually — prefer a pointer or a value if the object must be assignable.
- **No god classes.** If a class grows a second responsibility, split it. The `Engine` class
  owns protocol state and delegates search to `Searcher` and counting to `Perft`/`Bench` — it
  does not *do* those jobs itself.
- **Prefer free functions** for operations that do not need private state (`evaluate` is a free
  function, not an `Evaluator` method — there is no state to hide).

---

## 10. Ownership, memory & lifetime

- **No owning raw pointers, no `new`/`delete`.** Ownership is expressed with types:
  `std::unique_ptr<T>` for sole ownership, `std::vector<T>` for owned buffers, plain values and
  stack storage by default.
- **Value semantics first.** `Board`, `Move`, `Value` are cheap-ish values passed as described
  in [§8](#8-functions). The single mutated board threaded through the search (make/unmake) is
  the deliberate exception to "copy freely" — see [`PLAN.md`](PLAN.md) Component 2.
- **No heap allocation on the hot path.** Search, eval, movegen, and (later) qsearch must not
  allocate per node. `chess::Movelist` is a fixed-capacity stack array — use it; do not build a
  `std::vector` of moves per node. Per-ply search metadata lives in a preallocated stack, never
  in per-node allocations.
- **Non-owning references** (raw `T*` or `T&`) are fine to *borrow* without owning, provided the
  lifetime is obvious and outlives the borrow (`std::atomic<bool>& stop_` outlives the
  `Searcher`). Document ownership in the companion whenever a pointer is stored.
- **Prefer `std::array`/`std::span` over C arrays** for new code. (The local `Case cases[]`
  table in [`perft.cpp`](src/perft.cpp) is a file-local `static const` lookup and is acceptable
  as-is; new fixed tables should use `std::array`.)

---

## 11. Const-correctness & immutability

- **`const` by default.** A local that is not reassigned is `const` (`const int mtg = ...`,
  `const bool white = ...`, `const auto& m : moves` — the codebase does this consistently).
- **`constexpr` for anything computable at compile time** — constants and pure helpers
  (`mate_in`, `is_mate`, the `V_*` values).
- **`const&` parameters** for read-only inputs; **`const` methods** for non-mutating queries.
- **Immutability aids the optimizer and the reader.** Prefer transforming data into a new
  `const` value over mutating in place, except where a hot loop needs the mutation.

---

## 12. Error handling

The engine core is a real-time search: it **does not throw**. Errors are handled by strategy,
by layer:

- **Search / eval / movegen (hot core): no exceptions, no error codes.** Represent "no result"
  with a sentinel from [`types.hpp`](src/types.hpp): `Move(Move::NO_MOVE)`, `VALUE_NONE`, or
  `std::optional<T>` for a genuinely optional value. [`Searcher::think`](src/search.cpp)
  guarantees a legal fallback move rather than ever emitting a null `bestmove`.
- **Protocol / input boundary (UCI): tolerate and ignore malformed input.** Per the UCI spec,
  unknown tokens are silently ignored ([`Engine::loop`](src/uci.cpp)); malformed values are
  swallowed locally with a narrow `try { ... } catch (...) { /* ignore malformed */ }` exactly
  as [`Engine::handleSetOption`](src/uci.cpp) does. Never let a parse error crash the loop or
  escape to `main`.
- **`assert` for programmer invariants only** (things that are impossible unless the code is
  wrong), never for runtime/user input. Asserts compile out of release builds — do not put
  side effects in them.
- **Exceptions never cross the search recursion.** A throw unwinding through `search` would be
  both a correctness and a performance bug. Keep any exception contained to the boundary that
  raised it.
- **Fail loudly at startup, quietly in play.** A misconfiguration detectable at construction may
  report and exit; mid-game, prefer a safe legal move over a crash.

---

## 13. Concurrency

Search runs on a worker thread so the UCI thread stays responsive to `stop` — see
[`uci.cpp`](src/uci.cpp) and [`PLAN.md`](PLAN.md) Component 26 (Lazy SMP is the future
multi-thread model).

- **Cross-thread flags are `std::atomic` with an explicit memory order.** The shared stop flag
  is `std::atomic<bool>`, loaded with `std::memory_order_relaxed` on the hot check
  ([`Searcher::checkStop`](src/search.cpp)). Always state the memory order; never use the
  default seq-cst by omission on a hot path without reason.
- **Own your threads with RAII.** A launched `std::thread` must be `join`ed before its owner
  dies — `Engine::~Engine()` calls `stopSearch()`, which sets the flag and joins
  ([`uci.cpp`](src/uci.cpp)). **Never `detach`.**
- **Search on a snapshot.** The `go` handler copies the board and hands the copy to the worker
  (`Board snapshot = board_;`) so the engine's authoritative state is not raced.
- **No data races, ever.** Shared mutable state crosses threads only through atomics or (later)
  the lock-less TT (see [`PLAN.md`](PLAN.md) Component 21). Document the thread-safety contract
  of any type touched by more than one thread in its companion.
- **Prefer immutable sharing.** Data that does not change during a search should be shared as
  `const` and needs no synchronization.

---

## 14. Performance discipline (hot paths)

This is a performance engine; performance is a *discipline*, not an excuse for unclean code.

**What counts as a hot path:** anything called per-node — `search`, `evaluate`, move
generation/ordering, SEE, make/unmake, TT probe, and (Phase 2) the NNUE accumulator. Everything
else (UCI parsing, option handling, `bench`/`perft` setup) is cold and is written purely for
clarity.

**Hot-path rules:**

- **No allocation, no I/O, no locks** inside the per-node path. Print `info` lines from the
  driver loop, not from `search`.
- **Pass `const&`, keep data on the stack, prefer `constexpr`/`inline`.** Avoid virtual dispatch
  in the core; use templates/static polymorphism if you need variation.
- **Batch expensive checks.** Follow the existing pattern of checking the clock only every
  `TIME_CHECK_MASK + 1` nodes (`(nodes_ & TIME_CHECK_MASK) == 0`) rather than every node.
- **Respect the library's fast paths** (see [§18](#18-using-the-vendored-chess-library)): use
  incremental `board.hash()`, never `board.zobrist()`; avoid `board.isGameOver()` in search.
- **Branch-predictably.** Order conditions so the common case is the fast path; reserve
  `[[likely]]`/`[[unlikely]]` for measured, genuinely skewed branches — not decoration.

**Discipline around optimization:**

- **Measure first.** `./chessai bench` gives a deterministic node count and nps — the build
  signature. A change that claims a speedup must show it in `bench`; a change that claims Elo
  must pass SPRT ([`PLAN.md`](PLAN.md) §29). No "it feels faster."
- **Determinism is sacred.** `bench` must produce an identical node count everywhere. Do not
  introduce nondeterminism (unseeded randomness, iteration-order dependence, data races) into
  the search.
- **Never obscure code for an unmeasured gain.** If an optimization makes the code harder to
  read, it must be justified by a measured, commented win and localized behind a named helper.

---

## 15. Comments

- **Comment *why*, not *what*.** The code says what; a comment explains the non-obvious reason,
  the invariant, or the gotcha. Good examples already in the tree: the negamax sign-flip note in
  [`eval.cpp`](src/eval.cpp), the twofold-repetition rationale in [`search.cpp`](src/search.cpp),
  the zugzwang/soft-stop reasoning.
- **Engine subtleties must be commented** where they bite: negamax sign conventions, mate-score
  encoding (`VALUE_MATE - ply`), stand-pat, zugzwang guards, en-passant/castling edge cases, SEE
  X-ray handling. A future reader (or you) will not reconstruct these from the code alone.
- **File-top banner** (`//` block) states the module's purpose and any core convention, as
  [`types.hpp`](src/types.hpp) does. Keep it short; the full behavior contract lives in the
  `.md` companion.
- **`//` line comments**, sentence-style, kept in sync with the code they annotate. A comment
  that lies is worse than none — update or delete it when the code changes.
- **TODOs tie to the roadmap:** `// Phase 1a: quiescence replaces this static leaf.` Reference
  the [`PLAN.md`](PLAN.md) phase rather than leaving a bare `TODO`.
- **Behavioral/interface documentation is not a code comment** — it belongs in the module's
  `.md` companion per [`documentation-style-guide.md`](documentation-style-guide.md). Code
  comments are for *local implementation reasoning* only. Do not duplicate the companion in
  comments; they will drift.

---

## 16. Enforcement (how this is kept honest)

This guide is not advisory. It is enforced for LLMs and humans through layers that do not depend
on anyone remembering the rules — mirroring how [documentation is
enforced](documentation-style-guide.md#9-enforcement-automated).

### 16.1 Always-loaded rules (context injection)

- **Claude Code** auto-loads [`CLAUDE.md`](CLAUDE.md) into every session. It carries the
  "C++ must be ultra-clean" rule and points here as the single source of truth.
- **Cursor / Windsurf** (and other editors reading that convention) load
  [`.agents/rules/cpp-style.md`](.agents/rules/cpp-style.md), which is `trigger: always_on`.

If you edit the rule in one place, mirror it in the other and here.

### 16.2 Machine-enforced formatting — [`.clang-format`](.clang-format)

Formatting is defined by [`.clang-format`](.clang-format) at the repo root, so it is objective
and reproducible. Before finishing any C++ change, format the files you touched:

```bash
clang-format -i src/<file>.hpp src/<file>.cpp     # or your editor's format-on-save
```

The config encodes [§6](#6-formatting--layout): C++20, 4-space indent, 100 columns, attached
braces, left-aligned pointers, preserved include grouping, aligned consecutive
declarations/assignments, and namespace-comment fixups. Do not hand-format against it.

### 16.3 Static analysis — [`.clang-tidy`](.clang-tidy)

A curated [`.clang-tidy`](.clang-tidy) at the repo root flags bug-proneness, performance traps,
non-modern C++, and Core-Guidelines violations. It is **advisory** (it does not fail the build);
run it against a compile database:

```bash
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON   # once, to emit compile_commands.json
clang-tidy -p build src/*.cpp
```

Its check set is intentionally scoped: naming, magic numbers, and `std::endl` are **left out**
because this project's sanctioned `snake_case` idiom ([§5](#5-naming-conventions)), tuned
constants, and UCI line-flushing would make those checks misfire. The file's header comment
documents every carve-out — read it before adding or removing a check.

### 16.4 The automated gate — [`check_cpp_style.py`](scripts/check_cpp_style.py) hooks

A dependency-free checker enforces the mechanical rules clang-format cannot fix for you,
mirroring the documentation gate. [`.claude/settings.json`](.claude/settings.json) wires it into
two Claude Code hooks:

| Hook | When | Effect |
|------|------|--------|
| **PostToolUse** (`Edit`/`Write`/`MultiEdit`) | right after you edit a C++ file | Non-blocking nudge listing any findings + a clang-format reminder. |
| **Stop** | when you try to end the turn | **Blocks** while any changed C++ file has a *blocking* violation. |

It **blocks only** on the unambiguous, guide-explicit, clang-format-can't-fix set: tab
indentation, a header missing `#pragma once`, and `using namespace` in a header. Softer issues
(trailing whitespace, a heuristic C-cast flag) only nudge. Run it yourself any time:

```bash
python scripts/check_cpp_style.py --check
```

If it blocks, fix the violation — never work around the check. Full behavior in
[`check_cpp_style.py.md`](scripts/check_cpp_style.py.md).

### 16.5 What the tooling cannot check, you must

The tools cover formatting, a slice of semantics, and three mechanical rules. They do **not**
check most of what makes code clean: naming, single-responsibility, const-correctness,
ownership, hot-path allocation, and the "no spaghetti" rules in
[§17](#17-anti-patterns--the-no-spaghetti-list) are on **you**. Read this guide in full before
writing C++, and self-review against [Appendix B](#appendix-b--pre-finish-checklist) before you
finish.

### 16.6 The rule you must never break

**Every code change requires a matching documentation change, in the same turn** — enforced by
[`scripts/check_docs_sync.py`](scripts/check_docs_sync.py) and the Claude Code **Stop** hook,
which *blocks the turn from ending* while any C++ change lacks its `.md` companion update. That
check is about doc sync, not style; passing it does not mean your code is clean. Both bars must
be cleared.

> If a rule here seems wrong for a specific case, **ask the user** — do not invent an exception,
> and do not silently deviate.

---

## 17. Anti-patterns — the "no spaghetti" list

Reject these on sight (in your own code and in review):

| Anti-pattern | Why it's banned | Do instead |
|--------------|-----------------|------------|
| Deep nesting / arrow code | Unreadable, hides logic | Early-return guard clauses (max depth ~3) |
| God function / god class | No single responsibility | Split by responsibility |
| Copy-paste logic | Drift, double-maintenance | Extract a named helper (DRY) |
| Magic numbers | Meaning is invisible | Named `constexpr` with units/comment |
| `#define` for constants/logic | No scope, no type, no debugger | `constexpr` / `inline` functions |
| `using namespace` in a header | Pollutes every includer | Qualify, or targeted `using` in a `.cpp` |
| Boolean-parameter blindness | `foo(true, false, true)` is unreadable | `enum class` / options struct / `/*name=*/` |
| Primitive obsession | Bag of loose `int`s | A struct with named fields (`Limits`) |
| Out-params where a return works | Hidden mutation | Return the value |
| Raw owning pointers | Leaks, unclear ownership | `unique_ptr` / value / stack |
| Hidden global mutable state | Untestable, race-prone | Pass state explicitly / atomics |
| Heap allocation per node | Kills nps | Stack / `Movelist` / preallocated |
| Manual raw loop where an algorithm fits | More bugs, less intent | `std::` algorithms (off hot path) |
| Premature abstraction / speculative generality | Complexity with no user | Write the concrete thing; abstract when a 2nd caller appears |
| Cleverness on cold paths | Costs readability, buys nothing | Write the obvious code |
| Commented-out / dead code | Rots, confuses | Delete it (git remembers) |

---

## 18. Using the vendored chess-library

The Disservin [`chess-library`](include/chess.hpp) is the fixed board/movegen foundation
([`PLAN.md`](PLAN.md) Part 1). It is **third-party**: do not modify it, do not document it, and
follow its performance-correct usage exactly.

- **Use `chess::Board` and the 16-bit `chess::Move` directly** in search, TT, and PV — no
  wrapper, no custom board ([`PLAN.md`](PLAN.md) Components 1 & 3). Alias them once
  (`using chess::Board;`) as [`types.hpp`](src/types.hpp) does.
- **Generate moves with `movegen::legalmoves(moves, board)`** into a stack `Movelist`; the moves
  are fully legal — no per-node re-validation. Use `legalmoves<MoveGenType::CAPTURE>` /
  `<QUIET>` for staged generation and quiescence.
- **Advance with make/unmake:** `board.makeMove(m)` / `board.unmakeMove(m)`, and
  `makeNullMove()` / `unmakeNullMove()` for null-move pruning.
- **Hashing:** read the incremental key with `board.hash()`; use `board.zobristAfter(m)` to
  prefetch a TT entry. **Never call `board.zobrist()` in search** — it recomputes from scratch.
- **Terminal/draw detection in search:** use `moves.empty()` + `board.inCheck()` for
  mate/stalemate, and `board.isRepetition(1)` / `board.isHalfMoveDraw()` /
  `board.isInsufficientMaterial()`. **Avoid `board.isGameOver()` inside search** — it
  regenerates moves you already have.

These are correctness-and-speed rules, not preferences: the wrong call (`zobrist()`,
`isGameOver()`) silently costs nps or breaks determinism.

---

## Appendix A — Golden examples

Read these before writing new code — they are the reference for "clean" in this project:

- [`src/types.hpp`](src/types.hpp) — a header-only module: `#pragma once`, `engine` namespace,
  aligned `constexpr` constant blocks, `constexpr` predicate helpers, a tight file-top banner.
- [`src/eval.hpp`](src/eval.hpp) / [`src/eval.cpp`](src/eval.cpp) — a minimal header+source
  module: one public free function, file-local `constexpr` piece values and an `inline` helper in
  an anonymous namespace, a commented negamax sign convention.
- [`src/search.cpp`](src/search.cpp) — the hot core done cleanly: guard-clause early returns,
  batched clock checks, sentinel handling, no allocation or I/O in `search`.

## Appendix B — Pre-finish checklist

Before ending a turn that touched C++:

- [ ] `#pragma once` on new headers; code inside `namespace engine { ... } // namespace engine`.
- [ ] Names follow [§5](#5-naming-conventions) (PascalCase types, camelCase functions/locals,
      `member_`, `SCREAMING_SNAKE` constants).
- [ ] Ran (or mentally applied) [`.clang-format`](.clang-format): 4-space, 100-col, attached
      braces, grouped includes preserved.
- [ ] No magic numbers, no `#define` constants, no C casts, no `using namespace` in a header.
- [ ] Functions small and single-purpose; nesting ≤ ~3; early returns over arrow code.
- [ ] `const`/`const&`/`constexpr` applied; methods that don't mutate are `const`.
- [ ] No heap allocation, I/O, or locks on the per-node path; library fast paths respected.
- [ ] Threads joined (never detached); shared flags are `std::atomic` with an explicit order.
- [ ] Comments explain *why*; behavior contract updated in the module's `.md` companion.
- [ ] **Documentation companion updated in the same turn** (the Stop hook will check).
