// doctest entry point for the ChessAIv2 unit-test binary.
//
// This translation unit is the ONE place that instantiates doctest's
// implementation and `main`; every other `tests/test_*.cpp` just includes
// <doctest.h> and registers `TEST_CASE`s that are linked into this binary.
// Keeping the runner separate means adding a new test file never touches this one.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
