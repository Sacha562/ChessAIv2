#pragma once
#include "chess.hpp"
#include "types.hpp"

//
// Move ordering (Phase 1a step 6, extended in Phase 1b step 1). Alpha-beta converges
// far faster when the best move is searched first, and every later pruning layer
// assumes it. The canonical order used here:
//
//     TT / hash move
//   > winning & equal captures (SEE >= 0), MVV-LVA within
//   > killer 1 > killer 2 > countermove
//   > remaining quiet moves, by butterfly + continuation history score
//   > losing captures (SEE < 0), MVV-LVA within
//
// The killer / countermove / history / continuation-history signals come from
// [History](history.hpp.md).
//
namespace engine {

class History;
struct ContHistContext;

// MVV-LVA score of `move` on `board`: most-valuable victim, least-valuable
// attacker. Higher is searched earlier. En passant is scored as a pawn victim.
[[nodiscard]] int mvvLva(const Board& board, Move move);

// Order a main-search node's move list in place, best first (see the order above).
// `ttMove` (may be `Move::NO_MOVE`) is placed first when present in the list. Quiet
// ordering consults `hist`: the killers stored at `ply`, the countermove to
// `prevMove` (`Move::NO_MOVE` if none), the butterfly-history score, and the
// continuation-history score for the recent-move context `contCtx`.
void orderMoves(const Board& board, chess::Movelist& moves, Move ttMove, const History& hist,
                int ply, Move prevMove, const ContHistContext& contCtx);

// Order a quiescence node's capture list in place by MVV-LVA. No SEE split is
// applied — the q-search prunes losing captures itself before searching them.
void orderCaptures(const Board& board, chess::Movelist& moves);

} // namespace engine
