#!/usr/bin/env python3
"""End-to-end tests for the DeepSeek CLI client.

Starts tests/mock_server.py (which speaks enough of the DeepSeek API to be
useful), runs the compiled client against it and checks the observable
behaviour: the payload shape, the parsed reply, multi-turn history, session
persistence, streaming reassembly and the error paths.

Usage:
    python tests/run_tests.py [--exe path/to/ai]
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# The Makefile names the binary "ai" everywhere, plus ".exe" on Windows.
EXE = os.path.join(ROOT, "ai.exe" if os.name == "nt" else "ai")
if not os.path.exists(EXE):
    EXE = os.path.join(ROOT, "ai")
MOCK = os.path.join(HERE, "mock_server.py")
FAKE_KEY = "sk-test-not-a-real-key"

results = []


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for_port(port, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


class Mock:
    """The mock API server, with the request bodies it received."""

    def __init__(self, port):
        self.port = port
        self.log = ""
        env = dict(os.environ)
        env["PYTHONUNBUFFERED"] = "1"
        env["PYTHONIOENCODING"] = "utf-8"
        self.proc = subprocess.Popen(
            [sys.executable, MOCK, str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
            cwd=ROOT, text=True, encoding="utf-8", errors="replace")

    def url(self):
        return "http://127.0.0.1:%d" % self.port

    def stop(self):
        """Terminate the server and capture its stderr (idempotent)."""
        if self.log:
            return self.log
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        _, err = self.proc.communicate(timeout=5)
        self.log = err or ""
        return self.log


def run_client(args, mock=None, key=FAKE_KEY, base_url=None, stdin=None,
               timeout=60, cwd=None):
    env = dict(os.environ)
    env.pop("DEEPSEEK_API_KEY", None)
    env.pop("DEEPSEEK_BASE_URL", None)
    env.pop("DEEPSEEK_MODEL", None)
    # The mock server is a Python child too; force UTF-8 so its log lines are
    # decodable regardless of the console code page.
    env["PYTHONIOENCODING"] = "utf-8"
    if key is not None:
        env["DEEPSEEK_API_KEY"] = key
    if base_url is None and mock is not None:
        base_url = mock.url()
    if base_url:
        env["DEEPSEEK_BASE_URL"] = base_url
    cmd = [EXE] + list(args)
    proc = subprocess.run(cmd, input=stdin, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", env=env,
                          timeout=timeout, cwd=cwd or ROOT)
    return proc


def requests_from(stderr_text):
    """Extract the JSON bodies the mock server logged."""
    bodies = []
    for line in stderr_text.splitlines():
        marker = "mock: request body: "
        if marker in line:
            raw = line.split(marker, 1)[1]
            try:
                bodies.append(json.loads(raw))
            except json.JSONDecodeError:
                pass
    return bodies


def case(name):
    def wrap(fn):
        results.append((name, fn))
        return fn
    return wrap


# ------------------------------------------------------------------ #
# test cases                                                          #
# ------------------------------------------------------------------ #

@case("one-shot question returns the parsed content")
def test_one_shot(mock):
    proc = run_client(["你好"], mock)
    assert proc.returncode == 0, "exit=%d stderr=%s" % (proc.returncode, proc.stderr)
    assert "你说了 1 次" in proc.stdout, proc.stdout
    assert "最后一次是：你好" in proc.stdout, proc.stdout
    bodies = requests_from(mock.stop())
    assert len(bodies) == 1, bodies
    body = bodies[0]
    assert body["model"] == "deepseek-chat", body
    assert body["messages"] == [{"role": "user", "content": "你好"}], body
    assert "stream" not in body, body
    return "payload: model=%s, messages=%d, reply parsed" % (
        body["model"], len(body["messages"]))


@case("interactive mode keeps history across turns")
def test_multi_turn(mock):
    stdin = "第一句\n第二句\n/exit\n"
    proc = run_client([], mock, stdin=stdin)
    assert proc.returncode == 0, proc.stderr
    assert "你说了 1 次" in proc.stdout, proc.stdout
    assert "你说了 2 次" in proc.stdout, proc.stdout
    bodies = requests_from(mock.stop())
    assert len(bodies) == 2, "expected 2 requests, got %d" % len(bodies)
    first, second = bodies
    assert len(first["messages"]) == 1, first
    # The second call must replay the whole dialogue: user, assistant, user.
    roles = [m["role"] for m in second["messages"]]
    assert roles == ["user", "assistant", "user"], roles
    assert second["messages"][0]["content"] == "第一句", second
    assert second["messages"][2]["content"] == "第二句", second
    return "2nd request roles=%s (history replayed)" % roles


@case("--session persists and resumes the history")
def test_session(mock):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "session.json")

        proc = run_client(["--session", path, "hello"], mock)
        assert proc.returncode == 0, proc.stderr
        assert os.path.exists(path), "session file was not written"
        saved = json.load(open(path, encoding="utf-8"))
        roles = [m["role"] for m in saved["messages"]]
        assert roles == ["user", "assistant"], roles
        mock.stop()

        # A second, independent run must resume the stored history and send it
        # along with the new turn.
        port = free_port()
        mock2 = Mock(port)
        assert wait_for_port(port), "second mock server did not start"
        proc2 = run_client(["--session", path, "again"], mock2)
        mock2.stop()
        assert proc2.returncode == 0, proc2.stderr
        assert "resumed 2 messages" in proc2.stderr, proc2.stderr
        bodies = requests_from(mock2.log)
        assert len(bodies) == 1, bodies
        roles2 = [m["role"] for m in bodies[0]["messages"]]
        assert roles2 == ["user", "assistant", "user"], roles2
        return "saved roles=%s, resumed roles=%s" % (roles, roles2)


@case("--system prompt leads the message array")
def test_system(mock):
    proc = run_client(["--system", "你是助手", "hi"], mock)
    assert proc.returncode == 0, proc.stderr
    bodies = requests_from(mock.stop())
    roles = [m["role"] for m in bodies[0]["messages"]]
    assert roles == ["system", "user"], roles
    assert bodies[0]["messages"][0]["content"] == "你是助手"
    assert "[sys:你是助手]" in proc.stdout, proc.stdout
    return "roles=%s" % roles


@case("--stream reassembles the SSE deltas")
def test_stream(mock):
    proc = run_client(["--stream", "stream me"], mock)
    assert proc.returncode == 0, proc.stderr
    assert "你说了 1 次" in proc.stdout, proc.stdout
    assert "最后一次是：stream me" in proc.stdout, proc.stdout
    bodies = requests_from(mock.stop())
    assert bodies[0].get("stream") is True, bodies[0]
    # The reply must not be duplicated by the streaming printer.
    assert proc.stdout.count("你说了") == 1, proc.stdout
    return "stream=true, %d bytes of stdout, deltas reassembled" % len(proc.stdout)


@case("--stream keeps the turn in the history")
def test_stream_history(mock):
    proc = run_client(["--stream"], mock, stdin="alpha\nbeta\n/exit\n")
    assert proc.returncode == 0, proc.stderr
    bodies = requests_from(mock.stop())
    assert len(bodies) == 2, bodies
    roles = [m["role"] for m in bodies[1]["messages"]]
    assert roles == ["user", "assistant", "user"], roles
    assert "你说了 1 次" in bodies[1]["messages"][1]["content"], bodies[1]
    return "streamed assistant turn stored: %r" % bodies[1]["messages"][1]["content"][:40]


@case("missing API key is reported, not crashed on")
def test_missing_key(mock):
    proc = run_client(["hi"], key=None, base_url="http://127.0.0.1:1")
    assert proc.returncode != 0, "should fail without a key"
    assert "DEEPSEEK_API_KEY" in proc.stderr, proc.stderr
    return "exit=%d, message mentions the env var" % proc.returncode


@case("HTTP 401 surfaces the API error object")
def test_auth_error(mock):
    # A bogus (but non-empty) key: the mock rejects it with a DeepSeek style
    # 401 body, which the client has to decode rather than dump as raw JSON.
    proc = run_client(["hi"], mock, key="sk-wrong-key")
    assert proc.returncode != 0, proc.stdout
    assert "401" in proc.stderr, proc.stderr
    assert "Authentication Fails" in proc.stderr, proc.stderr
    assert "invalid_api_key" in proc.stderr, proc.stderr
    mock.stop()
    return "401 body decoded: %s" % proc.stderr.strip().splitlines()[1]


@case("--base-url without a key reports the server's own error")
def test_no_key_custom_endpoint(mock):
    # With an explicit --base-url the client must not abort locally: a local
    # server may not need a key at all, so it sends the request and reports
    # whatever comes back.
    proc = run_client(["--base-url", mock.url(), "hi"], key=None)
    assert proc.returncode != 0, proc.stdout
    assert "warning" in proc.stderr, proc.stderr
    assert "Authentication Fails" in proc.stderr, proc.stderr
    mock.stop()
    return "sent without a key, server error surfaced"


@case("unreachable endpoint fails cleanly")
def test_unreachable(mock):
    mock.stop()
    proc = run_client(["hi"], base_url="http://127.0.0.1:9", timeout=120)
    assert proc.returncode != 0, "should fail"
    assert "error" in proc.stderr.lower(), proc.stderr
    return "exit=%d: %s" % (proc.returncode, proc.stderr.strip().splitlines()[0][:80])


@case("--model / --temperature / --max-tokens reach the payload")
def test_options(mock):
    proc = run_client(["-m", "deepseek-reasoner", "-t", "0.25", "-k", "64", "hi"], mock)
    assert proc.returncode == 0, proc.stderr
    body = requests_from(mock.stop())[0]
    assert body["model"] == "deepseek-reasoner", body
    assert abs(body["temperature"] - 0.25) < 1e-9, body
    assert body["max_tokens"] == 64, body
    return "model=%s temperature=%s max_tokens=%s" % (
        body["model"], body["temperature"], body["max_tokens"])


@case("non-ASCII prompts survive the command line encoding")
def test_utf8_argv(mock):
    # On Windows the console hands the program code page encoded bytes; the
    # client must convert them to UTF-8 before they reach the JSON body.
    text = "你好，世界 🌏"
    proc = run_client([text], mock)
    assert proc.returncode == 0, proc.stderr
    bodies = requests_from(mock.stop())
    assert bodies, "no request logged"
    content = bodies[0]["messages"][0]["content"]
    assert content == text, "sent %r, API received %r" % (text, content)
    return "round-tripped %r intact" % content


@case("a pasted shell command is caught, not sent to the model")
def test_paste_guard(mock):
    # Pasting `.\ai.exe "你好"` into the chat prompt used to be sent verbatim,
    # which the API rejects as an invalid JSON body.
    stdin = '.\\ai.exe "hello"\nreal question\n/exit\n'
    proc = run_client([], mock, stdin=stdin)
    assert proc.returncode == 0, proc.stderr
    assert "not a shell" in proc.stdout, proc.stdout
    assert "400" not in proc.stderr, proc.stderr
    bodies = requests_from(mock.stop())
    assert len(bodies) == 1, "the pasted command was sent to the API: %r" % bodies
    assert bodies[0]["messages"] == [{"role": "user", "content": "real question"}], bodies
    return "pasted command ignored, only the real question sent"


@case("--help documents the interface")
def test_help(mock):
    mock.stop()
    proc = run_client(["--help"])
    assert proc.returncode == 0, proc.stderr
    for expected in ["DEEPSEEK_API_KEY", "--session", "--stream", "/reset"]:
        assert expected in proc.stdout, "%s missing from help" % expected
    return "help lists the env var and the main flags"


# ------------------------------------------------------------------ #

def main():
    global EXE

    # Console code pages (GBK, cp1252, ...) cannot represent the Chinese text
    # used by these tests, which would break the report rather than the test.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    EXE = os.path.abspath(args.exe)
    if not os.path.exists(EXE):
        print("error: client not built: %s (run make first)" % EXE)
        return 2

    print("client : %s" % EXE)
    print("mock   : %s" % MOCK)
    print()

    passed = failed = 0
    for name, fn in results:
        port = free_port()
        mock = Mock(port)
        if not wait_for_port(port):
            print("FAIL  %s: mock server did not start" % name)
            failed += 1
            mock.stop()
            continue
        try:
            detail = fn(mock)
            print("PASS  %s\n        %s" % (name, detail))
            passed += 1
        except AssertionError as exc:
            print("FAIL  %s\n        %s" % (name, exc))
            failed += 1
        except Exception as exc:                        # noqa: BLE001
            print("ERROR %s\n        %s: %s" % (name, type(exc).__name__, exc))
            failed += 1
        finally:
            try:
                mock.stop()
            except Exception:                            # noqa: BLE001
                pass

    print("\n%d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
