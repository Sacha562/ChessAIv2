# Frontend: `index.html`

The dashboard's only HTML page, served at `/` by [server.py](../server.py.md#dashboard-endpoints).
It is a static shell; everything inside `<main>` is rendered by
[app.js](app.js.md) from the JSON API.

## Source File

- **Page:** [index.html](index.html)

## Structure

| Element | Purpose |
|---------|---------|
| `header.topbar` | Brand, navigation links (`#/overview`, `#/opponents`, `#/explore`, `#/queue`, `#/settings`; each has `data-nav` so [app.js](app.js.md#helpers) can highlight the active one), and `#worker-badge`, the live worker state. |
| `main#view` | Mount point for the current view (`aria-live="polite"`). |
| `#toast` | Transient status messages ([`toast`](app.js.md#helpers)). |

Loads [style.css](style.css.md) and [app.js](app.js.md) from `/static/`. There
are no external scripts, fonts or CDNs, so the dashboard works offline.

**Note:** the repo's `.gitignore` ignores `*.html` (self-play viewers);
`!/book/static/*.html` re-includes this file.
