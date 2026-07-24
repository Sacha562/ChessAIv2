# ChessAIv2 — Phase 0 Makefile
#
# OpenBench-compatible: it invokes `make EXE=<name>` and then runs `<name> bench`.
# Local use: just `make` (produces ./chessai or chessai.exe on Windows).

# clang++ is the default (`:=` beats make's built-in CXX=g++); override with `make CXX=g++`.
CXX      := clang++
CXXSTD   := -std=c++20
OPT      := -O3 -march=native -funroll-loops -flto -DNDEBUG
DEFS     := -DCHESS_USE_PEXT
WARN     := -Wall -Wextra -Wno-unused-parameter
INCLUDE  := -Iinclude -Isrc
SOURCES  := $(wildcard src/*.cpp)

# Unit tests: the doctest binary links every engine source EXCEPT main.cpp (doctest
# supplies its own main) plus everything under tests/. A lighter opt level (no LTO)
# keeps the edit-build-test loop and the commit gate fast; correctness is unaffected.
# -Wno-\#warnings silences the <ciso646>-deprecated #warning doctest triggers under
# a C++20 libstdc++ (the \# escapes make's comment character).
TESTOPT     := -O1 -march=native -DNDEBUG
TESTWARN    := $(WARN) -Wno-\#warnings
TESTSOURCES := $(wildcard tests/*.cpp) $(filter-out src/main.cpp,$(SOURCES))

ifeq ($(OS),Windows_NT)
    SUFFIX := .exe
else
    SUFFIX :=
endif
EXE ?= chessai$(SUFFIX)
TESTEXE ?= chessai-tests$(SUFFIX)
TUNEREXE ?= tuner$(SUFFIX)
EXTRACTEXE ?= extract$(SUFFIX)

.PHONY: all test tuner extract clean
all:
	$(CXX) $(CXXSTD) $(OPT) $(DEFS) $(WARN) $(INCLUDE) $(SOURCES) -pthread -o $(EXE)

# Build and run the unit tests (exit non-zero on any failing case).
test:
	$(CXX) $(CXXSTD) $(TESTOPT) $(DEFS) $(TESTWARN) $(INCLUDE) $(TESTSOURCES) -pthread -o $(TESTEXE)
	./$(TESTEXE)

# Offline Texel eval tuner (tools/tune.cpp). Links only the eval, not the search.
tuner:
	$(CXX) $(CXXSTD) $(OPT) $(DEFS) $(WARN) $(INCLUDE) tools/tune.cpp src/eval.cpp -o $(TUNEREXE)

# PGN -> labeled-FEN extractor for tuning data (tools/extract.cpp). Header-only.
extract:
	$(CXX) $(CXXSTD) $(OPT) $(DEFS) $(WARN) $(INCLUDE) tools/extract.cpp -o $(EXTRACTEXE)

clean:
	-rm -f chessai chessai.exe chessai-tests chessai-tests.exe tuner tuner.exe extract extract.exe
