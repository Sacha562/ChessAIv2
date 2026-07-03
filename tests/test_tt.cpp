// Unit tests for the transposition table (src/tt.*). Strategy: known-answer and
// property checks against the public API only — round-trip of every packed field,
// mate-score rebasing, bucket replacement / aging, lock-less edge cases (zero key),
// resize/clear, and a randomized stress pass. A single-cluster table (`mb == 0`,
// so multiply-shift maps every key to cluster 0) is used to exercise replacement
// deterministically.
#include "doctest.h"
#include "tt.hpp"

using namespace engine;

namespace {
Move            mv(uint16_t bits) { return Move(bits); }
const Move      NO_MV = Move(Move::NO_MOVE);
int             bi(Bound b) { return static_cast<int>(b); }   // for doctest printing
} // namespace

TEST_CASE("valueToTT / valueFromTT rebase mates and pass everything else through") {
    SUBCASE("non-mate scores are unchanged at any ply") {
        for (int ply : {0, 1, 7, 60}) {
            for (Value v : {-3000, -100, -1, 0, 1, 42, 3000}) {
                CHECK(valueToTT(v, ply) == v);
                CHECK(valueFromTT(v, ply) == v);
            }
        }
    }
    SUBCASE("VALUE_NONE passes through valueFromTT") {
        CHECK(valueFromTT(VALUE_NONE, 9) == VALUE_NONE);
    }
    SUBCASE("mates round-trip and are stored node-relative") {
        // A mate reachable `j` plies below a node that is `ply` from the root is
        // stored as mate_in(j) / mated_in(j); reading at the same ply restores it.
        for (int ply : {0, 1, 5, 20}) {
            for (int j : {1, 2, 10, 40}) {
                const Value win = mate_in(ply + j);
                const Value los = mated_in(ply + j);
                CHECK(valueToTT(win, ply) == mate_in(j));
                CHECK(valueToTT(los, ply) == mated_in(j));
                CHECK(valueFromTT(valueToTT(win, ply), ply) == win);
                CHECK(valueFromTT(valueToTT(los, ply), ply) == los);
            }
        }
    }
}

TEST_CASE("probe on an empty table misses") {
    TranspositionTable tt(1);
    CHECK_FALSE(tt.probe(0).hit);
    CHECK_FALSE(tt.probe(0xDEADBEEFull).hit);
}

TEST_CASE("store then probe round-trips every packed field") {
    TranspositionTable tt(1);
    tt.store(0xDEADBEEFull, 42, -1234, 777, mv(0x0ABC), BOUND_UPPER);
    const TTProbe p = tt.probe(0xDEADBEEFull);
    REQUIRE(p.hit);
    CHECK(p.depth == 42);
    CHECK(p.value == -1234);
    CHECK(p.eval == 777);
    CHECK(p.move.move() == 0x0ABC);
    CHECK(bi(p.bound) == bi(BOUND_UPPER));
}

TEST_CASE("re-storing the same key updates it in place") {
    TranspositionTable tt(1);
    tt.store(0x9, 3, 10, VALUE_NONE, mv(1), BOUND_UPPER);
    tt.store(0x9, 7, 20, VALUE_NONE, mv(2), BOUND_LOWER);
    const TTProbe p = tt.probe(0x9);
    REQUIRE(p.hit);
    CHECK(p.depth == 7);
    CHECK(p.value == 20);
    CHECK(p.move.move() == 2);
    CHECK(bi(p.bound) == bi(BOUND_LOWER));
}

TEST_CASE("a same-slot store carrying NO_MOVE preserves the stored move") {
    TranspositionTable tt(1);
    tt.store(0xABC, 5, 100, VALUE_NONE, mv(0x1234), BOUND_EXACT);
    tt.store(0xABC, 6, 120, VALUE_NONE, NO_MV, BOUND_LOWER);
    const TTProbe p = tt.probe(0xABC);
    REQUIRE(p.hit);
    CHECK(p.move.move() == 0x1234);   // preserved
    CHECK(p.depth == 6);              // everything else updated
    CHECK(bi(p.bound) == bi(BOUND_LOWER));
}

TEST_CASE("every Bound flag round-trips") {
    TranspositionTable tt(1);
    uint64_t k = 1;
    for (Bound b : {BOUND_UPPER, BOUND_LOWER, BOUND_EXACT}) {
        tt.store(k, 1, 1, VALUE_NONE, NO_MV, b);
        CHECK(bi(tt.probe(k).bound) == bi(b));
        ++k;
    }
}

TEST_CASE("extreme values and evals survive the int16 pack") {
    TranspositionTable tt(1);
    // The int16 fields span [-32768, 32767]; real engine scores never exceed
    // +/-VALUE_NONE (32002), so these boundary values probe the packing itself.
    const Value values[] = {VALUE_MATE, -VALUE_MATE, VALUE_MATE_IN_MAX_PLY,
                            VALUE_NONE, 32767, -32768, 0, -1, 1};
    uint64_t k = 1;
    for (Value v : values) {
        tt.store(k, 1, v, v, NO_MV, BOUND_EXACT);
        const TTProbe p = tt.probe(k);
        REQUIRE(p.hit);
        CHECK(p.value == v);
        CHECK(p.eval == v);
        ++k;
    }
}

TEST_CASE("depth round-trips across the signed int8 range") {
    TranspositionTable tt(1);
    for (int d : {-128, -1, 0, 1, 63, 100, 127}) {
        const uint64_t k = 0x100000ull + static_cast<uint64_t>(d + 200);
        tt.store(k, d, 10, VALUE_NONE, NO_MV, BOUND_EXACT);
        const TTProbe p = tt.probe(k);
        REQUIRE(p.hit);
        CHECK(p.depth == d);
    }
}

