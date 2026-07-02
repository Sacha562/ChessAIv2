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

ifeq ($(OS),Windows_NT)
    SUFFIX := .exe
else
    SUFFIX :=
endif
EXE ?= chessai$(SUFFIX)

.PHONY: all clean
all:
	$(CXX) $(CXXSTD) $(OPT) $(DEFS) $(WARN) $(INCLUDE) $(SOURCES) -pthread -o $(EXE)

clean:
	-rm -f chessai chessai.exe
