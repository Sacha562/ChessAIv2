"""Minimal client for the lichess game-export API.

Streams a user's finished games as NDJSON, oldest first, starting after a given
timestamp. No token is needed for public games; set ``LICHESS_TOKEN`` to get the
higher authenticated rate limit. See [lichess_api.py.md](lichess_api.py.md).
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Iterator

API_ROOT = "https://lichess.org"
EXPORT_TIMEOUT_S = 60.0


class RateLimited(Exception):
    """lichess answered HTTP 429; the caller must wait a full minute."""


def export_games(username: str, since_ms: int) -> Iterator[dict]:
    """Yield finished games of ``username`` created after ``since_ms``, oldest first.

    Each item is the decoded NDJSON object lichess returns (``id``, ``speed``,
    ``players``, ``moves`` in SAN, ``winner``, ``status``, ``createdAt``, ...).
    Raises RateLimited on HTTP 429 and urllib.error.URLError on network failure.
    """
    query = urllib.parse.urlencode({
        "since": since_ms, "moves": "true", "tags": "false", "clocks": "false",
        "evals": "false", "opening": "false", "finished": "true", "sort": "dateAsc",
    })
    url = f"{API_ROOT}/api/games/user/{urllib.parse.quote(username)}?{query}"
    headers = {"Accept": "application/x-ndjson", "User-Agent": "SmikkelBot-book"}
    token = os.environ.get("LICHESS_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    try:
        response = urllib.request.urlopen(request, timeout=EXPORT_TIMEOUT_S)
    except urllib.error.HTTPError as error:
        if error.code == 429:
            raise RateLimited() from error
        raise
    with response:
        for raw in response:
            line = raw.strip()
            if line:
                yield json.loads(line)
