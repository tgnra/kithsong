"""Local preview: serve dist/ and rebuild when content, docs, assets or config change."""

import functools
import http.server
import threading
import time
import traceback
from pathlib import Path

from .build import build

WATCH = ("content", "docs", "static", "assets", "site.toml")


def _snapshot(root):
    stamps = {}
    for name in WATCH:
        p = root / name
        for f in [p] if p.is_file() else p.rglob("*") if p.is_dir() else []:
            stamps[f] = f.stat().st_mtime_ns
    return stamps


def _rebuild(root, drafts, base_url):
    try:
        build(root, drafts=drafts, base_url=base_url)
    except Exception:
        traceback.print_exc()


def _watch(root, drafts, base_url):
    last = _snapshot(root)
    while True:
        time.sleep(0.5)
        now = _snapshot(root)
        if now != last:
            last = now
            _rebuild(root, drafts, base_url)


class Handler(http.server.SimpleHTTPRequestHandler):
    def send_error(self, code, message=None, explain=None):
        page = Path(self.directory) / "404.html"
        if code == 404 and page.exists():
            body = page.read_bytes()
            self.send_response(404)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            super().send_error(code, message, explain)

    def log_message(self, fmt, *args):
        pass


def serve(root=".", port=8000, drafts=False, base_url=None):
    root = Path(root).resolve()
    _rebuild(root, drafts, base_url)
    threading.Thread(target=_watch, args=(root, drafts, base_url), daemon=True).start()
    handler = functools.partial(Handler, directory=str(root / "dist"))
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as httpd:
        print(f"serving http://localhost:{port}/ (ctrl+c to stop)")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
