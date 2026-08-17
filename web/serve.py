#!/usr/bin/env python3
"""Serve this directory for local development.

    ./serve.py [port]          default 8000, then open http://localhost:8000/

The page cannot be opened as a file:// URL -- it starts a Worker and fetches
the disk images -- so it needs some server, and this is the smallest one that
gets the details right:

  * .wasm as application/wasm, without which the browser refuses the streaming
    compile and falls back (or, in stricter builds, fails outright);
  * no-store on everything, so a rebuild is picked up on reload instead of
    serving yesterday's nx88.wasm against today's nx88.js;
  * Range requests, which Python's stock handler does not do, so the 7 MB tape
    image is resumable and behaves under a throttled connection.

Nothing here is required in production.  The built page is plain static files:
any static host will do, and there are no COOP/COEP headers to arrange because
the emulator uses ASYNCIFY rather than SharedArrayBuffer (see emu/console.c).
"""
import http.server
import os
import re
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def send_head(self):
        """Same as the base class, plus single-range support."""
        rng = self.headers.get("Range")
        if not rng:
            return super().send_head()
        m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
        path = self.translate_path(self.path)
        if not m or os.path.isdir(path):
            return super().send_head()
        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(404, "File not found")
            return None
        size = os.fstat(f.fileno()).st_size
        start, end = m.group(1), m.group(2)
        if start == "":                      # bytes=-N -> the last N bytes
            start, end = max(0, size - int(end or 0)), size - 1
        else:
            start = int(start)
            end = int(end) if end else size - 1
        end = min(end, size - 1)
        if start > end or start >= size:
            f.close()
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return None
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        return _Slice(f, end - start + 1)


class _Slice:
    """A read-only view of the first n bytes of f, for copyfile()."""

    def __init__(self, f, n):
        self.f, self.n = f, n

    def read(self, size=-1):
        if self.n <= 0:
            return b""
        if size is None or size < 0:
            size = self.n
        data = self.f.read(min(size, self.n))
        self.n -= len(data)
        return data

    def close(self):
        self.f.close()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.exists("nx88.wasm"):
        print("nx88.wasm is missing -- run ./build.sh first", file=sys.stderr)
    with Server(("127.0.0.1", PORT), Handler) as httpd:
        print(f"serving {os.getcwd()} on http://localhost:{PORT}/  (Ctrl-C to stop)")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print()