TEST_CASE("a mate score survives storage node-relative and rebases on read") {
    TranspositionTable tt(1);
    const int   ply = 4;
    const Value win = mate_in(ply + 3);
    tt.store(0x77, 5, valueToTT(win, ply), VALUE_NONE, NO_MV, BOUND_EXACT);
    const TTProbe p = tt.probe(0x77);
    REQUIRE(p.hit);
    CHECK(p.value == mate_in(3));                  // node-relative in the table
    CHECK(valueFromTT(p.value, ply) == win);       // root-relative after rebasing
}

TEST_CASE("a zero Zobrist key is stored and probed correctly") {
    TranspositionTable tt(1);
    CHECK_FALSE(tt.probe(0).hit);                  // empty slots read as key^data==0
    tt.store(0, 3, 55, VALUE_NONE, mv(7), BOUND_EXACT);
    const TTProbe p = tt.probe(0);
    REQUIRE(p.hit);
    CHECK(p.value == 55);
    CHECK(p.move.move() == 7);
    CHECK_FALSE(tt.probe(0xFFFF).hit);             // other keys must not match it
}

TEST_CASE("bucket replacement evicts the shallowest entry first") {
    TranspositionTable tt(0);   // one 4-way cluster: every key maps to cluster 0
    tt.newSearch();
    tt.store(0x100, 1, 50, VALUE_NONE, mv(1), BOUND_EXACT);   // slot 0, shallow
    tt.store(0x200, 10, 60, VALUE_NONE, mv(2), BOUND_EXACT);
    tt.store(0x300, 10, 70, VALUE_NONE, mv(3), BOUND_EXACT);
    tt.store(0x400, 10, 80, VALUE_NONE, mv(4), BOUND_EXACT);  // cluster full
    for (uint64_t k : {0x100, 0x200, 0x300, 0x400}) REQUIRE(tt.probe(k).hit);

    tt.store(0x500, 5, 90, VALUE_NONE, mv(5), BOUND_EXACT);   // must evict one
    CHECK_FALSE(tt.probe(0x100).hit);                         // the depth-1 entry
    for (uint64_t k : {0x200, 0x300, 0x400, 0x500}) CHECK(tt.probe(k).hit);
}

TEST_CASE("older generations are replaced before fresh ones of equal depth") {
    TranspositionTable tt(0);   // single cluster
    tt.newSearch();             // generation 1
    for (uint64_t k : {0x1, 0x2, 0x3, 0x4})
        tt.store(k, 10, 1, VALUE_NONE, mv(static_cast<uint16_t>(k)), BOUND_EXACT);

    tt.newSearch();             // generation 2 — the four entries are now one gen old
    tt.store(0x5, 10, 2, VALUE_NONE, mv(5), BOUND_EXACT);
    CHECK_FALSE(tt.probe(0x1).hit);   // an aged entry was evicted for the fresh key
    tt.store(0x6, 10, 3, VALUE_NONE, mv(6), BOUND_EXACT);
    CHECK(tt.probe(0x5).hit);         // the fresh gen-2 entry survives...
    CHECK_FALSE(tt.probe(0x2).hit);   // ...another aged entry is evicted instead
}

TEST_CASE("clear and resize drop all entries") {
    TranspositionTable tt(1);
    tt.store(0x55, 5, 1, VALUE_NONE, NO_MV, BOUND_EXACT);
    REQUIRE(tt.probe(0x55).hit);
    SUBCASE("clear") {
        tt.clear();
        CHECK_FALSE(tt.probe(0x55).hit);
    }
    SUBCASE("resize") {
        tt.resize(2);
        CHECK_FALSE(tt.probe(0x55).hit);
    }
}

TEST_CASE("hashfull reports current-generation occupancy in permille") {
    TranspositionTable tt(0);   // 1 cluster = 4 slots
    CHECK(tt.hashfull() == 0);
    tt.newSearch();
    tt.store(1, 5, 1, VALUE_NONE, NO_MV, BOUND_EXACT);
    tt.store(2, 5, 1, VALUE_NONE, NO_MV, BOUND_EXACT);
    CHECK(tt.hashfull() == 500);   // 2 of 4 slots at the current generation
}

TEST_CASE("prefetch on any key is safe") {
    TranspositionTable tt(1);
    tt.prefetch(0);
    tt.prefetch(0xFFFFFFFFFFFFFFFFull);
    CHECK(true);
}

TEST_CASE("randomized stress: immediate probe always hits and round-trips") {
    TranspositionTable tt(1);
    tt.newSearch();
    uint64_t x = 0x123456789ABCDEFull;   // xorshift, deterministic
    for (int i = 0; i < 20000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const Value v = static_cast<int16_t>(x);
        tt.store(x, 1, v, VALUE_NONE, mv(static_cast<uint16_t>(x)), BOUND_EXACT);
        const TTProbe p = tt.probe(x);
        REQUIRE(p.hit);                  // just-stored key must be found
        CHECK(p.value == v);
        CHECK(p.move.move() == static_cast<uint16_t>(x));
    }
}

TEST_CASE("identical operation sequences produce identical results") {
    auto run = [](TranspositionTable& t) {
        t.newSearch();
        for (uint64_t k = 1; k <= 200; ++k)
            t.store(k * 2654435761ull, static_cast<int>(k % 13), static_cast<int>(k),
                    VALUE_NONE, mv(static_cast<uint16_t>(k)), BOUND_EXACT);
        int hits = 0;
        for (uint64_t k = 1; k <= 200; ++k)
            if (t.probe(k * 2654435761ull).hit) ++hits;
        return hits;
    };
    TranspositionTable a(1);
    TranspositionTable b(1);
    CHECK(run(a) == run(b));
}
