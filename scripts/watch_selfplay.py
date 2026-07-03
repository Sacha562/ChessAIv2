#!/usr/bin/env python3
"""Self-play -> self-contained HTML board. One command, open the result in a browser.

Drives one UCI engine to play itself, reconstructs each position from the engine's
own ``d`` dump (no chess library needed), and writes a standalone ``.html`` file
with the game embedded and an animated board viewer. It is the *watchable*
counterpart to the headless [match.py](match.py) harness — where that runs
fastchess for SPRT/regression, this produces a single game you can step through.

Dependency-free (Python 3.8+, standard library only). Everything runs on the
Linux/WSL2 toolchain per [README.md](../README.md). See
[watch_selfplay.py.md](watch_selfplay.py.md) for full documentation.

Usage::

    python3 scripts/watch_selfplay.py ./chessai                      # 20k nodes/move -> selfplay.html
    python3 scripts/watch_selfplay.py ./chessai --nodes 50000
    python3 scripts/watch_selfplay.py ./chessai --movetime 500 -o /mnt/c/Users/me/game.html

Then open the output file in any browser (write it under ``/mnt/c/...`` to
double-click it from Windows).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys

STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
PIECES = set("pnbrqkPNBRQK.")


# ---------------------------------------------------------------------------
# Self-play recorder (talks UCI, reads the engine `d` dump).
# ---------------------------------------------------------------------------

class Engine:
    """A single UCI engine subprocess, driven line by line."""

    def __init__(self, path: str) -> None:
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)

    def send(self, s: str) -> None:
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def wait_for(self, prefix: str) -> str | None:
        while True:
            line = self.p.stdout.readline()
            if not line:
                return None
            if line.strip().startswith(prefix):
                return line.strip()

    def ready(self) -> None:
        self.send("isready")
        self.wait_for("readyok")

    def set_position(self, moves: list[str]) -> None:
        self.send("position startpos moves " + " ".join(moves) if moves else "position startpos")
        self.ready()

    def bestmove(self, go_cmd: str) -> str | None:
        self.send(go_cmd)
        line = self.wait_for("bestmove")
        return line.split()[1] if line else None

    def fen(self) -> str | None:
        """Reconstruct the current position's FEN from the ``d`` dump."""
        self.send("d")
        rows: list[list[str]] = []
        fields: dict[str, str] = {}
        while True:
            line = self.p.stdout.readline()
            if not line:
                break
            s = line.strip()
            if s.startswith("Hash:"):
                break
            if ":" in s:
                key, _, val = s.partition(":")
                fields[key.strip()] = val.strip()
                continue
            toks = s.split()
            if len(toks) == 8 and all(len(t) == 1 and t in PIECES for t in toks):
                rows.append(toks)
        if len(rows) != 8:
            return None
        placement = "/".join(_compress(row) for row in rows)
        side = "b" if fields.get("Side to move") == "1" else "w"
        castling = fields.get("Castling rights") or "-"
        ep_idx = int(fields.get("EP", "64"))
        ep = "-" if ep_idx >= 64 else chr(97 + ep_idx % 8) + str(ep_idx // 8 + 1)
        return (f"{placement} {side} {castling} {ep} "
                f"{fields.get('Halfmoves', '0')} {fields.get('Fullmoves', '1')}")

    def quit(self) -> None:
        self.send("quit")


def _compress(row: list[str]) -> str:
    """Collapse a rank of piece tokens into its FEN run-length form."""
    out, empty = "", 0
    for cell in row:
        if cell == ".":
            empty += 1
        else:
            if empty:
                out += str(empty)
                empty = 0
            out += cell
    return out + (str(empty) if empty else "")


def record(engine_path: str, go_cmd: str, maxplies: int) -> dict:
    """Play one self-play game; return ``{"start", "moves":[{uci,fen}], "result"}``."""
    eng = Engine(engine_path)
    eng.send("uci")
    eng.wait_for("uciok")
    eng.send("ucinewgame")
    eng.ready()

    moves: list[str] = []
    records: list[dict] = []
    seen: dict[str, int] = {}
    result = "ply-cap"
    for _ in range(maxplies):
        eng.set_position(moves)
        mv = eng.bestmove(go_cmd)
        if mv in (None, "(none)", "0000"):
            result = "no-legal-moves"       # checkmate or stalemate
            break
        moves.append(mv)
        eng.set_position(moves)             # apply the move, then dump post-move FEN
        fen = eng.fen()
        records.append({"uci": mv, "fen": fen})
        print(f"\r[watch] playing... {len(records)} plies ({mv})",
              end="", file=sys.stderr, flush=True)
        if fen:
            parts = fen.split()
            key = " ".join(parts[:4])       # placement/side/castling/ep
            seen[key] = seen.get(key, 0) + 1
            if seen[key] >= 3:
                result = "threefold-repetition"
                break
            if int(parts[4]) >= 100:        # halfmove clock
                result = "fifty-move-rule"
                break
    eng.quit()
    return {"start": STARTPOS, "moves": records, "result": result}


# ---------------------------------------------------------------------------
# HTML emission.
# ---------------------------------------------------------------------------

def write_html(game: dict, out_path: str) -> None:
    """Inject the recorded game into the viewer template and write the file."""
    html = HTML_TEMPLATE.replace("__GAME_JSON__", json.dumps(game))
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html)


