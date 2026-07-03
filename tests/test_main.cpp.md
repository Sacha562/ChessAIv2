# Test: `test_main.cpp`

The doctest entry point for the unit-test binary. It contains **no test cases** — it
is the single translation unit that defines doctest's implementation and `main`
(via `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`). Every other `tests/test_*.cpp` includes
`<doctest.h>` (header-only, the impl lives here) and registers `TEST_CASE`s that are
linked into this binary, so adding a new test file never touches this one.

## Test Code

- **Test Code:** [test_main.cpp](test_main.cpp)
- **Vendored framework:** `include/doctest.h` (third-party, not documented here).
- **Test suites linked in:** [test_tt.cpp.md](test_tt.cpp.md).

## How to Build & Run

Built as part of the `chessai-tests` binary; it never runs on its own. See
[build.md](../build.md):

```bash
make test            # compiles tests/*.cpp + engine sources (minus main.cpp), runs all cases
```

## Tests

None. Registration and assertions live in the per-module `test_*.cpp` files listed
above.
