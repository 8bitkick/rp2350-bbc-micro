#!/usr/bin/env python3
"""Work on the web control panel without reflashing: serves src/boards/presto/web/index.html from
disk (read afresh on every request) and passes /api/... through to the Presto.

  tools/webdev.py [presto address, default beeb.local] [port, default 8080]

then open http://<this machine>:8080/ and refresh after each edit.
"""
import http.server
import socketserver
import sys
import urllib.error
import urllib.request
from pathlib import Path

PAGE = Path(__file__).resolve().parent.parent / 'src/boards/presto/web/index.html'
PRESTO = sys.argv[1] if len(sys.argv) > 1 else 'beeb.local'
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080


class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, code, ctype, body):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def proxy(self, method):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n) if n else None
        req = urllib.request.Request(f'http://{PRESTO}{self.path}', data=body, method=method)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                self.reply(r.status, r.headers.get('Content-Type', 'application/json'), r.read())
        except urllib.error.HTTPError as e:
            self.reply(e.code, e.headers.get('Content-Type', 'application/json'), e.read())
        except OSError as e:
            self.reply(502, 'application/json', ('{"error":"can\'t reach the Presto: %s"}' % e).encode())

    def do_GET(self):
        if self.path.startswith('/api/'):
            self.proxy('GET')
        elif self.path in ('/', '/index.html'):
            self.reply(200, 'text/html; charset=utf-8', PAGE.read_bytes())
        else:
            self.reply(404, 'text/plain', b'not found')

    def do_POST(self):
        self.proxy('POST')

    def log_message(self, fmt, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == '__main__':
    print(f'serving {PAGE} on port {PORT}, API from {PRESTO}')
    Server(('', PORT), Handler).serve_forever()