def main(argv: list[str]) -> None:
    ap = argparse.ArgumentParser(description="Record engine self-play and emit an HTML board viewer.")
    ap.add_argument("engine", help="path to the UCI engine binary (e.g. ./chessai)")
    limit = ap.add_mutually_exclusive_group()
    limit.add_argument("--nodes", type=int, default=20000, help="fixed nodes per move (default 20000)")
    limit.add_argument("--movetime", type=int, help="fixed ms per move instead of nodes")
    ap.add_argument("--maxplies", type=int, default=200, help="safety ply cap (default 200)")
    ap.add_argument("-o", "--out", default="selfplay.html", help="output HTML path (default selfplay.html)")
    args = ap.parse_args(argv)

    go_cmd = f"go movetime {args.movetime}" if args.movetime else f"go nodes {args.nodes}"
    print(f"[watch] self-play: {args.engine}  ({go_cmd})", file=sys.stderr)
    game = record(args.engine, go_cmd, args.maxplies)
    write_html(game, args.out)
    print(f"\n[watch] {len(game['moves'])} plies, result={game['result']}", file=sys.stderr)
    print(f"[watch] wrote {args.out} — open it in a browser.", file=sys.stderr)


HTML_TEMPLATE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ChessAIv2 — self-play</title>
<style>
  :root {
    --ground:#0f1319; --panel:#171d27; --panel-2:#1e2531; --line:#2a3342;
    --ink:#e6edf3; --muted:#8b98a9; --faint:#5b6675; --accent:#5eead4; --accent-dim:#2f9e8f;
    --sq-light:#cdd6e2; --sq-dark:#59677c; --last:rgba(94,234,212,0.38); --check:#f0736a;
    --mono:"Cascadia Code","JetBrains Mono","SF Mono",ui-monospace,Consolas,monospace;
    --sans:ui-sans-serif,system-ui,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  }
  * { box-sizing:border-box; }
  body { margin:0; }
  .wrap {
    max-width:1000px; margin:0 auto; padding:clamp(20px,4vw,48px) clamp(16px,4vw,40px) 40px;
    color:var(--ink); font-family:var(--sans); min-height:100vh;
    background:radial-gradient(1200px 500px at 80% -10%,rgba(94,234,212,0.06),transparent 60%),var(--ground);
  }
  header { margin-bottom:26px; }
  .eyebrow { font-family:var(--mono); font-size:12px; letter-spacing:0.22em; text-transform:uppercase; color:var(--accent); margin-bottom:12px; }
  h1 { font-size:clamp(26px,4.5vw,40px); line-height:1.05; margin:0 0 10px; font-weight:650; letter-spacing:-0.015em; text-wrap:balance; }
  h1 .dim { color:var(--faint); font-weight:350; }
  .sub { color:var(--muted); max-width:60ch; line-height:1.55; margin:0; font-size:15px; }
  .console { display:grid; grid-template-columns:minmax(0,1fr) 320px; gap:clamp(18px,3vw,34px); align-items:start; }
  @media (max-width:760px){ .console{ grid-template-columns:1fr; } }
  .board-col { min-width:0; }
  .board-frame { background:var(--panel); border:1px solid var(--line); border-radius:14px; padding:14px; }
  .board { width:100%; aspect-ratio:1; display:grid; grid-template-columns:repeat(8,1fr); grid-template-rows:repeat(8,1fr); border-radius:8px; overflow:hidden; box-shadow:0 10px 30px rgba(0,0,0,0.35); }
  .sq { position:relative; display:flex; align-items:center; justify-content:center; user-select:none; }
  .sq.light { background:var(--sq-light); } .sq.dark { background:var(--sq-dark); }
  .sq .coord { position:absolute; font-family:var(--mono); font-size:10px; opacity:0.65; font-weight:600; }
  .sq .coord.file { right:3px; bottom:2px; } .sq .coord.rank { left:3px; top:2px; }
  .sq.light .coord { color:#55627a; } .sq.dark .coord { color:#cdd6e2; }
  .sq .marker { position:absolute; inset:0; pointer-events:none; }
  .sq.last .marker { background:var(--last); }
  .sq.check .marker { background:radial-gradient(circle at center,var(--check) 0%,rgba(240,115,106,0.55) 42%,transparent 72%); }
  .piece { font-size:clamp(26px,7.4vw,52px); line-height:1; position:relative; z-index:1; transition:opacity 140ms ease; }
  .piece.w { color:#f6f8fb; text-shadow:0 0 1px #12161c,0.6px 0.6px 0 #10141a,-0.6px 0.6px 0 #10141a,0.6px -0.6px 0 #10141a,-0.6px -0.6px 0 #10141a; }
  .piece.b { color:#171b22; text-shadow:0 0 1px #d7dee8,0.6px 0.6px 0 #eef2f7; }
  .controls { display:flex; align-items:center; gap:8px; margin-top:14px; flex-wrap:wrap; }
  button { font-family:var(--mono); font-size:13px; color:var(--ink); background:var(--panel-2); border:1px solid var(--line); border-radius:8px; padding:8px 12px; cursor:pointer; transition:border-color 120ms ease,background 120ms ease,color 120ms ease; }
  button:hover { border-color:var(--accent-dim); }
  button:focus-visible { outline:2px solid var(--accent); outline-offset:2px; }
  button.primary { background:var(--accent); color:#06231f; border-color:var(--accent); font-weight:600; min-width:84px; }
  button.primary:hover { background:#7af0dd; }
  .spacer { flex:1; }
  .speed { display:inline-flex; gap:0; border:1px solid var(--line); border-radius:8px; overflow:hidden; }
  .speed button { border:0; border-radius:0; background:transparent; padding:8px 10px; color:var(--muted); }
  .speed button[aria-pressed="true"] { background:var(--panel-2); color:var(--accent); }
  .scrub-row { margin-top:12px; display:flex; align-items:center; gap:12px; }
  input[type="range"] { flex:1; -webkit-appearance:none; appearance:none; height:4px; border-radius:3px; background:var(--line); cursor:pointer; }
  input[type="range"]::-webkit-slider-thumb { -webkit-appearance:none; width:15px; height:15px; border-radius:50%; background:var(--accent); border:2px solid var(--ground); cursor:pointer; }
  input[type="range"]::-moz-range-thumb { width:15px; height:15px; border-radius:50%; background:var(--accent); border:2px solid var(--ground); cursor:pointer; }
  input[type="range"]:focus-visible { outline:2px solid var(--accent); outline-offset:3px; }
  .ply-count { font-family:var(--mono); font-size:12px; color:var(--muted); font-variant-numeric:tabular-nums; white-space:nowrap; }
  .telemetry { display:flex; flex-direction:column; gap:14px; }
  .card { background:var(--panel); border:1px solid var(--line); border-radius:12px; padding:14px 15px; }
  .card h2 { font-family:var(--mono); font-size:11px; letter-spacing:0.16em; text-transform:uppercase; color:var(--faint); margin:0 0 11px; font-weight:600; }
  .engine { display:flex; align-items:center; gap:10px; padding:7px 0; }
  .engine + .engine { border-top:1px solid var(--line); }
  .dot { width:11px; height:11px; border-radius:3px; flex:none; }
  .dot.white { background:#f6f8fb; } .dot.black { background:#171b22; border:1px solid #3a4454; }
  .engine .name { font-family:var(--mono); font-size:13px; }
  .engine .side { font-size:11px; color:var(--muted); margin-left:auto; letter-spacing:0.08em; text-transform:uppercase; }
  .engine.active .name { color:var(--accent); }
  .status { display:flex; align-items:center; gap:10px; flex-wrap:wrap; }
  .status .turn { font-family:var(--mono); font-size:13px; color:var(--muted); font-variant-numeric:tabular-nums; }
  .badge { font-family:var(--mono); font-size:11px; font-weight:700; letter-spacing:0.08em; text-transform:uppercase; padding:3px 8px; border-radius:5px; background:var(--panel-2); color:var(--faint); }
  .badge.check { background:rgba(240,115,106,0.15); color:var(--check); }
  .badge.mate { background:var(--check); color:#2a0b09; }
  .movelist { max-height:300px; overflow-y:auto; font-family:var(--mono); font-size:13px; font-variant-numeric:tabular-nums; }
  .mrow { display:grid; grid-template-columns:34px 1fr 1fr; gap:4px; padding:1px 0; }
  .mrow .no { color:var(--faint); text-align:right; padding-right:8px; }
  .mv { padding:2px 7px; border-radius:5px; cursor:pointer; color:var(--ink); }
  .mv:hover { background:var(--panel-2); }
  .mv.on { background:var(--accent); color:#06231f; font-weight:600; }
  .mv.empty { cursor:default; } .mv.empty:hover { background:transparent; }
  .result-banner { margin-top:2px; padding:12px 14px; border-radius:12px; border:1px solid var(--accent-dim); background:linear-gradient(180deg,rgba(94,234,212,0.10),rgba(94,234,212,0.03)); display:none; }
  .result-banner.show { display:block; }
  .result-banner .r-title { font-weight:650; font-size:15px; margin-bottom:3px; }
  .result-banner .r-sub { font-family:var(--mono); font-size:12px; color:var(--muted); }
  footer { margin-top:30px; padding-top:18px; border-top:1px solid var(--line); color:var(--faint); font-size:12.5px; line-height:1.6; max-width:70ch; }
  footer code { font-family:var(--mono); color:var(--muted); background:var(--panel); padding:1px 5px; border-radius:4px; font-size:11.5px; }
  @media (prefers-reduced-motion:reduce){ .piece{ transition:none; } }
</style></head>
<body>
<div class="wrap">
  <header>
    <div class="eyebrow">ChessAIv2 · self-play</div>
    <h1>The engine plays <span class="dim">itself</span></h1>
    <p class="sub">One binary, both sides. Recorded from the live engine and replayed below — step through with the controls or let it run.</p>
  </header>
  <main class="console">
    <section class="board-col">
      <div class="board-frame"><div class="board" id="board" role="img" aria-label="Chess board, self-play game"></div></div>
      <div class="controls">
        <button id="restart" title="Restart (Home)">&#8635;</button>
        <button id="prev" title="Previous move (Left)">&#9666;</button>
        <button id="play" class="primary" title="Play / pause (Space)">Play</button>
        <button id="next" title="Next move (Right)">&#9656;</button>
        <span class="spacer"></span>
        <div class="speed" role="group" aria-label="Playback speed">
          <button data-ms="1500">0.5&times;</button>
          <button data-ms="850" aria-pressed="true">1&times;</button>
          <button data-ms="380">2&times;</button>
        </div>
      </div>
      <div class="scrub-row">
        <input type="range" id="scrub" min="0" max="1" value="0" step="1" aria-label="Move scrubber">
        <span class="ply-count" id="plycount">0 / 0</span>
      </div>
    </section>
    <aside class="telemetry">
      <div class="card">
        <h2>Engines</h2>
        <div class="engine" id="eng-w"><span class="dot white"></span><span class="name">chessai</span><span class="side">White</span></div>
        <div class="engine" id="eng-b"><span class="dot black"></span><span class="name">chessai</span><span class="side">Black</span></div>
      </div>
      <div class="card">
        <h2>Status</h2>
        <div class="status"><span class="turn" id="turn">Start position</span><span class="badge" id="badge">&mdash;</span></div>
        <div class="result-banner" id="banner"><div class="r-title" id="r-title"></div><div class="r-sub" id="r-sub"></div></div>
      </div>
      <div class="card"><h2>Moves · UCI</h2><div class="movelist" id="movelist"></div></div>
    </aside>
  </main>
  <footer>Generated by <code>watch_selfplay.py</code>: the UCI engine played itself and each position was reconstructed from the engine's own <code>d</code> dump.</footer>
</div>
<script>
  const GAME = __GAME_JSON__;
  const GLYPH = { k:"♚", q:"♛", r:"♜", b:"♝", n:"♞", p:"♟" };
  const positions = [{ fen: GAME.start, move: null }].concat(GAME.moves.map(m => ({ fen: m.fen, move: m.uci })));
  const N = positions.length - 1;
  function parseFen(fen) {
    const parts = fen.split(" "); const board = [];
    for (const rankStr of parts[0].split("/")) {
      const row = [];
      for (const ch of rankStr) {
        if (/\d/.test(ch)) { for (let i=0;i<+ch;i++) row.push(null); }
        else row.push({ piece: ch.toLowerCase(), white: ch === ch.toUpperCase() });
      }
      board.push(row);
    }
    return { board, whiteToMove: parts[1] === "w" };
  }
  function sqToRC(sq){ return [8 - (+sq[1]), sq.charCodeAt(0) - 97]; }
  function inB(r,c){ return r>=0&&r<8&&c>=0&&c<8; }
  function get(b,r,c){ return inB(r,c)?b[r][c]:null; }
  function attacked(board,r,c,byWhite){
    const pr = byWhite ? r+1 : r-1;
    for (const pc of [c-1,c+1]){ const cell=get(board,pr,pc); if (cell&&cell.piece==="p"&&cell.white===byWhite) return true; }
    for (const off of [[-2,-1],[-2,1],[-1,-2],[-1,2],[1,-2],[1,2],[2,-1],[2,1]]){ const cell=get(board,r+off[0],c+off[1]); if (cell&&cell.piece==="n"&&cell.white===byWhite) return true; }
    for (let dr=-1;dr<=1;dr++) for (let dc=-1;dc<=1;dc++){ if(!dr&&!dc)continue; const cell=get(board,r+dr,c+dc); if (cell&&cell.piece==="k"&&cell.white===byWhite) return true; }
    const rays=[{dirs:[[-1,0],[1,0],[0,-1],[0,1]],types:["r","q"]},{dirs:[[-1,-1],[-1,1],[1,-1],[1,1]],types:["b","q"]}];
    for (const ray of rays) for (const d of ray.dirs){ let rr=r+d[0],cc=c+d[1]; while(inB(rr,cc)){ const cell=board[rr][cc]; if(cell){ if(cell.white===byWhite&&ray.types.includes(cell.piece)) return true; break; } rr+=d[0]; cc+=d[1]; } }
    return false;
  }
  function findKing(board,white){ for(let r=0;r<8;r++) for(let c=0;c<8;c++){ const cell=board[r][c]; if(cell&&cell.piece==="k"&&cell.white===white) return [r,c]; } return null; }
  const boardEl=document.getElementById("board"); const cells=[];
  for(let r=0;r<8;r++) for(let c=0;c<8;c++){
    const sq=document.createElement("div"); sq.className="sq "+((r+c)%2===0?"light":"dark");
    const marker=document.createElement("div"); marker.className="marker"; sq.appendChild(marker);
    if(c===0){ const rk=document.createElement("span"); rk.className="coord rank"; rk.textContent=8-r; sq.appendChild(rk); }
    if(r===7){ const fl=document.createElement("span"); fl.className="coord file"; fl.textContent=String.fromCharCode(97+c); sq.appendChild(fl); }
    const piece=document.createElement("span"); piece.className="piece"; sq.appendChild(piece);
    boardEl.appendChild(sq); cells.push({sq,piece});
  }
  function g(id){ return document.getElementById(id); }
  const els={ play:g("play"),prev:g("prev"),next:g("next"),restart:g("restart"),scrub:g("scrub"),plycount:g("plycount"),turn:g("turn"),badge:g("badge"),banner:g("banner"),rTitle:g("r-title"),rSub:g("r-sub"),movelist:g("movelist"),engW:g("eng-w"),engB:g("eng-b") };
  els.scrub.max=String(N);
  let index=0, playing=false, timer=null, stepMs=850;
  function render(){
    const pos=positions[index]; const parsed=parseFen(pos.fen); const board=parsed.board; const whiteToMove=parsed.whiteToMove;
    let fromRC=null,toRC=null;
    if(pos.move){ fromRC=sqToRC(pos.move.slice(0,2)); toRC=sqToRC(pos.move.slice(2,4)); }
    const kingRC=findKing(board,whiteToMove);
    const inCheck=kingRC&&attacked(board,kingRC[0],kingRC[1],!whiteToMove);
    const isEnd=index===N&&GAME.result==="no-legal-moves";
    const checkRC=inCheck?kingRC:null;
    for(let r=0;r<8;r++) for(let c=0;c<8;c++){
      const i=r*8+c; const cell=board[r][c]; const piece=cells[i].piece; const sq=cells[i].sq;
      piece.textContent=cell?GLYPH[cell.piece]:""; piece.className="piece"+(cell?(cell.white?" w":" b"):"");
      const isLast=(fromRC&&fromRC[0]===r&&fromRC[1]===c)||(toRC&&toRC[0]===r&&toRC[1]===c);
      const isCheck=checkRC&&checkRC[0]===r&&checkRC[1]===c;
      sq.classList.toggle("last",!!isLast); sq.classList.toggle("check",!!isCheck);
    }
    els.scrub.value=String(index); els.plycount.textContent=index+" / "+N;
    const moveNo=Math.ceil(index/2);
    els.turn.textContent= index===0 ? "Start position" : "Move "+moveNo+" · "+(whiteToMove?"White":"Black")+" to move";
    els.engW.classList.toggle("active",whiteToMove); els.engB.classList.toggle("active",!whiteToMove);
    els.badge.className="badge";
    if(isEnd&&inCheck){ els.badge.textContent="Checkmate"; els.badge.classList.add("mate"); }
    else if(isEnd){ els.badge.textContent="Stalemate"; els.badge.classList.add("mate"); }
    else if(inCheck){ els.badge.textContent="Check"; els.badge.classList.add("check"); }
    else { els.badge.textContent="—"; }
    if(isEnd){
      els.banner.classList.add("show");
      if(inCheck){ els.rTitle.textContent=(whiteToMove?"Black":"White")+" wins · checkmate"; els.rSub.textContent=Math.ceil(N/2)+" moves · final: "+pos.move; }
      else { els.rTitle.textContent="Draw · stalemate"; els.rSub.textContent="no legal moves"; }
    } else els.banner.classList.remove("show");
    document.querySelectorAll(".mv.on").forEach(function(e){ e.classList.remove("on"); });
    if(index>0){ const active=document.querySelector('.mv[data-i="'+(index-1)+'"]'); if(active){ active.classList.add("on"); active.scrollIntoView({block:"nearest"}); } }
  }
  (function(){
    const pairs=Math.ceil(GAME.moves.length/2);
    for(let p=0;p<pairs;p++){
      const row=document.createElement("div"); row.className="mrow";
      const no=document.createElement("span"); no.className="no"; no.textContent=(p+1)+"."; row.appendChild(no);
      for(const half of [0,1]){
        const mi=p*2+half; const mv=document.createElement("span");
        if(mi<GAME.moves.length){ mv.className="mv"; mv.dataset.i=String(mi); mv.textContent=GAME.moves[mi].uci; mv.addEventListener("click",function(){ pause(); index=mi+1; render(); }); }
        else mv.className="mv empty";
        row.appendChild(mv);
      }
      els.movelist.appendChild(row);
    }
  })();
  function tick(){ if(index>=N){ pause(); return; } index++; render(); if(index>=N) pause(); }
  function play(){ if(index>=N){ index=0; render(); } playing=true; els.play.textContent="Pause"; clearInterval(timer); timer=setInterval(tick,stepMs); }
  function pause(){ playing=false; els.play.textContent="Play"; clearInterval(timer); }
  function toggle(){ playing?pause():play(); }
  els.play.addEventListener("click",toggle);
  els.next.addEventListener("click",function(){ pause(); if(index<N){ index++; render(); } });
  els.prev.addEventListener("click",function(){ pause(); if(index>0){ index--; render(); } });
  els.restart.addEventListener("click",function(){ pause(); index=0; render(); });
  els.scrub.addEventListener("input",function(){ pause(); index=+els.scrub.value; render(); });
  document.querySelectorAll(".speed button").forEach(function(b){ b.addEventListener("click",function(){ document.querySelectorAll(".speed button").forEach(function(x){ x.setAttribute("aria-pressed","false"); }); b.setAttribute("aria-pressed","true"); stepMs=+b.dataset.ms; if(playing) play(); }); });
  window.addEventListener("keydown",function(e){ if(e.key==="ArrowRight"){ pause(); if(index<N){ index++; render(); } } else if(e.key==="ArrowLeft"){ pause(); if(index>0){ index--; render(); } } else if(e.key===" "){ e.preventDefault(); toggle(); } else if(e.key==="Home"){ pause(); index=0; render(); } });
  render(); setTimeout(play,700);
</script>
</body></html>
"""


if __name__ == "__main__":
    main(sys.argv[1:])
