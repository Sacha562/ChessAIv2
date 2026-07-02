# High-Performance Chess AI in C++ — Component Plan & Decision Document

**Status:** Research complete. Meta-direction locked (2026-07-02): **top-tier / maximal** ambition, build for **this PC only** (`-march=native`, single binary), **Clang** release builds, **"you choose, I build."** Per-component selections still open — see the Decision Checklist.
**Board/move-gen foundation:** [Disservin `chess-library`](https://github.com/Disservin/chess-library) (already chosen).
**Primary research source:** [Chess Programming Wiki](https://www.chessprogramming.org) (CPW), cross-checked with Stockfish/OpenBench docs and the modern engine-dev ecosystem.

---

## How to use this document

Every component below lists **1–5 options**. Each option has: a short description, pros/cons, implementation **difficulty**, approximate **strength impact**, and **synergy** notes (what it pairs with). Each component ends with **→ My recommendation** and *why*.

At the very bottom is a **Decision Checklist** — go through it and pick one option per component. Anything you don't care about, just accept my recommended default and we move on.

### A note on the Elo numbers
Elo figures are approximate and come mostly from **mature-engine self-play** (Stockfish/Fishtest), where each technique fights for marginal gains against an already-strong baseline. **A new engine sees much larger absolute jumps** from the fundamentals (a transposition table or null-move pruning can be worth 50–150+ Elo early on, vs. single digits in Stockfish). Treat the numbers as *relative priority signals*, not promises. The only ground truth is your own SPRT testing (Component 29).

### The single most important idea
The engine is built in **layers that multiply each other**. Move ordering makes pruning safe; pruning makes depth cheap; depth makes evaluation matter; a transposition table makes everything faster and unlocks parallelism. **Build in the order given by the roadmap (Part 8), and test each layer with SPRT before adding the next.** A fancy technique on a broken foundation loses Elo.

---

## Table of Contents

- **Part 0 — Meta / Project-level decisions**
  - 0.1 Target strength & scope · 0.2 Language standard · 0.3 Toolchain & compiler · 0.4 Build flags & binary strategy
- **Part 1 — Board & Foundation**
  - 1. Board representation & move generation · 2. Make/Unmake vs Copy-Make · 3. Internal move encoding
- **Part 2 — Search Core**
  - 4. Search framework · 5. Main search variant · 6. Aspiration windows · 7. Quiescence search · 8. Static Exchange Evaluation (SEE) · 9. Search extensions
- **Part 3 — Move Ordering**
  - 10. Move-ordering stack · 11. Staged move generation
- **Part 4 — Pruning & Reductions**
  - 12. Null-move pruning · 13. Late move reductions · 14. Shallow-depth pruning trio · 15. Advanced selectivity
- **Part 5 — Evaluation**
  - 16. Evaluation strategy (HCE / NNUE / hybrid) · 17. HCE feature set · 18. Eval tuning · 19. NNUE architecture · 20. NNUE training pipeline
- **Part 6 — Infrastructure**
  - 21. Transposition table · 22. Zobrist hashing · 23. Time management · 24. Opening book · 25. Endgame tablebases · 26. Parallel search · 27. Protocol (UCI)
- **Part 7 — Tooling**
  - 28. Build system · 29. Testing & correctness
- **Part 8 — Recommended roadmap (phased)**
- **Part 9 — Recommended "default stack" at a glance**
- **Part 10 — Open questions for you**
- **Decision Checklist**

---

# Part 0 — Meta / Project-level decisions

These shape everything downstream. Worth settling first.

## 0.1 Target strength & scope

This is the biggest lever on the whole plan. It decides whether NNUE, parallel search, and tablebases are worth building.

| Option | What it means | Effort | Ends up around |
|---|---|---|---|
| **A. Learning-grade** | Solid HCE engine, clean search, single-thread. A great educational project. | Weeks | ~1800–2400 Elo |
| **B. Strong club engine (HCE, tuned)** | Full HCE + all classic search techniques + Texel tuning + TT + Lazy SMP. | 1–3 months | ~2600–2900 Elo |
| **C. Master-class / NNUE** | Everything in B, then migrate eval to a trained NNUE. This is where the modern strength ceiling is. | 3–6+ months | ~3000–3400+ Elo |
| **D. Top-tier** | C + big nets, huge tuning budget, 7-man tablebases, cluster testing. Diminishing returns, very high effort. | 6+ months, ongoing | 3400+ |

**→ My recommendation: aim for C, but build B first and treat NNUE as Phase 2.** The HCE engine (B) is not throwaway work — it generates the training data, provides the correctness harness, and forces the exact incremental make/unmake discipline NNUE's accumulator reuses. You get a strong, complete engine at the end of Phase 1 and a clear on-ramp to NNUE. If you only ever finish B, you still have a genuinely strong engine.

**✅ Your selection (2026-07-02): D — top-tier / maximal.** We still build B → C in sequence (that's the only sane path to D — the HCE foundation can't be skipped), but we aim each stage at its strongest settings: bigger NNUE nets and better feature sets, heavier tuning budgets, 6-man tablebases locally, PGO release builds. "Maximal" doesn't change the *order* of work — it raises the *ceiling* we point each component at.

## 0.2 Language / C++ standard

| Option | Notes |
|---|---|
| **C++17** | The library's minimum. Safe, universal, everything you need. |
| **C++20** | `std::bit_cast`, `<bit>` (`std::popcount`, `std::countr_zero`), concepts, `constinit`, better `constexpr`. Cleaner bit-twiddling without compiler intrinsics. Excellent compiler support in 2026. |
| **C++23** | Marginal extra niceties (`std::mdspan`, `std::print`); not needed. |

**→ My recommendation: C++20.** `<bit>` alone (portable `popcount`/bitscan without `__builtin_*` or `_mm_*`) is worth it for a bitboard engine, and support is rock-solid now. The library is C++17 but compiles fine under C++20.

## 0.3 Toolchain & compiler (Windows)

You're on Windows 11. This matters for performance — a bitboard engine is unusually sensitive to codegen quality, PGO, and intrinsics support.

| Option | Pros | Cons | Notes |
|---|---|---|---|
| **A. Clang/LLVM (clang-cl or clang++)** | Usually the **fastest codegen** for this workload; great `-march=native`, LTO, PGO; excellent intrinsics. | Slightly more setup on Windows. | Most modern engines favor Clang. **My pick.** |
| **B. MinGW-w64 GCC** | Also excellent codegen (often ~on par with Clang); GNU flags; what many engines ship with; native `make`. | Separate toolchain to install (MSYS2). | Great choice, and matches OpenBench's `make` convention. |
| **C. MSVC (cl.exe)** | Native Windows, VS integration, easy debugging. | Historically **~5–10% slower** on this kind of code; PGO workflow clunkier; PEXT/intrinsics spelled differently. | Fine for development/debugging; consider Clang/GCC for release builds. |

**→ My recommendation: develop with whatever you find comfortable (MSVC/VS is fine for debugging), but produce release builds with Clang or MinGW-w64 GCC.** I lean **Clang** for the release binary. We can keep CMake configured so all three work.

## 0.4 Build flags & binary strategy

| Concern | Recommendation |
|---|---|
| **Optimization** | `-O3` (Clang/GCC) / `/O2` (MSVC). |
| **Native ISA** | `-march=native` for your personal build (auto-enables POPCNT, BMI2/PEXT, AVX2 if your CPU has them). |
| **PEXT movegen** | Compile the library with `-DCHESS_USE_PEXT` **if** your CPU is modern Intel (Haswell+) or AMD Zen 3+. Older AMD has microcoded (slow) `pext` — leave it off there (falls back to magic bitboards). |
| **LTO** | Link-time optimization on for release — a few % NPS. |
| **PGO** | Profile-guided optimization (train on a `bench` run) — typically **+5–10% NPS ≈ +10–20 Elo** for free. Worth a build-script step later. |
| **Distribution (optional)** | If you ever share binaries, build multiple ISA variants like Stockfish does (`x86-64-v2` / `-v3` / `-v4`, or `sse4` / `avx2` / `bmi2`), because `-march=native` bakes in *your* CPU's features and will crash `SIGILL` on older machines. |

**→ Recommendation:** `-O3 -march=native -flto` for your local release build, PGO added via build script in Phase 1. Since it's a personal single-machine build, `-march=native` is ideal (no portability concern — confirmed by your "just me on this PC" choice). We'll empirically **benchmark magic vs. PEXT** (`-DCHESS_USE_PEXT`) with the `bench` command in Phase 0 and keep whichever is faster on your CPU — so I don't need to know your exact chip up front (though if you tell me the CPU model, I can pre-empt a couple of micro-decisions like AVX2 vs AVX-512 NNUE kernels and expected Lazy-SMP core scaling).

---

# Part 1 — Board & Foundation

You've chosen the Disservin `chess-library`. These components are about *how you use it*, not whether. Good news: the library already implements the top-tier combination (bitboards + magic/PEXT sliders + fully-legal, pin-aware generation + make/unmake + incremental Zobrist), validated at **~355M nps** perft (startpos d7) / **~449M nps** (Kiwipete d5).

## Component 1 — Board representation & move generation

The library's internals are fixed (bitboards + a 64-entry mailbox side-array; magic bitboards, optional PEXT). The **decision** is how your search consumes it.

| Option | Description | Pros | Cons | Difficulty |
|---|---|---|---|---|
| **A. Use `chess::Board` directly in search** | Your search tree operates on the library's `Board`; store its 16-bit `Move` in TT/PV. | Zero conversion cost; keeps the ~350–450M nps perft; less code; fewer bugs. | Coupled to the library's API. | Easy |
| **B. Thin wrapper around `Board`** | Wrap `Board` behind your own interface, delegating to it. | Insulates your code from library churn; a seam for later customization. | Boilerplate; risk of accidental copies; no perf benefit. | Easy–Med |
| **C. Custom board, library only for perft/validation** | Reimplement bitboards yourself, use the library just to cross-check. | Full control; learning value. | You throw away the fast, correct, tested foundation you were handed. Huge effort for likely-slower code. | Hard |

**→ My recommendation: A — use `chess::Board` directly.** The entire reason to adopt this library is to skip reinventing correct, fast move generation. Wrap it (B) only if you later hit a concrete limitation. C defeats the purpose.

**Integration specifics (from reading the header):**
- Generate moves: `chess::Movelist moves; chess::movegen::legalmoves(moves, board);` — fully legal, no per-node re-validation needed. `Movelist` is a fixed-capacity stack array: range-`for`, `.size()`, `[]`, `.add()`.
- Staged generation: `legalmoves<chess::movegen::MoveGenType::CAPTURE>(...)` and `...::QUIET` — feeds Component 11 and quiescence.
- Make/unmake: `board.makeMove(m); board.unmakeMove(m);` plus `makeNullMove()/unmakeNullMove()` (needed for Component 12) and `backtrackTo()` (~20% faster than repeated unmakes for some patterns).
- Hash: `board.hash()` → incrementally-maintained `uint64_t` (O(1)); `board.zobristAfter(m)` → post-move key *without* making the move (lets you **prefetch** the TT entry). **Do not** call `board.zobrist()` in search (it recomputes from scratch).
- Draw/terminal state: use `moves.empty()` + `board.inCheck()` for mate/stalemate, `board.isRepetition()`, `board.isHalfMoveDraw()`. **Avoid `board.isGameOver()` inside search** — it re-generates moves you already have.

## Component 2 — Make/Unmake vs Copy-Make

How the tree advances/retreats. The library's native model is make/unmake, but `Board` is copyable so copy-make is available.

| Option | Description | Pros | Cons | Difficulty |
|---|---|---|---|---|
| **A. Make/Unmake (library-native)** | Mutate one `Board`, undo on the way back up. | Low memory bandwidth, cache-friendly, the library's fast path; required style for incremental NNUE accumulator. | Must undo carefully (library handles it). | Easy (given the library) |
| **B. Copy-Make** | Copy the whole `Board` into a per-ply stack each node; "undo" = discard. | Trivial undo; random access to any ancestor position; parallelism-friendly. | Copies full state every node (more bandwidth). | Easy |

**→ My recommendation: A — make/unmake.** It's the library's optimized path and, importantly, it's the model NNUE's incremental accumulator (Component 19) is built around. Keep a per-ply "search stack" for *your* metadata (killers, static eval, current move, etc.) alongside the single mutated board.

## Component 3 — Internal move encoding

| Option | Description | Notes |
|---|---|---|
| **A. Reuse the library's 16-bit `Move`** | 6 bits from, 6 bits to, 2 bits type, 2 bits promo. `Move::make<...>()`, `.from()`, `.to()`, `.typeOf()`, `.promotionType()`. | Compact, TT-friendly, already correct. **My pick.** |
| **B. Custom move struct (add scored ordering key)** | Pair the `Move` with a separate `int score` for ordering, in a small struct or parallel array. | You'll want this for move ordering anyway — but keep the *canonical* move as the library's 16-bit token. |

**→ My recommendation: A for storage/TT/PV (16-bit token), plus an ordering score carried in a parallel `ScoredMove` struct during a node's move loop (B) — not stored long-term.** This keeps TT entries small and avoids duplicating move logic.

---

# Part 2 — Search Core

## Component 4 — Search framework

The recursion scaffold. Effectively non-negotiable; listed for completeness.

| Option | Description | Verdict |
|---|---|---|
| **A. Negamax + Iterative Deepening** | Single side-relative routine (`max(a,b) = -min(-a,-b)`); re-search depth 1,2,3… until time runs out. | **Standard.** ID gives free move ordering (previous PV/TT), clean time management, and TT reuse; it's usually *faster* than a direct fixed-depth search despite re-computation, because the leaf layer dominates. |
| **B. Plain Minimax** | Separate max/min. | Teaching only — 2× code, no clean α-β. Skip. |

**→ My recommendation: A.** Negamax + iterative deepening + fail-soft alpha-beta is the foundation everything else sits on. Non-negotiable.

**Conventions to fix now:** centipawn scores; mate scored as `MATE - ply` (so shorter mates are preferred and mate scores survive the TT correctly); draw = 0 with a tiny contempt option later.

## Component 5 — Main search variant (the pruning core)

| Option | Description | Pros | Cons | Difficulty | Impact |
|---|---|---|---|---|---|
| **A. Alpha-Beta, fail-hard** | Clamp returns to `[α,β]`. | Simplest correct pruning. | Discards bound info (worse TT/aspiration). | Easy | Baseline |
| **B. Alpha-Beta, fail-soft** | Return the best score even outside `[α,β]`. | Better TT bounds & aspiration behavior; "generally regarded better." | A touch more care. | Easy | small+ |
| **C. Principal Variation Search (PVS / NegaScout)** | Full window on move 1; zero-window "scout" `(α, α+1)` on the rest; re-search full only on fail-high. | ~10% fewer nodes; scout confirms good ordering cheaply. | Needs strong ordering (~90% fail-high-on-first) or the re-searches cost you. | Medium | moderate |
| **D. MTD(f)** | Repeated zero-window probes converging on the value from a seed. | Theoretically fewest nodes. | Fragile: heavy TT dependence, documented search instability, awkward with eval granularity. | Hard | situational |

**→ My recommendation: C, fail-soft PVS (i.e., B+C together).** This is the modern standard and composes perfectly with the transposition table, aspiration windows, and good move ordering. **Avoid MTD(f)** for a first strong engine — PVS + aspiration reaches the same node efficiency with far less fragility. Implementation note: PVS only pays off *after* move ordering is good, so wire the framework as fail-soft alpha-beta first, then switch move 2+ to zero-window once ordering (Component 10) is in.

## Component 6 — Aspiration windows

Narrowing the root window around the previous iteration's score to get more cutoffs.

| Option | Description | Pros | Cons | Difficulty |
|---|---|---|---|---|
| **A. Infinite window** | Always `(-∞, +∞)` at root. | No re-search logic. | Misses easy cutoffs. | Easy |
| **B. Fixed aspiration** | Window = prev score ± ~¼–½ pawn (25–50 cp). | More cutoffs, faster. | A fail-high/low forces a full re-search. | Easy |
| **C. Dynamic exponential widening (Stockfish-style)** | Start narrow (~10–25 cp); on a fail, widen **only the failing bound** exponentially (e.g. +25 → +100 → … → ∞); keep the other bound fixed. | Best of both; robust under search instability; standard in strong engines. | Slightly more logic; interacts with PVS re-searches (budget time for them). | Medium |

**→ My recommendation: C.** Start narrow and widen only the failing side. It's a clear, cheap win once iterative deepening is stable, and it plays nicely with fail-soft PVS. Gate it to kick in only at depth ≥ ~4–5 (shallow depths are noisy).

## Component 7 — Quiescence search

At leaf nodes, keep searching *forcing* moves (captures, maybe checks) until the position is "quiet," so the static eval isn't taken in the middle of a capture sequence. Mandatory for any tactical strength.

Core mechanic: **stand-pat** — take the static eval as a lower bound; if `stand_pat ≥ β` return immediately, else `α = max(α, stand_pat)`. When *in check*, forbid stand-pat and search **all** evasions.

| Design axis | Options | Recommendation |
|---|---|---|
| **Scope** | (a) captures only; (b) captures + queen promotions; (c) + checks (must be depth/ply-limited — qsearch has no natural depth bound). | **Start with (b).** Add (c) later behind a limit; it catches more tactics but risks node explosion. |
| **Capture pruning** | MVV-LVA ordering; **prune captures with SEE < 0** (materially losing). | **Both.** SEE-pruning losing captures is near-essential and huge for qsearch node counts. |
| **Delta pruning** | Skip a capture if `stand_pat + captured_value + margin < α`; "big delta" skips everything if even winning a queen can't reach α. | **Yes**, margin ~200 cp, big-delta ≈ queen (~975) +~775 if promotion. **Disable in low-material endgames** or it goes blind to material transitions. |

**→ My recommendation: captures + queen promos, SEE<0 pruning, delta pruning (off in late endgames); add depth-limited checks later.** Difficulty medium. This is where a large share of nodes go and it's the backbone of tactical strength — but it *requires* Component 8 (SEE) and decent capture ordering first.

## Component 8 — Static Exchange Evaluation (SEE)

Computes the net material outcome of the capture sequence on a single square (a "swap" algorithm: recursively take with the least-valuable attacker; each side may stand pat). One implementation powers **three** things: qsearch capture pruning (Component 7), capture ordering (Component 10), and check-extension gating (Component 9).

| Option | Description | Notes |
|---|---|---|
| **A. Bitboard swap algorithm (iterative)** | Use attackers-to-square bitboards; iterate least-valuable attacker; handle X-ray/discovered attackers by re-scanning sliders. | Standard, fast, non-recursive. **My pick.** |
| **B. Boolean SEE (sign-only)** | Optimize to answer "is SEE ≥ threshold?" without the exact value. | Even faster for the common "is this capture losing?" query; strong engines use this variant for pruning gates. |

**→ My recommendation: implement A, then add the B "SEE ≥ threshold" fast path.** Build this **early** — it's a multiplier that unblocks good quiescence and good capture ordering simultaneously. Difficulty medium. (Caveat: SEE ignores pins and post-exchange tactics — that's fine for its jobs.)

## Component 9 — Search extensions (selective depth ↑)

Extend the search on forcing lines. **Cap cumulative extensions per branch** (or gate by node type/depth) to prevent explosion.

| Option | Description | Difficulty | Impact | Notes |
|---|---|---|---|---|
| **A. Check extension** | Extend when giving/evading check (optionally only SEE ≥ 0 checks). | Easy | Significant (esp. SEE-gated) | Highest ROI extension; a cheaper alternative to putting all checks in qsearch. |
| **B. One-reply extension** | Extend when the side has a single legal move. | Easy | Small | Cheap, clearly forcing. |
| **C. Singular extension** | Do a reduced-depth, lowered-β search excluding the TT move; if *all* alternatives fail low, the TT move is "singular" → extend it. | Hard | Large ("big jump" in top engines) | Needs a robust TT; also enables negative extensions / multi-cut. Add late. |
| **D. Recapture / passed-pawn / mate-threat** | Situational extensions. | Med | Small | Mostly redundant with a strong qsearch; low priority. |

**→ My recommendation: A (check, SEE-gated) first; add C (singular) in Phase 1 late, once the TT is solid.** B is a cheap freebie you can add anytime. Skip D unless SPRT says otherwise.

---

# Part 3 — Move Ordering

**Move ordering doesn't change the result — it changes how fast alpha-beta converges, and it's the hard prerequisite for all the pruning in Part 4.** LMR, NMP, and LMP all assume the best moves are tried first. Get this right *before* adding aggressive pruning.

## Component 10 — Move-ordering stack

Canonical search order: **TT move → winning/equal captures & promotions → killers → quiet moves (history) → losing captures.** Pick which signals to include.

| Signal | Description | Difficulty | Impact | Depends on |
|---|---|---|---|---|
| **TT move** | Try the best move from a prior/shallower search of this exact position first. | Easy | Highest single ordering signal | A working TT (Component 21) |
| **MVV-LVA** | Order captures by most-valuable-victim / least-valuable-attacker. | Easy | Good default capture order | — |
| **SEE capture split** | Use SEE to put winning/equal captures before quiets and **losing captures after quiets**. | Med | Notable | Component 8 |
| **Killer heuristic** | Remember ~2 quiet moves that caused a β-cutoff at the same ply; try them early. | Easy | ~8% tree reduction | per-ply table |
| **History heuristic** | Per-(piece/from,to) score of cutoff success, bonus `~depth²`, with **aging + malus** for quiets that failed to cut. | Med | Big for quiet ordering | aging discipline |
| **Countermove** | Store the quiet refutation to the opponent's last move; bonus it (a 1-ply continuation history). | Easy | Small but cheap | prev move |
| **Continuation history** | History conditioned on the move played 1/2/(4/6) plies ago. | Med–Hard | Among the strongest modern quiet signals | more state threaded through search |
| **Capture history** | History table for captures (piece, to-sq, captured type); can augment/replace MVV-LVA. | Med | Small, tuned-engine gain | — |
| **IIR / IID** | When a node has no TT move, either just reduce depth (**IIR**, simple, modern) or do a shallow search to find a move (**IID**, classic). | Easy–Med | Rescues ordering when TT move missing | — |

**→ My recommendation, in two tiers:**
- **Phase-1 baseline (do all of these):** TT move → SEE-split captures (good before quiets, losing after) with MVV-LVA as the tiebreak → **killers** → **history** (with gravity/aging + malus) → **countermove**. Add **IIR** (not classic IID).
- **Phase-1 refinement (add after baseline is stable, tune with SPRT):** **1-ply + 2-ply continuation history**, then capture history.

This ordering is the foundation that makes Part 4 safe. History quality directly bounds how aggressively you can reduce/prune.

## Component 11 — Staged move generation

Generate moves in stages and stop early on a β-cutoff, rather than generating + scoring everything up front.

| Option | Description | Pros | Cons | Difficulty |
|---|---|---|---|---|
| **A. Generate-all, then sort/pick** | `legalmoves(ALL)`, score all, selection-sort the best each iteration. | Simplest; fine to start. | Wastes work generating moves you never try after a cutoff. | Easy |
| **B. Staged (TT move → captures → killers → quiets)** | Yield the TT move first (no gen), then `legalmoves<CAPTURE>`, then killers, then `legalmoves<QUIET>`; stop when a stage causes a cutoff. | Skips generating quiets entirely on the frequent early cutoff — real search speedup. | More bookkeeping (a small state machine). | Medium |

**→ My recommendation: start with A, migrate to B in Phase 1.** The library gives you the primitives for B for free (`legalmoves<CAPTURE>` / `<QUIET>`), and staged generation is a clean, measurable NPS win. Don't over-invest before the ordering signals (Component 10) exist, since B's whole point is ordering-driven early exit.

---

# Part 4 — Pruning & Reductions

These trade completeness for depth. **Every one risks tactical blindness and must be gated:** never prune/reduce when in check, near mate scores, or (mostly) at PV nodes; never blindly prune the TT move or clearly tactical moves; always re-search LMR fail-highs. Add them **one at a time, each behind an SPRT test.**

## Component 12 — Null-move pruning (NMP)

Give the opponent a free move at reduced depth; if the position is *still* ≥ β, assume the real position is too and prune. One of the two biggest Elo wins in the engine.

| Option | Description | Notes |
|---|---|---|
| **A. Basic NMP, fixed R=2 or 3** | Reduce by a constant. | Simple starting point. |
| **B. Adaptive R** | `R = base + depth/6 + min((eval-β)/200, 3)` style; deeper/further-ahead ⇒ reduce more. | Standard; stronger. **My pick.** |
| **C. + Verification search** | Re-search at reduced depth to confirm before trusting, to dodge zugzwang. | Crafty found it net-unhelpful; optional. |

**Guards (mandatory):** skip when in check, in a PV node, when eval < β, and when the side to move has only king+pawns (**zugzwang** risk — otherwise catastrophic in pawn endgames). Uses `makeNullMove()`/`unmakeNullMove()`.

**→ My recommendation: B (adaptive R), with the zugzwang/endgame guard; skip verification initially.** Depends on nothing but the TT and a static eval — add it early in the pruning phase. Also yields a "threat" signal you can reuse for ordering.

## Component 13 — Late move reductions (LMR)

Search late-ordered quiet moves at reduced depth; if one unexpectedly beats α, re-search it at full depth. The biggest modern gain after NMP; it pushes the effective branching factor toward < 2.

| Option | Description | Notes |
|---|---|---|
| **A. Fixed reduction** | Reduce late moves by 1 ply. | Crude; leaves Elo on the table. |
| **B. Logarithmic table** | `reduction ≈ 0.78 + ln(depth)·ln(moveCount)/2.4`, precomputed into a table. | Standard, strong. **My pick.** |
| **C. B + adjustments** | Modulate by: is-PV, improving, TT-move, killer, history score, gives-check, cut-node. | Where most of LMR's Elo actually lives; add incrementally. |

**Guards:** start reducing after ~3–4 moves and at depth ≥ 3; don't reduce captures/promotions/checks/killers (much) or in-check positions; **always re-search at full depth if a reduced search returns > α.**

**→ My recommendation: B, then layer in the C adjustments (start with is-PV, improving, and history-based).** LMR depends *entirely* on good ordering + a working history heuristic — that's why Part 3 comes first. This + NMP are the two techniques to get right.

## Component 14 — Shallow-depth pruning trio

Cheap forward-pruning near the horizon. Usually added together; all are low-difficulty, non-PV, low-depth, not-in-check, away-from-mate.

| Technique | Rule | Difficulty | Notes |
|---|---|---|---|
| **Reverse futility / static null-move (RFP)** | If `staticEval − margin ≥ β` at low depth, return early. | Easy | `margin ≈ ~75–150 × depth cp`, reduced when "improving." Trivial code, reliable Elo. |
| **Futility pruning** | Near horizon, if `staticEval + margin ≤ α`, skip quiet non-tactical moves. | Easy | Skip captures/checks; guard mate. |
| **Late move pruning / move-count pruning (LMP)** | Past a per-depth move-count threshold, skip the remaining quiets entirely. | Easy | e.g. skip quiets after ~`{d1:8, d2:12, d3:16, d4:24, …}` (tune). Low depth only. |

**→ My recommendation: add all three, individually SPRT-tested, after NMP + LMR are stable.** They're cheap and collectively worth a healthy chunk of Elo, but each needs its margins tuned to your eval's scale — so land them one at a time.

## Component 15 — Advanced selectivity (add last / optional)

| Technique | Description | Difficulty | Verdict |
|---|---|---|---|
| **Singular extensions** (see also Comp. 9C) | Extend a TT move proven singular; enables **negative extensions** (reduce non-singular moves) and **multi-cut**. | Hard | **High value**, add in late Phase 1. |
| **Razoring** | Near horizon, if `eval + margin < α`, drop straight to qsearch instead of a full search. | Med | Marginal now (~1 Elo); largely subsumed by NMP+RFP. Low priority. |
| **ProbCut** | A shallow capture search with a statistically-derived margin predicts a deep β-cutoff. | Hard | Works in modern Stockfish for captures; historically a washout elsewhere. Optional, tune-last. |
| **Multi-cut** | At expected cut-nodes, if several reduced searches fail high, prune. | Med | Often folded into singular search. Optional. |
| **History-based pruning** | Prune/reduce late quiets with very negative history. | Med | Nice complement to LMP once history is trustworthy. |

**→ My recommendation: singular extensions + history-based pruning are worth it; treat razoring / ProbCut / multi-cut as optional tune-last extras.** Only add these once the core (Parts 2–4) is solid and you're chasing marginal gains with SPRT.

---

# Part 5 — Evaluation

This is where the biggest strength difference lives, and the biggest fork in the plan.

## Component 16 — Evaluation strategy (the big fork)

| Option | Description | Strength | Effort | Runtime cost | Dependencies |
|---|---|---|---|---|---|
| **A. Hand-crafted eval (HCE)** | Linear sum of human-designed features × tuned weights. | Baseline; strong club level when tuned | Weeks of feature + tuning work | Cheap (~2× the NPS of NNUE) | Tapered eval, pawn hash, a tuner |
| **B. NNUE** | Small neural net evaluated on CPU with an incrementally-updated accumulator. | **~+200 Elo over the same engine's HCE** — the largest single jump in Stockfish's history | Training pipeline + SIMD kernels + data | ~½ the NPS, needs AVX2/AVX-512 | Incremental make/unmake, SIMD, GPU trainer, self-play data |
| **C. Hybrid** | NNUE main eval + a tiny material/HCE fast path for trivial cases; HCE used to bootstrap the first net. | Best of both (this is what real NNUE engines do) | A + B | — | All of the above |

**→ My recommendation: C, reached in stages — build A first, migrate to B, keep a minimal HCE fallback.** Crucially, **HCE is not throwaway**: it produces the labeled training data and the correctness harness for NNUE, and it forces the exact incremental-update discipline the accumulator reuses. NNUE does *not* need tapered eval (buckets replace it) but *does* need clean incremental make/unmake + SIMD. This staged path de-risks the hardest part of the engine.

*(If your answer to 0.1 was "A. Learning-grade," then stop at HCE and skip Components 19–20.)*

## Component 17 — HCE feature set

Roughly ordered by Elo-per-effort. Scores are side-relative centipawns; **everything hinges on tapered eval + a pawn hash table.**

| Feature | Description | Difficulty | Impact | Notes |
|---|---|---|---|---|
| **Material** | Piece values (P=100, N/B≈300–330, R≈500, Q≈900; Kaufman-tuned). | Easy | Baseline | Incremental counter. |
| **Piece-square tables (PSQT)** | Per-piece per-square bonus, separate opening/endgame tables. | Easy | +100–200 over material | Incrementally updatable. |
| **Tapered eval / game phase** | Interpolate midgame↔endgame by remaining material (`phase 0–24 → 0–256`). | Easy | Enables everything else | **Structural prerequisite.** |
| **Mobility** | Count of safe legal moves per piece (exclude squares hit by enemy pawns). | Med | Solid | Reuses attack tables. |
| **Pawn structure** | Doubled/isolated/backward penalties; passed/connected/phalanx bonuses; islands. | Med | Notable | **Needs a pawn hash table** (>95% hit rate) — feeds king safety & rook files. |
| **King safety** | Attack-unit accounting on the king zone → S-curve table; pawn shield/storm; tropism. | Hard | Large in sharp positions | Hardest to tune; nonlinear. |
| **Piece terms** | Bishop pair (~+50cp), rook on open/semi-open file, rook on 7th, knight outposts, bad bishop, early-queen penalty. | Med | Cumulative | Cheap, well-understood. |
| **Space / center / tempo** | Small positional nudges. | Med/Easy | Small | Add last. |

**→ My recommendation:** implement in this order, banking a playable milestone early:
1. **Material + PSQT + tapered eval** (a "PeSTO"-style eval already plays respectably — first real milestone).
2. **Add pawn hash + pawn structure, mobility, piece terms.**
3. **King safety** last (highest skill, highest payoff in tactics-rich positions).
Don't hand-pick weights — that's Component 18's job.

## Component 18 — Eval tuning

| Option | Description | Pros | Cons | Difficulty |
|---|---|---|---|---|
| **A. Texel tuning** | Logistic regression fitting weights to game results: minimize MSE of `sigmoid(K·eval)` vs. the 1/½/0 outcome. | Uses only your own self-play PGNs; large reliable gains (creator reports ~+100 Elo cumulative); needs only ~tens of thousands of games. | Eval must be deterministic (no TT/history leaking into the scored eval). | Med |
| **B. Gradient descent / SGD** | Analytic gradient of the same MSE loss. | Scales to thousands of weights; faster convergence. | Requires a differentiable eval path. | Med |
| **C. SPSA / CLOP** | Black-box; perturb params, judge by game results. | Tunes **search + eval jointly**, captures interactions. | Slow, noisy (needs many games). | Med |
| **D. TD-λ / TD-Leaf** | Self-play reinforcement, no labels. | No dataset needed. | Slow, historical. | Hard |

**→ My recommendation: A (Texel) for eval weights — it's the highest Elo-per-hour in the whole HCE phase — and C (SPSA) later for search parameters** (LMR/NMP/futility margins), which Texel can't tune. B is a fine faster alternative to A if you make the eval differentiable. Skip D.

## Component 19 — NNUE architecture *(Phase 2; skip if HCE-only)*

The modern "`(768 → N) × 2`" perspective network.

| Choice | Options | Recommendation |
|---|---|---|
| **Input feature set** | **768** (piece×color×square, no king bucketing) · **HalfKP** (piece relative to own king) · **HalfKA / HalfKAv2** (king as a feature, bucketed) · **HalfKAv2_hm** (+ horizontal mirroring; Stockfish's). | **Start with plain 768.** It captures most of the gain, is far easier to get correct, and is what most new engines ship first. Move to HalfKA-style later for the ceiling. |
| **Accumulator size N** | 256 … 3072 per side (top engines ≥1024). | **512–1024** to start (bigger is generally stronger but slower to train/eval). |
| **Activation** | **SCReLU** `clamp(x,0,QA)²` (strongest, most common) · **CReLU** (simpler to vectorize) · plain ReLU (overflow risk). | **CReLU first for a correct baseline, then SCReLU** for strength (needs the "square in int16" trick for AVX2 `madd`). |
| **Output** | Single scalar, optionally **output buckets / LayerStacks** (e.g. 8 buckets keyed on remaining material). | **Add ~8 output buckets** — cheap, replaces tapered eval, real gain. |
| **Quantization** | Integer inference: `QA≈255`, `QB≈64`, `SCALE≈400`; `eval = out·SCALE/(QA·QB)`. | Standard constants; match your trainer's. |

**Hard dependencies:** strictly-incremental accumulator updates on make/unmake (quiet move = 2 feature deltas, capture = 3, castle = 4), a ply-indexed accumulator stack, **hand-written AVX2/AVX-512 kernels** (without SIMD, NNUE is too slow to be worth it), and "Finny tables"/refresh cache for king-bucket boundary crossings.

**→ My recommendation: plain 768 → 512-or-1024 × 2, CReLU→SCReLU, ~8 output buckets, AVX2 kernels.** Get a *correct, small* net working end-to-end before scaling up architecture.

## Component 20 — NNUE training pipeline *(Phase 2; skip if HCE-only)*

| Aspect | Options | Recommendation |
|---|---|---|
| **Trainer** | **bullet** (Rust; the widely-adopted 2025/26 standard) · **nnue-pytorch** (Stockfish's) · marlinflow / Grapheus. | **bullet** — fast, well-supported, designed for exactly this. |
| **Data source** | Your **HCE engine's self-play** (labeled by shallow-search eval + game result/WDL) · existing public datasets · fine-tune an existing net. | **Self-play from your HCE engine** for the first net (this is why Phase 1 matters); mix eval + WDL labels. |
| **Data volume** | ~**300–600M** positions gets a first net working (+~200 Elo results reported at this scale); top nets use 3–6 billion. | Start ~300–600M; iterate. |
| **Hardware** | One consumer **GPU** trains a `768→N` net in hours-to-days. | A single modern GPU suffices. |
| **Loop** | generate → train → tournament-select → repeat. | Standard iterative improvement. |

**→ My recommendation: bullet + HCE self-play data (start ~300–600M positions, scale toward billions for "maximal") + GPU training, iterated.** This is a first-class deliverable, not an afterthought — budget for the data-gen and SIMD-kernel work explicitly.

**⚠️ AMD-GPU training reality (your RX 6800 XT — gfx1030 / RDNA2):** First, the reassuring part — this only affects *training*. **NNUE inference inside the engine runs on the CPU's AVX2/AVX-512**, so your GPU does **not** limit the engine's playing strength in any way. For *training*, the mainstream chess trainer `bullet` is CUDA-first and AMD support is not first-class. gfx1030 *is* in AMD's ROCm compatibility matrix, but on **Linux**, not cleanly on **Windows/WSL2** (WSL2 + ROCm on RX 6800 is currently flaky — documented `rocminfo`/`HSA_STATUS_ERROR_OUT_OF_RESOURCES` failures). Practical paths, best-first for a maximal target:
- **Rent a cloud NVIDIA GPU** (vast.ai / RunPod / Lambda) for training runs and use mainline `bullet` (CUDA). Training is intermittent and cheap per-run; least-friction, best-supported toolchain. *My default.*
- **Native Linux (dual-boot) + ROCm + `nnue-pytorch`** — gfx1030 has solid Linux ROCm support; uses your own GPU, no cloud cost, heavier setup.
- **`bullet` CPU backend on Windows** — zero GPU setup; fine for small nets, too slow for maximal-scale data.
- **WSL2 + ROCm** — possible but currently fights you; revisit once AMD's WSL path for gfx1030 stabilizes.

This is a Phase-2 decision (months out); we finalize it then. Nothing here blocks Phase 1.

---

# Part 6 — Infrastructure

## Component 21 — Transposition table (TT)

Caches search results by Zobrist key so transpositions aren't re-searched. **The single highest-value infrastructure component** and a prerequisite for move ordering, iterative deepening efficiency, and parallel search.

| Decision | Options | Recommendation |
|---|---|---|
| **Entry layout** | 8–16 bytes: key-check bits (16–32), best move (16), score (16), static eval (16, optional), depth (8), bound flag (2: EXACT/LOWER/UPPER), age (6). | **~10–16 byte entry.** Store the library's 16-bit move directly. |
| **Replacement scheme** | Always-replace · depth-preferred · **buckets (2–4 way, replace worst by depth − age·penalty)** · two-tier. | **Bucketed (2–4 way) + aging.** Best practical scheme; one cluster = one 64-byte cache line. |
| **Indexing** | modulo · **multiply-shift** `(hash * size) >> 64` (power-of-two-free, uses the full key). | **Multiply-shift** (Stockfish-style). |
| **Concurrency** | plain · **lock-less XOR (Hyatt)**: store `key XOR data`, reader XORs back & validates → torn writes self-detect. | **Lock-less XOR** — it's easy and it's the hinge that makes Lazy SMP (Component 26) nearly free. Do it from the start. |
| **PV/collision care** | Don't take TT *cutoffs* at PV nodes; always seed ordering from the TT move; verify move legality; be careful with mate/50-move scores through the TT. | Follow these rules to avoid instability and illegal-move crashes. |

**Sizing:** expose a UCI `Hash` option (MB); default ~half of free RAM. Bigger helps more as thread count rises.

**→ My recommendation: bucketed, aged, lock-less-XOR, multiply-shift-indexed TT with a ~16-byte entry — built early and built for threads.**

## Component 22 — Zobrist hashing

| Option | Description | Recommendation |
|---|---|---|
| **A. Use the library's `board.hash()`** | Incrementally maintained U64 key; `zobristAfter(m)` for TT prefetch. | **Use it** — it's correct, incremental, and free. **My pick.** |
| **B. Roll your own 781-key set** | 12×64 piece + side + castling + en-passant-file keys. | Only if you need a *specific* key set (e.g., Polyglot book compatibility uses Polyglot's published keys). |

**→ My recommendation: A for the TT/search; adopt Polyglot's key set only for the book path (Component 24) if you use one.** Use `zobristAfter(m)` to prefetch the TT entry before making the move — a cheap latency win.

## Component 23 — Time management

| Option | Description | Difficulty | Notes |
|---|---|---|---|
| **A. Fixed slice** | `time ≈ remaining/movestogo + inc`, or the robust `base/20 + inc/2`. | Easy | Captures most of the value. |
| **B. Soft/hard limits** | Soft = don't *start* a new ID iteration past it; hard = abort mid-search (checked every N nodes). | Easy | Worth tens of Elo vs. a fixed per-move slice. |
| **C. Stability scaling** | Spend less when the best move/score is stable across iterations; extend when the root move changes or eval drops. | Med | ~10–30 Elo. |
| **D. Node-based (`nodestime`)** | Budget in nodes, not wall-clock — deterministic, immune to load spikes. | Easy | Great for reproducible testing/CI. |

Always subtract a **move-overhead** buffer (~10–50 ms, UCI `Move Overhead`) for GUI/network lag.

**→ My recommendation: A+B+C (fixed base with soft/hard limits and stability scaling) + a move-overhead buffer; add D as a testing mode.** This is a clear multi-tens-of-Elo win and prevents the two catastrophic failure modes (time forfeits, and dumping all your time in one move).

## Component 24 — Opening book

| Option | Description | Recommendation |
|---|---|---|
| **A. None (GUI supplies openings)** | Modern top engines often ship no book; testing frameworks feed opening positions anyway. | **Perfectly fine, and simplest.** |
| **B. Consume a Polyglot `.bin`** | Binary-search a sorted array of 16-byte entries by Polyglot Zobrist key; play a weighted move. | Easy to add; huge public books exist. **Add if you want book play out of the box.** |
| **C. Own internal book + learning** | Custom format, adjust weights by results. | More code; defer unless you specifically want book learning. |

**→ My recommendation: A now, add B (read-only Polyglot) if/when you want it.** Skip writing a book generator (C). Note: your SPRT testing (Component 29) uses opening *suites*, not a book, so this doesn't block strength work.

## Component 25 — Endgame tablebases

Perfect-play databases. Probe **WDL in-search** (exact scores / pruning at any depth) and **DTZ at root** (progress under the 50-move rule).

| Option | Metric | Disk (approx) | Notes |
|---|---|---|---|
| **A. Syzygy 3–4–5-man** | WDL + DTZ50 | **~939 MB** | The sweet spot: cheap, fits in RAM/SSD, clearly positive, avoids endgame blunders. **My pick.** |
| **B. Syzygy 6-man** | WDL + DTZ50 | **~150 GB** | Only ~2–3 Elo in mature engines at fast TC; needs SSD; optional. |
| **C. Syzygy 7-man** | WDL + DTZ50 | **~16.7 TB** | Impractical for local use. Skip. |
| **D. Gaviota / Nalimov** | DTM | small / ~1.2 TB | Legacy; Syzygy is the de-facto standard. Skip. |

Use the **Fathom** probing library (MIT, standalone C: `tb_init`, `tb_probe_wdl`, `tb_probe_root`) — drop-in, no engine coupling. Watch the correctness rules (don't probe above the root's piece count; handle castling/en-passant restrictions).

**→ My recommendation: Syzygy 3–4–5-man via Fathom to start, behind a UCI `SyzygyPath` option.** Easy, clearly positive, and it eliminates a whole class of endgame blunders. **For your "maximal" target:** add **6-man Syzygy** (~150 GB, keep on SSD) once the basics work — that's the realistic local ceiling. **7-man is ~16.7 TB** and impractical to host locally; if you ever want 7-man perfection, probe it online (e.g. the Lichess tablebase API) at analysis time instead of storing it.

## Component 26 — Parallel search

| Option | Description | Difficulty | Scaling | Notes |
|---|---|---|---|---|
| **A. Lazy SMP** | N threads search the same root sharing one TT; diversity via staggered depths/aspiration. Coordination is *only* the shared TT. | **Easy** (~tens of lines) | ~+178 Elo at 8 threads (Stockfish LTC); scales well past 8 cores | **Requires the lock-less TT (Component 21).** The modern default. |
| **B. YBWC / Jamboree** | Search the eldest child fully, then split siblings at split points (master/slave). | Hard | Better time-to-depth | Lots of split-point bookkeeping & load balancing. |
| **C. Root splitting** | Assign root moves to threads. | Med | Poor past a few cores | Simple but limited. |
| **D. ABDADA / DTS** | Shared-hash with busy counters / dynamic tree splitting. | Hard | DTS scales best | Highest complexity. |

**→ My recommendation: A — Lazy SMP.** Enormous Elo for minimal code *because* you already built the lock-less TT. Expose a UCI `Threads` option. Bigger `Hash` compounds with more threads. (Build and debug the engine single-threaded first; turn on threads once it's correct.)

## Component 27 — Communication protocol

| Option | Description | Recommendation |
|---|---|---|
| **A. UCI** | Stateless; GUI owns clock/state. `uci`/`id`/`option` → `isready` → `ucinewgame` → `position … moves …` → `go …` → streamed `info … pv …` → `bestmove`. Standard options: `Hash`, `Threads`, `MultiPV`, `Ponder`, `SyzygyPath`, `Move Overhead`, `UCI_Chess960`. | **UCI only.** Universal in 2026 (Cutechess/fastchess, all modern GUIs, OpenBench). **My pick.** |
| **B. CECP / xboard** | Engine owns state. | Legacy; skip unless a specific old GUI requires it. |

**→ My recommendation: A — UCI.** Implement `stop`, `ponderhit`, `setoption`, and MultiPV. Pondering (searching on the opponent's clock) is a nice later add. The library gives you FEN/`position … moves` handling cleanly.

---

# Part 7 — Tooling

## Component 28 — Build system

| Option | Description | Recommendation |
|---|---|---|
| **A. CMake** | Broadest IDE/tooling support (VS, CLion, VS Code), cross-platform. | **Primary build.** Configure presets for Clang/GCC/MSVC. **My pick.** |
| **B. Meson + Ninja** | Fast, simple; what the library and lc0 use. | Fine alternative; slightly less ubiquitous on Windows. |
| **C. Plain Makefile** | — | **Also ship one** with an `EXE=` target — OpenBench and many test scripts build via `make`. |

**→ My recommendation: CMake as primary + a thin Makefile for OpenBench compatibility.** Wire PGO and the ISA/PEXT flags (Component 0.4) into build presets/scripts.

## Component 29 — Testing & correctness

Not optional — this is *how you make the engine strong.* You cannot eyeball Elo.

| Tool | Purpose | Priority |
|---|---|---|
| **Perft** | Validate move generation against known node counts (startpos d6 = 119,060,324). | **First, before anything else.** The library is already perft-correct, so this mainly guards *your* make/unmake and staged-gen glue. |
| **`bench` command** | `engine bench` runs a fixed suite, prints `<nodes> nodes <nps> nps`, exits. Must be **deterministic** (identical node count everywhere). | Add early; it's your build signature and OpenBench's speed/consistency gate. |
| **fastchess** (or cutechess-cli) | Runs engine-vs-engine matches locally with built-in **SPRT**. | **Core loop.** fastchess is the modern successor. |
| **SPRT** | Sequential test: keep a change iff the log-likelihood ratio crosses **+2.94** (accept), reject at **−2.94** (α=β=0.05). Gainer bounds `[0, 2]`; simplification/non-regression `[-1.75, 0.25]`. TCs: **STC 10+0.1s**, **LTC 60+0.6s**. | **Every change goes through this.** |
| **OpenBench** | Distributed SPRT across volunteer/your machines; builds via `make`, verifies via `bench`. | Optional; great once you have spare hardware or want faster tests. |

**→ My recommendation: perft-green first → deterministic `bench` → fastchess + SPRT as the daily loop → OpenBench later.** Rule: **get perft green before spending a single SPRT hour** — testing a buggy move generator wastes cores and misleads. Every technique in Parts 3–5 lands *individually, behind its own SPRT test.*

---

# Part 8 — Recommended roadmap (phased)

Each phase produces a working, playable engine that's stronger than the last. Test continuously.

**Phase 0 — Skeleton (a few days)**
Integrate `chess.hpp`; CMake + Makefile; UCI loop (`position`/`go`/`bestmove`); fixed-depth negamax + material-only eval; perft harness green; `bench` command. *Deliverable: a legal-move-playing UCI engine.*

**Phase 1a — Search core (1–2 weeks)**
Iterative deepening; fail-soft alpha-beta → PVS; quiescence (captures + SEE-prune + delta); SEE; transposition table (bucketed, lock-less, aged); MVV-LVA + TT-move ordering; soft/hard time management. *Deliverable: a tactically competent engine (~2000+).*

**Phase 1b — Ordering & pruning (1–2 weeks)**
Killers, history (+aging/malus), countermove, IIR; staged movegen; NMP; LMR; RFP + futility + LMP; aspiration windows; check extension. Each behind SPRT. *Deliverable: a strong search (~2400–2600).*

**Phase 1c — Evaluation (HCE) & tuning (2–4 weeks)**
PSQT + tapered eval → pawn hash + pawn structure + mobility + piece terms → king safety; Texel-tune weights; SPSA-tune search params; continuation history; singular extensions. *Deliverable: a strong, tuned HCE engine (~2700–2900).*

**Phase 1d — Infrastructure polish (days)**
Lazy SMP (`Threads`); Syzygy 3–4–5 via Fathom (`SyzygyPath`); stability-based time management; MultiPV; optional Polyglot book. *Deliverable: a complete, multi-threaded, tablebase-aware HCE engine.*

**Phase 2 — NNUE (1–3 months)**
Incremental accumulator on make/unmake; AVX2 kernels; self-play data generation from the HCE engine (~300–600M positions); train a `768→512/1024×2` SCReLU net with 8 buckets via **bullet**; swap eval to NNUE with a minimal HCE fallback; iterate (bigger nets, HalfKA, more data, AVX-512). *Deliverable: a master-class engine (~3000–3400+).*

**Phase 3 — Ongoing (optional)**
Bigger nets, PGO/ISA-variant release binaries, 6-man tablebases, OpenBench-driven parameter tuning, pondering, Chess960.

---

# Part 9 — Recommended "default stack" at a glance

If you just want my complete recommended set to accept wholesale, it's:

| Component | Default pick |
|---|---|
| 0.1 Target | **C** (HCE first → NNUE) |
| 0.2 Standard | **C++20** |
| 0.3 Compiler | **Clang** release (MSVC/GCC fine for dev) |
| 0.4 Flags | `-O3 -march=native -flto -DCHESS_USE_PEXT`, PGO later |
| 1 Board/movegen | **A** — use `chess::Board` directly |
| 2 Make/unmake | **A** — make/unmake |
| 3 Move encoding | **A** — library 16-bit `Move` |
| 4 Framework | **A** — negamax + iterative deepening |
| 5 Search variant | **C** — fail-soft PVS |
| 6 Aspiration | **C** — dynamic exponential widening |
| 7 Quiescence | captures+promos, SEE-prune, delta; checks later |
| 8 SEE | **A+B** — bitboard swap + sign-only fast path |
| 9 Extensions | **A** check (SEE-gated) now, **C** singular later |
| 10 Ordering | TT → SEE captures → killers → history → countermove → (continuation history) |
| 11 Staged movegen | **B** (after ordering exists) |
| 12 NMP | **B** — adaptive R + zugzwang guard |
| 13 LMR | **B→C** — log table + adjustments |
| 14 Shallow pruning | RFP + futility + LMP (all) |
| 15 Advanced | singular + history pruning; rest optional |
| 16 Eval strategy | **C** — hybrid, staged HCE→NNUE |
| 17 HCE features | PSQT+tapered → pawns+mobility+pieces → king safety |
| 18 Tuning | **A** Texel (eval) + **C** SPSA (search) |
| 19 NNUE arch | 768 → 512/1024 ×2, CReLU→SCReLU, 8 buckets |
| 20 NNUE training | **bullet** + HCE self-play (~300–600M pos) + 1 GPU |
| 21 TT | bucketed + lock-less XOR + aged + multiply-shift |
| 22 Zobrist | **A** — `board.hash()` |
| 23 Time mgmt | fixed base + soft/hard + stability + overhead |
| 24 Book | **A** none (optional read-only Polyglot) |
| 25 Tablebases | **A** — Syzygy 3–4–5 via Fathom |
| 26 Parallel | **A** — Lazy SMP |
| 27 Protocol | **A** — UCI |
| 28 Build | CMake + Makefile |
| 29 Testing | perft → bench → fastchess+SPRT → OpenBench |

---

# Part 10 — Open questions for you

Your answers will let me tailor the recommendations (especially Parts 0 and 5):

1. **Target strength / ambition (Component 0.1)?** Learning-grade HCE, strong HCE club engine, or all-the-way-to-NNUE? This gates ~⅓ of the plan.
2. **NNUE later — do you have a GPU** (and roughly what: e.g. an RTX-class card)? Determines whether Phase 2 is realistic and how big a net to target.
3. **Distribution:** just for yourself on this machine (then `-march=native` is perfect), or do you want portable binaries others can run?
4. **Timeline / intensity:** a focused sprint, or a long-running hobby project? Affects how much I front-load vs. spread out.
5. **Any constraints I should know:** must-use tools, Chess960/variants wanted, target GUI (Arena, Cute Chess, BanksiaGUI, En Croissant), or a specific opponent/rating you want to beat?
6. **Anything you specifically want to learn/build yourself** vs. have me implement? (E.g., some people want to write their own movegen for the learning even though we have the library — worth knowing.)

---

*Once you mark up the Decision Checklist below (or just say "go with your defaults"), I'll scaffold the project (Phase 0) and we'll build up from there, SPRT-testing as we go.*

---

# Decision Checklist

Fill in a pick per line (or write "default"):

```
0.1 Target strength ............ [ A learning | B strong-HCE | C NNUE | D top-tier ]
0.2 C++ standard ............... [ C++17 | C++20 | C++23 ]
0.3 Release compiler ........... [ Clang | MinGW-GCC | MSVC ]
0.4 Build flags ................ [ default | custom: __________ ]

1.  Board/movegen usage ........ [ A direct | B wrapper | C custom ]
2.  Make vs copy ............... [ A make/unmake | B copy-make ]
3.  Move encoding .............. [ A library Move | B custom ]

4.  Search framework ........... [ A negamax+ID (only sane pick) ]
5.  Main search variant ........ [ A fail-hard | B fail-soft | C PVS | D MTD(f) ]
6.  Aspiration ................. [ A none | B fixed | C dynamic ]
7.  Quiescence scope ........... [ captures | +promos | +checks ]
8.  SEE ........................ [ A swap | A+B swap+fastpath | none ]
9.  Extensions ................. [ check | +one-reply | +singular | +others ]

10. Ordering signals ........... [ default set | custom: __________ ]
11. Staged movegen ............. [ A gen-all | B staged ]

12. Null-move pruning .......... [ A fixed R | B adaptive | C +verification | off ]
13. LMR ........................ [ A fixed | B log | C log+adjust | off ]
14. Shallow pruning ............ [ RFP | futility | LMP | all | none ]
15. Advanced selectivity ....... [ singular | history-prune | razoring | probcut | none ]

16. Eval strategy .............. [ A HCE | B NNUE | C hybrid ]
17. HCE features ............... [ default staged set | custom: __________ ]
18. Tuning ..................... [ A Texel | B gradient | C SPSA | A+C ]
19. NNUE architecture .......... [ 768 | HalfKP | HalfKA | HalfKAv2_hm ] size ____
20. NNUE trainer ............... [ bullet | nnue-pytorch | other ]

21. Transposition table ........ [ default bucketed/lockless | custom ]
22. Zobrist .................... [ A board.hash() | B custom ]
23. Time management ............ [ A fixed | +B soft/hard | +C stability | +D node ]
24. Opening book ............... [ A none | B Polyglot read | C own ]
25. Tablebases ................. [ A Syzygy 3-4-5 | B +6-man | none ]
26. Parallel search ............ [ A Lazy SMP | B YBWC | C root | none/single ]
27. Protocol ................... [ A UCI | B CECP | both ]

28. Build system ............... [ CMake+Make | Meson | other ]
29. Testing .................... [ perft+bench+fastchess+SPRT (+OpenBench?) ]
```
