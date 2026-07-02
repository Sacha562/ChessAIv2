---
trigger: always_on
---

**Reading order — docs first.** Every C++ module (a header plus its optional same-stem source) has one colocated `.md` companion, kept in sync with the code. To learn what code *does* (purpose, inputs/outputs, how it connects), read the `.md` companion first — it is authoritative for behavior and documents both the header interface and the source implementation in one place. Only read the actual source when you need to know how it *works*: writing or modifying code, fixing bugs, reasoning about exact implementation, or verifying docs match the code. If you compare docs to code and find a mismatch, fix the docs as part of your change. The vendored `include/chess.hpp` is third-party — do not document it.

**Every code change requires a matching documentation change, in the same turn.** When you create a module, create its `.md` companion. When you modify a header or source file, update its companion. When you delete or rename a module, delete or rename its companion. A C++ header and its source share ONE header-keyed companion (`eval.hpp` + `eval.cpp` → `eval.hpp.md`); source-only and header-only files key off their own name (`main.cpp` → `main.cpp.md`, `types.hpp` → `types.hpp.md`); build files map to the single `build.md`. Never finish a task with code changes whose docs have not been updated. Do NOT wait to be reminded, and do NOT batch documentation "for later".

Before writing or updating any documentation (`.md` companion files), read the documentation style guide located at the **workspace root**: `documentation-style-guide.md` (absolute path: `c:\Users\sivuc\projects\ChessAIv2\documentation-style-guide.md`). Read it **in full**. This file contains important information about the documentation style that should be followed **in all cases**.

If unsure about anything regarding documentation, **ask**. Do NOT assume or improvise, or create exceptions to the existing style guide on your own.

This rule is enforced automatically (see Section 9 of the style guide): `scripts/check_docs_sync.py` compares changed code against its `.md` companions (implementing the header-keyed module mapping), and in Claude Code a Stop hook blocks the turn from ending while anything is out of sync. Run `python scripts/check_docs_sync.py --check` to verify at any time. The equivalent always-on rule for Claude Code lives in `CLAUDE.md`.
