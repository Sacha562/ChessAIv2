"use strict";
// SmikkelBot opening-book dashboard. Hash-routed single page over the JSON API
// in server.py. See app.js.md.

const PIECES = { K: "♚", Q: "♛", R: "♜", B: "♝", N: "♞", P: "♟" };
const FILES = "abcdefgh";
const REFRESH_MS = 5000;

const view = document.getElementById("view");
let refreshTimer = null;
let routeSeq = 0; // bumped per navigation; renders started earlier must not paint
let explorerState = { selected: null, flipped: null };

// ---------------------------------------------------------------- helpers

function esc(value) {
  return String(value ?? "").replace(/[&<>"']/g, (c) => (
    { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function pct(x) { return x == null ? "—" : `${(x * 100).toFixed(1)}%`; }
function num(x, digits = 1) { return x == null ? "—" : Number(x).toFixed(digits); }
function when(ts) { return ts ? new Date(Number(ts) * (ts > 1e11 ? 1 : 1000)).toLocaleString() : "—"; }
function scoreClass(x) { return x == null ? "" : x >= 0.55 ? "good" : x < 0.45 ? "bad" : ""; }

function sfText(c) {
  if (c.sf_mate != null) return `#${c.sf_mate}`;
  if (c.sf_cp == null) return "—";
  return (c.sf_cp > 0 ? "+" : "") + (c.sf_cp / 100).toFixed(2);
}

async function api(path, options = {}) {
  const init = { ...options, headers: { "Content-Type": "application/json" } };
  if (options.body !== undefined) init.body = JSON.stringify(options.body);
  const response = await fetch(path, init);
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.detail ? JSON.stringify(data.detail) : response.statusText);
  return data;
}

function toast(message) {
  const el = document.getElementById("toast");
  el.textContent = message;
  el.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { el.hidden = true; }, 3500);
}

function parseHash() {
  const raw = location.hash.replace(/^#\/?/, "");
  const [path, query = ""] = raw.split("?");
  return { parts: path.split("/").filter(Boolean), params: new URLSearchParams(query) };
}

function exploreLink(moves, opponent, speed) {
  const params = new URLSearchParams();
  if (moves.length) params.set("moves", moves.join(","));
  if (opponent) params.set("opp", opponent);
  if (speed) params.set("speed", speed);
  return `#/explore?${params}`;
}

function setActiveNav(name) {
  document.querySelectorAll("[data-nav]").forEach((a) => {
    a.classList.toggle("active", a.dataset.nav === name);
  });
}

function updateBadge(worker) {
  const badge = document.getElementById("worker-badge");
  if (!worker) return;
  const state = worker.state || "unknown";
  badge.className = `badge ${state}`;
  const job = worker.job ? ` · ${worker.job.kind}` : "";
  const games = worker.active_games?.length ? ` · ${worker.active_games.length} live game(s)` : "";
  badge.textContent = `worker: ${state}${job}${games}`;
}

// ---------------------------------------------------------------- overview

async function renderOverview() {
  setActiveNav("overview");
  const seq = routeSeq;
  const data = await api("/api/overview");
  if (seq !== routeSeq) return;
  updateBadge(data.worker);
  const t = data.totals;
  const queueRows = Object.entries(data.queue).map(([kind, counts]) => `
    <tr><td>${esc(kind)}</td><td class="num">${counts.queued || 0}</td>
    <td class="num">${counts.running || 0}</td><td class="num">${counts.done || 0}</td>
    <td class="num">${counts.failed || 0}</td></tr>`).join("");
  const ingest = data.worker.last_ingest;
  view.innerHTML = `
    <h1>Overview</h1>
    <div class="tiles">
      <div class="tile"><div class="label">Games</div><div class="value">${t.games}</div></div>
      <div class="tile"><div class="label">Opponents</div><div class="value">${t.opponents}</div></div>
      <div class="tile"><div class="label">Score</div><div class="value ${scoreClass(t.score)}">${pct(t.score)}</div></div>
      <div class="tile"><div class="label">Analysed positions</div><div class="value">${data.coverage.analysed}</div></div>
      <div class="tile"><div class="label">Active candidates</div><div class="value">${data.coverage.candidates}</div></div>
      <div class="tile"><div class="label">Manual candidates</div><div class="value">${data.coverage.manual}</div></div>
    </div>
    <h2>By time control</h2>
    <div class="panel"><table>
      <tr><th>Speed</th><th class="num">Games</th><th class="num">Opponents</th><th class="num">Score</th><th></th></tr>
      ${data.speeds.map((s) => `<tr><td>${esc(s.speed)}</td><td class="num">${s.games}</td>
        <td class="num">${s.opponents}</td><td class="num ${scoreClass(s.score)}">${pct(s.score)}</td>
        <td><a href="${exploreLink([], "", s.speed)}">pooled book →</a></td></tr>`).join("")}
    </table></div>
    <h2>Worker</h2>
    <div class="panel">
      <div class="row">
        <span>State: <b>${esc(data.worker.state)}</b></span>
        <span class="muted">Last ingest: ${when(data.last_ingest_at)}${ingest?.error ? ` · <span class="bad">${esc(ingest.error)}</span>` : ingest ? ` · ${ingest.stored} new` : ""}</span>
        <button id="ingest-now">Ingest now</button>
      </div>
      ${data.worker.last_error ? `<div class="bad">Last error: ${esc(data.worker.last_error)}</div>` : ""}
      <table><tr><th>Job kind</th><th class="num">Queued</th><th class="num">Running</th><th class="num">Done</th><th class="num">Failed</th></tr>${queueRows}</table>
    </div>
    <h2>Recent book decisions</h2>
    <div class="panel"><table>
      <tr><th>When</th><th>Game</th><th>Opponent</th><th>Speed</th><th class="num">Ply</th><th>Move</th><th>Source</th></tr>
      ${data.events.map((e) => `<tr><td>${when(e.created_at)}</td>
        <td><a href="https://lichess.org/${esc(e.game_id)}" target="_blank" rel="noopener">${esc(e.game_id)}</a></td>
        <td><a href="#/opponent/${encodeURIComponent(e.opponent)}">${esc(e.opponent)}</a></td>
        <td>${esc(e.speed)}</td><td class="num">${e.ply}</td><td>${esc(e.move || "—")}</td>
        <td class="${e.source === "miss" ? "warn" : ""}">${esc(e.source)}</td></tr>`).join("")}
    </table></div>`;
  document.getElementById("ingest-now").onclick = async () => {
    await api("/api/ingest", { method: "POST" });
    toast("Ingest scheduled");
  };
}

// ---------------------------------------------------------------- opponents

async function renderOpponents() {
  setActiveNav("opponents");
  const seq = routeSeq;
  const rows = await api("/api/opponents");
  if (seq !== routeSeq) return;
  view.innerHTML = `
    <h1>Opponents</h1>
    <div class="row"><input id="filter" placeholder="Filter by name" autocomplete="off"></div>
    <div class="panel"><table id="opp-table">
      <tr><th>Opponent</th><th class="num">Games</th><th class="num">Score</th><th>By speed</th>
      <th class="num">Determinism</th><th class="num">Wins (replayable)</th><th>Last played</th></tr>
      ${rows.map((o) => `<tr data-name="${esc(o.opponent)}">
        <td><a href="#/opponent/${encodeURIComponent(o.opponent)}">${esc(o.name)}</a>${o.is_bot ? ' <span class="chip">BOT</span>' : ""}</td>
        <td class="num">${o.games}</td><td class="num ${scoreClass(o.score)}">${pct(o.score)}</td>
        <td>${o.speeds.map((s) => `<span class="chip">${esc(s.speed)} ${s.games} · ${pct(s.score)}</span>`).join("")}</td>
        <td class="num" title="${o.repeated_positions} repeated positions">${pct(o.determinism)}</td>
        <td class="num">${o.wins}</td><td>${when(o.last_played)}</td></tr>`).join("")}
    </table></div>`;
  document.getElementById("filter").oninput = (event) => {
    const needle = event.target.value.toLowerCase();
    document.querySelectorAll("#opp-table tr[data-name]").forEach((tr) => {
      tr.hidden = !tr.dataset.name.includes(needle);
    });
  };
}

async function renderOpponent(name) {
  setActiveNav("opponents");
  const seq = routeSeq;
  const d = await api(`/api/opponents/${encodeURIComponent(name)}`);
  if (seq !== routeSeq) return;
  view.innerHTML = `
    <h1>${esc(d.name)} <span class="muted">· determinism ${pct(d.determinism)} over ${d.repeated_positions} repeated positions</span></h1>
    <div class="panel"><table>
      <tr><th>Speed</th><th>Color</th><th class="num">Games</th><th class="num">W</th><th class="num">D</th><th class="num">L</th>
      <th class="num">Avg signal</th><th class="num">Avg book exit (ply)</th><th></th></tr>
      ${d.summary.map((s) => `<tr><td>${esc(s.speed)}</td><td>${esc(s.color)}</td><td class="num">${s.games}</td>
        <td class="num good">${s.wins}</td><td class="num">${s.draws}</td><td class="num bad">${s.losses}</td>
        <td class="num ${scoreClass(s.signal)}">${pct(s.signal)}</td><td class="num">${num(s.avg_exit_ply)}</td>
        <td><a href="${exploreLink([], d.opponent, s.speed)}">explore →</a></td></tr>`).join("")}
    </table></div>
    <h2>Most played lines (first ${6} plies)</h2>
    <div class="panel"><table>
      <tr><th>Speed</th><th>Color</th><th>Line</th><th class="num">Games</th><th class="num">Score</th><th class="num">Signal</th></tr>
      ${d.lines.map((l) => `<tr><td>${esc(l.speed)}</td><td>${esc(l.color)}</td>
        <td><a href="${exploreLink(l.moves, d.opponent, l.speed)}">${esc(l.san)}</a></td>
        <td class="num">${l.games}</td><td class="num ${scoreClass(l.score)}">${pct(l.score)}</td>
        <td class="num ${scoreClass(l.signal)}">${pct(l.signal)}</td></tr>`).join("")}
    </table></div>
    <h2>Recent games</h2>
    <div class="panel"><table>
      <tr><th>Date</th><th>Game</th><th>Speed</th><th>Color</th><th class="num">Result</th><th class="num">Signal</th>
      <th class="num">Exit ply</th><th>Opening</th></tr>
      ${d.games.map((g) => `<tr><td>${when(g.created_at)}</td>
        <td><a href="https://lichess.org/${esc(g.id)}" target="_blank" rel="noopener">${esc(g.id)}</a></td>
        <td>${esc(g.speed)}</td><td>${esc(g.color)}</td>
        <td class="num ${scoreClass(g.result)}">${g.result === 1 ? "1" : g.result === 0 ? "0" : "½"}</td>
        <td class="num ${scoreClass(g.signal)}">${pct(g.signal)}</td>
        <td class="num" title="${esc(g.exit_source)}">${g.exit_ply}</td><td>${esc(g.opening)}</td></tr>`).join("")}
    </table></div>`;
}

// ---------------------------------------------------------------- explorer

function parseFenBoard(fen) {
  const squares = {};
  fen.split(" ")[0].split("/").forEach((rank, r) => {
    let file = 0;
    for (const ch of rank) {
      if (/\d/.test(ch)) { file += Number(ch); continue; }
      squares[`${FILES[file]}${8 - r}`] = ch;
      file += 1;
    }
  });
  return squares;
}

function boardHtml(pos, flipped) {
  const squares = parseFenBoard(pos.fen);
  const last = pos.moves.length ? pos.moves[pos.moves.length - 1] : "";
  const selected = explorerState.selected;
  const targets = new Set(pos.legal.filter((m) => m.move.slice(0, 2) === selected).map((m) => m.move.slice(2, 4)));
  let html = "";
  for (let row = 0; row < 8; row += 1) {
    for (let col = 0; col < 8; col += 1) {
      const file = flipped ? 7 - col : col;
      const rank = flipped ? row + 1 : 8 - row;
      const sq = `${FILES[file]}${rank}`;
      const piece = squares[sq];
      const classes = ["sq", (file + rank) % 2 === 1 ? "light" : "dark"];
      if (last && (last.slice(0, 2) === sq || last.slice(2, 4) === sq)) classes.push("last");
      if (sq === selected) classes.push("selected");
      if (targets.has(sq)) classes.push("target");
      const glyph = piece ? `<span class="piece ${piece === piece.toUpperCase() ? "w" : "b"}">${PIECES[piece.toUpperCase()]}</span>` : "";
      const coord = col === 0 ? `<span class="coord">${rank}</span>` : "";
      html += `<div class="${classes.join(" ")}" data-sq="${sq}">${glyph}${coord}</div>`;
    }
  }
  return html;
}

function moveListHtml(pos, opponent, speed) {
  if (!pos.san_path.length) return '<span class="muted">Start position</span>';
  return pos.san_path.map((san, i) => {
    const prefix = i % 2 === 0 ? `${i / 2 + 1}. ` : "";
    const cls = i === pos.san_path.length - 1 ? "current" : "";
    return `${prefix}<a class="${cls}" href="${exploreLink(pos.moves.slice(0, i + 1), opponent, speed)}">${esc(san)}</a>`;
  }).join(" ");
}

function candidateRows(pos, opponent, speed) {
  if (!pos.candidates.length) {
    return `<tr><td colspan="13" class="muted">No candidates yet${pos.jobs.length ? ` — job ${esc(pos.jobs[0].kind)} ${esc(pos.jobs[0].status)}` : ""}.</td></tr>`;
  }
  return pos.candidates.map((c) => {
    const status = !c.enabled ? '<span class="muted">disabled</span>'
      : c.exhausted ? '<span class="bad">exhausted</span>'
        : c.playable ? '<span class="good">playable</span>' : '<span class="warn">locked tier</span>';
    const tierCell = c.tier >= 99 ? "—" : c.tier;
    return `<tr class="${c.enabled ? "" : "dim"}">
      <td><a href="${exploreLink([...pos.moves, c.move], opponent, speed)}"><b>${esc(c.san)}</b></a></td>
      <td class="num">${tierCell}</td><td>${esc(c.source)}</td>
      <td class="num">${sfText(c)}</td><td class="num">${pct(c.sf_expected)}</td>
      <td class="num">${num(c.pooled_n, 1)}</td><td class="num ${scoreClass(c.pooled_mean)}">${pct(c.pooled_mean)}</td>
      <td class="num">${num(c.opp_n, 1)}</td><td class="num ${scoreClass(c.opp_mean)}">${pct(c.opp_mean)}</td>
      <td class="num ${scoreClass(c.opp_post)}" title="Beta(${num(c.alpha, 2)}, ${num(c.beta, 2)})">${pct(c.opp_post)}</td>
      <td class="num">${c.playable && c.enabled ? pct(c.pick) : "—"}</td><td>${status}</td>
      <td>
        <button class="link" data-act="toggle" data-move="${esc(c.move)}" data-enabled="${c.enabled ? 1 : 0}">${c.enabled ? "disable" : "enable"}</button>
        <select data-act="tier" data-move="${esc(c.move)}" title="Set tier (marks it manual)">
          ${[0, 1, 2, 3].map((t) => `<option value="${t}" ${t === c.tier ? "selected" : ""}>tier ${t}</option>`).join("")}
        </select>
        ${c.source === "manual" ? `<button class="link" data-act="delete" data-move="${esc(c.move)}">delete</button>` : ""}
      </td></tr>`;
  }).join("");
}

function playedRows(pos, opponent, speed) {
  if (!pos.played.length) return '<tr><td colspan="5" class="muted">Never reached in a game.</td></tr>';
  return pos.played.map((p) => `<tr>
    <td><a href="${exploreLink([...pos.moves, p.move], opponent, speed)}">${esc(p.san)}</a></td>
    <td>${p.by_us ? "us" : "opponent"}</td><td class="num">${p.games}</td>
    <td class="num">${opponent ? p.opponent_games : "—"}</td>
    <td class="num ${scoreClass(p.score)}">${pct(p.score)}</td></tr>`).join("");
}

async function renderExplore(params) {
  setActiveNav("explore");
  const moves = (params.get("moves") || "").split(",").filter(Boolean);
  const opponent = params.get("opp") || "";
  const settings = await api("/api/settings");
  const speed = params.get("speed") || "blitz";
  const query = `moves=${encodeURIComponent(moves.join(","))}&opponent=${encodeURIComponent(opponent)}&speed=${encodeURIComponent(speed)}`;
  const seq = routeSeq;
  const pos = await api(`/api/position?${query}`);
  if (seq !== routeSeq) return;
  pos.san_path = pos.san_path ? pos.san_path.split(" ") : [];
  const turn = pos.turn;
  if (explorerState.flipped === null) explorerState.flipped = false;
  const analysis = pos.analysis;
  view.innerHTML = `
    <h1>Explorer</h1>
    <div class="row">
      <label>Opponent <input id="opp" value="${esc(opponent)}" placeholder="(pooled: all opponents)" list="opp-list" autocomplete="off"></label>
      <datalist id="opp-list"></datalist>
      <label>Speed <select id="speed">${settings.speeds.map((s) => `<option ${s === speed ? "selected" : ""}>${s}</option>`).join("")}</select></label>
      <button id="apply">Apply</button>
    </div>
    <div class="explorer">
      <div>
        <div class="board" id="board">${boardHtml(pos, explorerState.flipped)}</div>
        <div class="row" style="margin-top:8px">
          <button id="back" ${moves.length ? "" : "disabled"}>← Back</button>
          <button id="flip">Flip</button>
          <a href="${exploreLink([], opponent, speed)}">Start</a>
          <span class="muted">${esc(turn)} to move · ply ${pos.ply}${pos.in_horizon ? "" : " · beyond book horizon"}</span>
        </div>
        <div class="movelist">${moveListHtml(pos, opponent, speed)}</div>
        <div class="panel">
          <div class="muted">${analysis?.analysed_at
            ? `Analysed depth ${analysis.analysed_depth}, MultiPV ${analysis.analysed_multipv}${analysis.widened ? ", widened" : ""} · ${when(analysis.analysed_at)}`
            : "Not analysed yet"}</div>
          <div class="row" style="margin-top:8px">
            <button id="analyse">Analyse now</button>
            <button id="widen">Widen</button>
          </div>
          <div class="row">
            <select id="add-move">${pos.legal.map((m) => `<option value="${esc(m.move)}">${esc(m.san)}</option>`).join("")}</select>
            <select id="add-tier">${[0, 1, 2, 3].map((t) => `<option value="${t}">tier ${t}</option>`).join("")}</select>
            <button class="primary" id="add">Add candidate</button>
          </div>
        </div>
      </div>
      <div>
        <div class="muted">${opponent ? `Stats for <b>${esc(opponent)}</b> at ${esc(speed)} (other speeds weighted ${settings.settings.other_speed_weight})` : `Pooled view: all opponents at ${esc(speed)}`}
          · tier limit ${pos.tier_limit}${pos.all_exhausted ? ' · <span class="bad">all tiers exhausted</span>' : ""}</div>
        <h2>Candidates</h2>
        <div class="panel"><table id="cand-table">
          <tr><th>Move</th><th class="num">Tier</th><th>Source</th><th class="num">SF</th><th class="num">SF exp.</th>
          <th class="num">Pool n</th><th class="num">Pool</th><th class="num">Opp n</th><th class="num">Opp</th>
          <th class="num">Posterior</th><th class="num">Pick</th><th>Status</th><th></th></tr>
          ${candidateRows(pos, opponent, speed)}
        </table></div>
        <h2>Moves played here</h2>
        <div class="panel"><table>
          <tr><th>Move</th><th>By</th><th class="num">Games</th><th class="num">vs this opp</th><th class="num">Our score</th></tr>
          ${playedRows(pos, opponent, speed)}
        </table></div>
      </div>
    </div>`;
  wireExplorer(pos, moves, opponent, speed);
  api("/api/opponents").then((rows) => {
    document.getElementById("opp-list").innerHTML = rows.map((o) => `<option value="${esc(o.opponent)}">`).join("");
  }).catch(() => {});
}

function wireExplorer(pos, moves, opponent, speed) {
  const go = (newMoves, opp = opponent, spd = speed) => {
    explorerState.selected = null;
    location.hash = exploreLink(newMoves, opp, spd);
  };
  document.getElementById("apply").onclick = () => go(moves, document.getElementById("opp").value.trim().toLowerCase(),
    document.getElementById("speed").value);
  document.getElementById("back").onclick = () => go(moves.slice(0, -1));
  document.getElementById("flip").onclick = () => { explorerState.flipped = !explorerState.flipped; route(); };
  document.getElementById("board").onclick = (event) => {
    const sq = event.target.closest("[data-sq]")?.dataset.sq;
    if (!sq) return;
    const from = explorerState.selected;
    const options = from ? pos.legal.filter((m) => m.move.slice(0, 2) === from && m.move.slice(2, 4) === sq) : [];
    if (options.length) {
      const queen = options.find((m) => m.move.length === 5 && m.move[4] === "q");
      go([...moves, (queen || options[0]).move]);
      return;
    }
    explorerState.selected = pos.legal.some((m) => m.move.slice(0, 2) === sq) ? sq : null;
    document.getElementById("board").innerHTML = boardHtml(pos, explorerState.flipped);
  };
  const post = async (path, body, message) => {
    try {
      await api(path, body);
      toast(message);
      route();
    } catch (error) {
      toast(`Error: ${error.message}`);
    }
  };
  document.getElementById("analyse").onclick = () => post("/api/analyse", { method: "POST", body: { moves, widen: false } }, "Analysis queued");
  document.getElementById("widen").onclick = () => post("/api/analyse", { method: "POST", body: { moves, widen: true } }, "Widen queued");
  document.getElementById("add").onclick = () => post("/api/candidates", {
    method: "POST",
    body: { moves, move: document.getElementById("add-move").value, tier: Number(document.getElementById("add-tier").value) },
  }, "Candidate added; Stockfish will evaluate it");
  document.getElementById("cand-table").onclick = (event) => {
    const btn = event.target.closest("button[data-act]");
    if (!btn) return;
    const move = btn.dataset.move;
    if (btn.dataset.act === "toggle") {
      post("/api/candidates", { method: "PATCH", body: { moves, move, enabled: btn.dataset.enabled !== "1" } }, "Updated");
    } else if (btn.dataset.act === "delete") {
      post("/api/candidates", { method: "DELETE", body: { moves, move } }, "Deleted");
    }
  };
  document.getElementById("cand-table").onchange = (event) => {
    const sel = event.target.closest("select[data-act=tier]");
    if (!sel) return;
    post("/api/candidates", { method: "PATCH", body: { moves, move: sel.dataset.move, tier: Number(sel.value) } }, "Tier set (now manual)");
  };
}

// ---------------------------------------------------------------- queue

async function renderQueue() {
  setActiveNav("queue");
  const seq = routeSeq;
  const data = await api("/api/queue");
  if (seq !== routeSeq) return;
  updateBadge(data.worker);
  view.innerHTML = `
    <h1>Analysis queue</h1>
    <div class="panel"><table>
      <tr><th>Kind</th><th>Target</th><th class="num">Priority</th><th>Status</th><th>Requested</th><th>Finished</th><th>Error</th></tr>
      ${data.jobs.map((j) => `<tr>
        <td>${esc(j.kind)}</td>
        <td>${j.kind === "signal" ? `<a href="https://lichess.org/${esc(j.target)}" target="_blank" rel="noopener">${esc(j.target)}</a>` : `<span title="${esc(j.fen)}">${esc(j.fen ? j.fen.split(" ").slice(0, 2).join(" ") : j.target)}</span>`}</td>
        <td class="num">${j.priority}</td>
        <td class="${j.status === "failed" ? "bad" : j.status === "running" ? "good" : ""}">${esc(j.status)}</td>
        <td>${when(j.requested_at)}</td><td>${when(j.finished_at)}</td><td class="bad">${esc(j.error || "")}</td></tr>`).join("")}
    </table></div>`;
}

// ---------------------------------------------------------------- settings

function calibrationChart(cal, settings) {
  const rows = cal.windows.filter((w) => w.start_moves != null && w.r != null);
  if (!rows.length) {
    return `<p class="muted">Not enough repeated lines yet (${cal.eligible_groups} groups with ≥4 games). Calibration needs several opponent/line groups.</p>`;
  }
  const width = 640;
  const height = 200;
  const barWidth = (width - 40) / rows.length;
  const bars = rows.map((w, i) => {
    const r = Math.max(0, w.r);
    const h = r * (height - 40);
    const current = w.start_moves === settings.window_start_moves && w.end_moves === settings.window_end_moves;
    return `<rect class="bar ${current ? "current" : ""}" x="${30 + i * barWidth + 2}" y="${height - 20 - h}" width="${barWidth - 4}" height="${h}">
      <title>moves ${w.start_moves}–${w.end_moves}: r = ${w.r.toFixed(3)} (${w.groups} groups)</title></rect>
      <text x="${30 + i * barWidth + barWidth / 2}" y="${height - 6}" text-anchor="middle">${w.start_moves}</text>`;
  }).join("");
  return `<svg class="chart" viewBox="0 0 ${width} ${height}" role="img" aria-label="Split-half reliability by window start">
    <line x1="30" y1="${height - 20}" x2="${width}" y2="${height - 20}"></line>
    <text x="0" y="14">r</text>${bars}</svg>
    <p class="muted">Bars: split-half reliability r of the eval signal for a 10-move window starting N moves after book exit (green = current window).
    Result-only baseline r = ${cal.windows[0].r == null ? "—" : cal.windows[0].r.toFixed(3)}.</p>`;
}

async function renderSettings() {
  setActiveNav("settings");
  const seq = routeSeq;
  const [data, cal] = await Promise.all([api("/api/settings"), api("/api/calibration")]);
  if (seq !== routeSeq) return;
  const fields = Object.entries(data.settings).map(([name, value]) => {
    const def = data.defaults[name];
    const input = typeof def === "boolean"
      ? `<input type="checkbox" name="${name}" ${value ? "checked" : ""}>`
      : `<input name="${name}" value="${esc(Array.isArray(value) ? JSON.stringify(value) : value)}">`;
    return `<tr><td>${esc(name)}</td><td>${input}</td><td class="muted">${esc(JSON.stringify(def))}</td></tr>`;
  }).join("");
  view.innerHTML = `
    <h1>Settings</h1>
    <div class="panel"><form id="settings-form"><table>
      <tr><th>Setting</th><th>Value</th><th>Default</th></tr>${fields}
    </table><div class="row" style="margin-top:10px"><button class="primary" type="submit">Save</button></div></form></div>
    <h2>Eval-window calibration</h2>
    <div class="panel">${calibrationChart(cal, data.settings)}</div>`;
  document.getElementById("settings-form").onsubmit = async (event) => {
    event.preventDefault();
    const updates = {};
    for (const el of event.target.elements) {
      if (!el.name) continue;
      updates[el.name] = el.type === "checkbox" ? el.checked : el.value;
    }
    try {
      await api("/api/settings", { method: "PUT", body: updates });
      toast("Settings saved");
    } catch (error) {
      toast(`Error: ${error.message}`);
    }
  };
}

// ---------------------------------------------------------------- router

async function route() {
  clearInterval(refreshTimer);
  routeSeq += 1;
  const { parts, params } = parseHash();
  const page = parts[0] || "overview";
  try {
    if (page === "opponents") await renderOpponents();
    else if (page === "opponent" && parts[1]) await renderOpponent(decodeURIComponent(parts[1]));
    else if (page === "explore") await renderExplore(params);
    else if (page === "queue") {
      await renderQueue();
      refreshTimer = setInterval(() => renderQueue().catch(() => {}), REFRESH_MS);
    } else if (page === "settings") await renderSettings();
    else {
      await renderOverview();
      refreshTimer = setInterval(() => renderOverview().catch(() => {}), REFRESH_MS);
    }
  } catch (error) {
    view.innerHTML = `<p class="bad">Failed to load: ${esc(error.message)}</p>`;
  }
}

window.addEventListener("hashchange", route);
route();
setInterval(() => api("/api/queue").then((d) => updateBadge(d.worker)).catch(() => {}), REFRESH_MS * 2);
