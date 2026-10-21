# syntax=docker/dockerfile:1

# ---- SmikkelBot opening-book server (book/) with Stockfish 19 baked in ----
#
# Build context is this book/ directory (see book/Dockerfile.md). The package is
# copied to /app/book and run as `python -m book serve`; the SQLite database
# lives on the /data volume. Stockfish is the official sf_19 Linux x86-64
# "universal" release (runtime CPU dispatch), SHA256-verified.
FROM python:3.14-slim

ADD --checksum=sha256:9defc0d4e55d49c65a6d042f3e571a39fcea499ade6dbe741b53b8c65e03611f \
    https://github.com/official-stockfish/Stockfish/releases/download/sf_19/stockfish-linux-x86-64-universal.tar.gz \
    /tmp/stockfish.tar.gz
RUN tar -xzf /tmp/stockfish.tar.gz -C /tmp \
    && install -m 0755 /tmp/stockfish/stockfish-linux-x86-64-universal /usr/local/bin/stockfish \
    && rm -rf /tmp/stockfish /tmp/stockfish.tar.gz

WORKDIR /app
COPY requirements.txt /app/book/requirements.txt
RUN python -m pip install --no-cache-dir -r /app/book/requirements.txt
COPY . /app/book

ENV BOOK_DB=/data/book.sqlite \
    BOOK_STOCKFISH=/usr/local/bin/stockfish \
    BOOK_HOST=0.0.0.0 \
    BOOK_PORT=8765 \
    PYTHONUNBUFFERED=1
VOLUME /data
EXPOSE 8765
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/api/settings', timeout=4)"
CMD ["python", "-m", "book", "serve"]
