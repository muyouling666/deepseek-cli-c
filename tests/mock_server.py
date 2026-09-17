#!/usr/bin/env python3
"""Minimal mock of the DeepSeek /chat/completions endpoint for local testing.

It speaks enough of the real API to exercise the C client end to end:

  * requires an "Authorization: Bearer <key>" header
  * validates that the body carries a non-empty messages[] array
  * answers with choices[0].message.content, or SSE chunks when stream=true
  * reports the request's JSON body on stderr so tests can assert on history

Usage:
    python tests/mock_server.py [port]
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
MODEL = "deepseek-chat"
# The only key this mock accepts; tests pass it or deliberately do not.
VALID_KEY = os.environ.get("MOCK_API_KEY", "sk-test-not-a-real-key")


def reply_for(messages):
    """Echo back what the server saw, so multi-turn history is observable."""
    user_turns = [m for m in messages if m.get("role") == "user"]
    last = user_turns[-1]["content"] if user_turns else ""
    system = next((m["content"] for m in messages if m.get("role") == "system"), None)
    prefix = "[sys:%s] " % system if system else ""
    return ("%s你说了 %d 次，最后一次是：%s"
            % (prefix, len(user_turns), last))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("mock: " + (fmt % args) + "\n")
        sys.stderr.flush()

    def _json(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if not self.path.startswith("/chat/completions"):
            self._json(404, {"error": {"message": "unknown path " + self.path,
                                       "type": "invalid_request_error"}})
            return

        auth = self.headers.get("Authorization", "")
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))

        sys.stderr.write("mock: request body: %s\n"
                         % raw.decode("utf-8", "replace"))
        sys.stderr.flush()

        if auth != "Bearer " + VALID_KEY:
            self._json(401, {"error": {"message": "Authentication Fails, Your api key is invalid",
                                       "type": "authentication_error",
                                       "code": "invalid_api_key"}})
            return

        try:
            body = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            self._json(400, {"error": {"message": "invalid JSON: %s" % exc,
                                       "type": "invalid_request_error"}})
            return

        messages = body.get("messages")
        if not isinstance(messages, list) or not messages:
            self._json(400, {"error": {"message": "messages must be a non-empty array",
                                       "type": "invalid_request_error"}})
            return

        answer = reply_for(messages)
        model = body.get("model", MODEL)

        if body.get("stream"):
            self._stream(model, answer)
        else:
            self._json(200, {
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "created": 1700000000,
                "model": model,
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": answer},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 11, "completion_tokens": 22,
                          "total_tokens": 33},
            })

    def _stream(self, model, answer):
        base = {"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                "created": 1700000000, "model": model}
        events = []

        # A few characters per event, so the client has to reassemble the deltas
        # and cannot rely on one chunk per character.
        for i in range(0, len(answer), 3):
            chunk = dict(base)
            chunk["choices"] = [{"index": 0,
                                 "delta": {"content": answer[i:i + 3]},
                                 "finish_reason": None}]
            events.append("data: " + json.dumps(chunk, ensure_ascii=False) + "\n\n")

        final = dict(base)
        final["choices"] = [{"index": 0, "delta": {}, "finish_reason": "stop"}]
        final["usage"] = {"prompt_tokens": 11, "completion_tokens": 22,
                          "total_tokens": 33}
        events.append("data: " + json.dumps(final, ensure_ascii=False) + "\n\n")
        events.append("data: [DONE]\n\n")

        payload = "".join(events).encode("utf-8")

        # Content-Length framing keeps the connection reusable and lets the
        # handler close cleanly; the client still sees the deltas split across
        # several SSE events.
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
        self.wfile.flush()


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    sys.stderr.write("mock DeepSeek API listening on http://127.0.0.1:%d\n" % PORT)
    sys.stderr.flush()
    server.serve_forever()
