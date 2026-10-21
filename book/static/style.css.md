# Frontend: `style.css`

Styles for the dashboard ([index.html](index.html.md), [app.js](app.js.md)).
Plain CSS, no framework.

## Source File

- **Stylesheet:** [style.css](style.css)

## Design tokens

Colors are custom properties on `:root`, redefined under
`@media (prefers-color-scheme: dark)`, so the page follows the OS theme:

| Token | Use |
|-------|-----|
| `--bg`, `--surface`, `--border`, `--text`, `--muted` | Page, panels, rules, text. |
| `--accent` | Links, primary button, chart bars. |
| `--good` / `--bad` / `--warn` | Scores ≥ 55 % / < 45 %, failures, locked or missed states. |
| `--square-light`, `--square-dark`, `--square-selected`, `--square-last` | Board squares, selected square, last-move highlight. |
| `--radius` | Corner radius of tiles and panels. |

## Components

| Selector | What it styles |
|----------|----------------|
| `.topbar`, `.badge` (`.analysing` / `.paused` / `.error`) | Sticky header and worker status pill. |
| `.tiles`, `.tile` | Overview stat tiles (auto-fill grid). |
| `.panel`, `table`, `td.num`, `tr.dim` | Bordered sections; tabular-number tables; dimmed disabled candidates. |
| `.chip` | Inline per-speed summaries and the BOT tag. |
| `.explorer` | Two-column explorer layout (board up to 440 px + tables); one column below 900 px. Both use `minmax(0, …)` so wide tables scroll inside their panel instead of squeezing the board. |
| `.board`, `.sq` (`.light` / `.dark` / `.last` / `.selected` / `.target`), `.piece.w` / `.piece.b`, `.coord` | The 8×8 board: square grid with `aspect-ratio: 1`; Unicode piece glyphs (white pieces get an outline shadow); dots on legal target squares. |
| `.movelist` | Clickable SAN path. |
| `.toast` | Bottom-right message. |
| `.chart` | Calibration bar chart (current window in `--good`). |

Below 600 px the main padding drops to a 16 px gutter and the header wraps.
