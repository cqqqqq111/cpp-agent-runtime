#!/usr/bin/env python3
"""
serve.py — stdlib-only trace viewer server (Day 22).

Serves the web/index.html trace panel, the logs/*.json traces produced by the
C++ demos/agent, and a Server-Sent Events (SSE) channel that notifies the
browser the moment a new trace file appears. Zero third-party dependencies —
only the Python standard library (http.server + threading + glob).

Why SSE instead of WebSocket:
  WebSocket needs the `websockets` package (not installed here) or a hand-rolled
  RFC 6455 handshake + frame codec. SSE is a plain HTTP response with
  `Content-Type: text/event-stream` and can be served with http.server as-is,
  which keeps the whole panel runnable with `python web/serve.py` and nothing
  else. SSE is one-directional (server → browser), which is exactly what a
  live trace feed needs.

Usage:
    python web/serve.py            # default: 0.0.0.0:8000
    python web/serve.py 9001       # custom port

Endpoints:
    GET /                  → 302 to /web/index.html
    GET /api/traces        → {"traces": [{"name","size","mtime"}, ...]}
    GET /api/trace/<name>  → the trace JSON (or 404)
    GET /api/events        → SSE stream; emits "trace-added"/"trace-changed"
    GET /logs/<name>       → static trace file (also served by the file handler)
"""

import argparse
import glob
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS_DIR = os.path.join(ROOT, "logs")
WEB_DIR = os.path.join(ROOT, "web")


def list_traces():
    """Return trace metadata for every logs/*.json, newest first."""
    traces = []
    for path in glob.glob(os.path.join(LOGS_DIR, "*.json")):
        try:
            st = os.stat(path)
        except OSError:
            continue
        traces.append({
            "name": os.path.basename(path),
            "size": st.st_size,
            "mtime": st.st_mtime,
        })
    traces.sort(key=lambda t: t["mtime"], reverse=True)
    return traces


def trace_snapshot():
    """Fingerprint of the current logs/ directory for change detection."""
    sig = []
    for t in list_traces():
        sig.append((t["name"], t["size"], t["mtime"]))
    return sig


class Handler(BaseHTTPRequestHandler):
    server_version = "cpp-agent-trace/1.0"

    # ---- helpers ---------------------------------------------------------
    def _send(self, status, body, content_type="application/json; charset=utf-8"):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, status=200):
        self._send(status, json.dumps(obj, ensure_ascii=False, indent=2))

    # ---- routing ---------------------------------------------------------
    def do_GET(self):
        path = self.path.split("?", 1)[0]

        if path == "/":
            self.send_response(302)
            self.send_header("Location", "/web/index.html")
            self.end_headers()
            return

        if path == "/api/traces":
            return self._json({"traces": list_traces()})

        if path.startswith("/api/trace/"):
            name = os.path.basename(path[len("/api/trace/"):])
            return self._serve_trace(name)

        if path == "/api/events":
            return self._serve_events()

        return self._serve_static(path)

    # ---- trace file ------------------------------------------------------
    def _serve_trace(self, name):
        fp = os.path.join(LOGS_DIR, name)
        if not os.path.isfile(fp):
            return self._json({"error": "trace not found"}, 404)
        with open(fp, "rb") as f:
            return self._send(200, f.read(),
                              "application/json; charset=utf-8")

    # ---- static files ----------------------------------------------------
    def _serve_static(self, path):
        # Resolve within ROOT, guarding against path traversal.
        if path.startswith("/logs/"):
            fp = os.path.join(LOGS_DIR, os.path.basename(path))
        elif path.startswith("/web/"):
            fp = os.path.join(WEB_DIR, os.path.basename(path))
        else:
            self._json({"error": "not found"}, 404)
            return

        if not os.path.isfile(fp):
            self._json({"error": "not found"}, 404)
            return

        ctype = "text/html; charset=utf-8"
        if fp.endswith(".json"):
            ctype = "application/json; charset=utf-8"
        elif fp.endswith(".css"):
            ctype = "text/css; charset=utf-8"
        elif fp.endswith(".js"):
            ctype = "application/javascript; charset=utf-8"
        with open(fp, "rb") as f:
            self._send(200, f.read(), ctype)

    # ---- SSE -------------------------------------------------------------
    def _serve_events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

        last = trace_snapshot()
        try:
            while True:
                cur = trace_snapshot()
                if cur != last:
                    # Report the newest trace and let the browser re-fetch the list.
                    newest = list_traces()[0]["name"] if list_traces() else ""
                    event = json.dumps({"type": "changed", "newest": newest})
                    self.wfile.write(f"data: {event}\n\n".encode("utf-8"))
                    self.wfile.flush()
                    last = cur
                time.sleep(1.0)
        except (BrokenPipeError, ConnectionResetError):
            return

    def log_message(self, fmt, *args):  # quieter logs
        pass


def main():
    ap = argparse.ArgumentParser(description="Trace viewer server (stdlib only)")
    ap.add_argument("port", nargs="?", type=int, default=8000)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    os.makedirs(LOGS_DIR, exist_ok=True)
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"cpp-agent trace viewer → http://{args.host}:{args.port}")
    print(f"  traces: {len(list_traces())} in {LOGS_DIR}")
    print("  Ctrl+C to stop")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
        srv.server_close()


if __name__ == "__main__":
    main()
