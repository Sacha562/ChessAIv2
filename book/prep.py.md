# Module: `book.prep`

Per-opponent **prepared moves** ("prep"): hand-set overrides that say "against
this opponent, in this exact position, always play this move". They are meant
for opponents with a known, fixed repertoire. Bongaclang, for example, always
answers with 1...e5 2...Ke7 as Black and opens 1.e4 2.Ke2 as White, so we can
steer straight into the line that punishes it hardest.

Prep has the highest precedence in [`selector.choose`](selector.py.md#choose):
**prep > replay > book > engine**. It applies at every speed and at any ply
(the book horizon does not limit it), but only in the stored position (matched
by Polyglot key, so transpositions count). As soon as the opponent deviates,
the normal replay/book/engine logic takes over. A prep move is logged in
`book_events` with source `prep`, which counts as a served move for the book
exit ([`exit_ply`](ingest.py.md#exit_ply)).

Prep is managed from the dashboard ([static/app.js.md](static/app.js.md#explorer))
or the API ([server.py.md](server.py.md#dashboard-endpoints)).

## Source File

- **Code:** [prep.py](prep.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md) (`PrepTests`, `ServerTests`)

## Objects / Interfaces

Table `prep_moves` ([db.py.md](db.py.md#schema-schema)), primary key
(`opponent`, `key`):

| Column | Meaning |
|--------|---------|
| `opponent` | Lowercase lichess id. |
| `key` | Polyglot key of the position (signed 64-bit). |
| `moves` | Space-separated UCI path from the start position to it (for display and editing). |
| `move` | The prepared UCI move. |
| `note` | Optional free text. |
| `created_at` | Unix seconds. |

## Functions

### `prep_move`

**Returns:** the prepared UCI move for (`opponent`, `key`), or `None`.

### `set_prep`

Stores or replaces the prep for the position after `moves`.

**Warnings:** raises `ValueError` if the path or the move is illegal. The
caller commits.

### `set_prep_line`

Prepares every move of `color` in a full line from the start position. For
example, `e4 e5 f4 Ke7` as White prepares `e4` at the start and `f4` after
`1.e4 e5`. The whole line is validated before anything is written.

**Returns:** the number of moves prepared. The caller commits.

### `remove_prep`

Deletes the prep for the position after `moves`.

**Returns:** `True` if an entry existed. The caller commits.

### `list_prep`

Lists prep entries, all of them or one opponent's, ordered by opponent and then
depth. Each entry has `opponent`, `moves` (list), `move`, `note`, `created_at`
and `san`. `san` is the numbered SAN of the path plus the prepared move, e.g.
`1.e4 d5 2.Ke2 dxe4`.

### `_board` / `_san_path`

Helpers. `_board` replays and validates a UCI path; `_san_path` renders the
numbered SAN, using `1...` when the first rendered move is Black's.
