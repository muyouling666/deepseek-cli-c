"""Check what the client actually sends for a non-ASCII prompt.

Writes the mock server's log to a file so the result can be inspected without
going through a console code page.

Usage: python tests/check_encoding.py
"""
import os
import re
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "ai.exe" if os.name == "nt" else "ai")
TEXT = "你好，世界 🌏"
LOG = os.path.join(ROOT, "build", "encoding-check.log")

received = {}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        received["raw"] = raw
        body = b'{"choices":[{"index":0,"message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}]}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    time.sleep(0.3)

    env = dict(os.environ)
    env["DEEPSEEK_API_KEY"] = "sk-test"
    env["DEEPSEEK_BASE_URL"] = "http://127.0.0.1:%d" % port

    proc = subprocess.run([EXE, TEXT], capture_output=True, env=env, cwd=ROOT)
    server.shutdown()

    raw = received.get("raw", b"")
    lines = []
    lines.append("exit code      : %d" % proc.returncode)
    lines.append("console bytes  : %r" % TEXT.encode("gbk", "replace"))
    lines.append("raw body bytes : %r" % raw)
    try:
        decoded = raw.decode("utf-8")
        lines.append("decoded as utf-8: OK")
    except UnicodeDecodeError as exc:
        decoded = raw.decode("utf-8", "replace")
        lines.append("decoded as utf-8: FAILED (%s)" % exc)
    lines.append("body           : %s" % decoded)

    match = re.search(r'"content":"(.*?)"', decoded)
    sent = match.group(1) if match else None
    ok = sent == TEXT
    lines.append("expected       : %s" % TEXT)
    lines.append("sent           : %s" % sent)
    lines.append("RESULT         : %s" % ("PASS" if ok else "FAIL"))

    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines).encode("ascii", "backslashreplace").decode("ascii"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
