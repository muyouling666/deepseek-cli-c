# ai — a DeepSeek command line client in C

[中文说明](README.md) | English

A small, dependency-light CLI chat client for the DeepSeek API, written in C99.
Ask a question on the command line, get an answer in the terminal, or keep an
interactive multi-turn session going.

```
$ export DEEPSEEK_API_KEY=sk-xxxxxxxx
$ make
$ ./ai "hello"
Hi! How can I help you today?

$ ./ai
DeepSeek CLI - model deepseek-chat
Type a message and press Enter. /help for commands, /exit to quit.

you> explain recursion in one sentence
ai > Recursion is when a function calls itself to solve a smaller version...
you> give me an example
ai > For instance, computing a factorial: n! = n * (n-1)! ...
```

## What it does

- **One-shot questions**: `./ai "your question"`
- **Multi-turn chat**: run with no argument; the whole history is replayed on
  every request, so the model sees the full context
- **Streaming** (`--stream`): parses the SSE response and prints deltas as they
  arrive, then stores the assembled reply in the history
- **Resumable sessions** (`--session file.json`): history survives across runs
- HTTP via **libcurl**, JSON via **cJSON** (vendored), built with a **Makefile**
- API key from the `DEEPSEEK_API_KEY` environment variable

## Layout

```
Makefile                      build rules (all / deps / setup / test / clean)
src/main.c                    argument parsing, request flow, REPL, streaming
src/http.c                    libcurl wrapper: one POST, response in memory
src/conversation.c            multi-turn history, request payload, session I/O
src/console.c                 Windows Unicode console I/O (see below)
src/util.c                    growable buffer, libcurl sink, string helpers
vendor/cJSON.{c,h}            cJSON 1.7.18
tests/mock_server.py          local fake DeepSeek API (offline tests)
tests/run_tests.py            14 end-to-end tests
scripts/                      dependency bootstrap (POSIX shell and PowerShell)
```

## Build

### Linux / macOS

```sh
sudo apt install build-essential libcurl4-openssl-dev   # Debian/Ubuntu
brew install curl                                       # macOS
make
```

### Windows

With MSYS2 (MinGW-w64 toolchain and libcurl):

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-curl make
make
```

With only gcc and no POSIX shell, use the PowerShell bootstrap, which compiles
cJSON and a static libcurl (schannel TLS backend, no OpenSSL needed) into
`build/` and writes `build/local.mk` that the Makefile picks up:

```powershell
powershell -File scripts\Setup-Build.ps1 -Gcc C:\mingw64\bin\gcc.exe -CurlSource C:\src\curl-8.11.1
```

### No libcurl installed at all

```sh
make deps && make
```

`make deps` builds cJSON and libcurl from source into `build/`.

## Options

| Option | Description |
| --- | --- |
| `-m, --model <id>` | model, default `deepseek-chat` (`deepseek-reasoner` also works) |
| `-s, --system <text>` | system prompt, placed first in the message array |
| `-t, --temperature <n>` | sampling temperature, default 1.0; negative omits the field |
| `-k, --max-tokens <n>` | cap the reply length, 0 lets the API decide |
| `--session <file>` | load history from `<file>` and write the new turns back |
| `--base-url <url>` | API root, default `https://api.deepseek.com` |
| `--stream` | stream the reply as it is generated |
| `--stats` | print token usage after each answer |
| `-h, --help` | usage |

Environment: `DEEPSEEK_API_KEY` (required), `DEEPSEEK_BASE_URL`,
`DEEPSEEK_MODEL`.

Interactive commands: `/help`, `/exit`, `/quit`, `/reset`, `/history`,
`/save <file>`, `/load <file>`.

## How it works

1. **Request** (`src/http.c`) — libcurl easy interface: `CURLOPT_POST` with a
   JSON body, `Authorization: Bearer $DEEPSEEK_API_KEY` via
   `CURLOPT_HTTPHEADER`, the response collected with a `CURLOPT_WRITEFUNCTION`
   sink, and `CURLOPT_ACCEPT_ENCODING ""` so curl decompresses for us.
2. **Payload** (`src/conversation.c`) — the history is an array of cJSON
   objects; each call duplicates it into `{"model":..., "messages":[...]}` and
   serialises it with `cJSON_PrintUnformatted`.
3. **Response** (`src/main.c`) — `cJSON_Parse`, then walk
   `choices[0].message.content`. On a non-2xx status the `error.message` /
   `error.type` / `error.code` fields are decoded and printed. A malformed or
   unexpected body produces a clear message instead of a crash.
4. **Multi-turn** — the assistant reply is appended to the same array, and the
   next request sends the whole thing. A failed turn is rolled back so a broken
   request cannot leave a dangling user message in the context.
5. **Streaming** — SSE lines are split by hand; each chunk's
   `choices[0].delta.content` is printed immediately and appended to the
   assembled reply that goes into the history.

### The Windows encoding problem

Windows consoles have *two* code pages. `chcp` reports the output one (often
65001/UTF-8 in modern terminals) while the **input** code page is frequently
still the legacy ANSI page (936/GBK on Chinese systems, 1252 elsewhere). Reading
a typed prompt with `fgets` therefore yields GBK bytes — not valid UTF-8 — and
the API rejects the request:

```
HTTP 400 ... messages[0].content: invalid unicode code point
```

The mirror image happens on output: UTF-8 bytes rendered as GBK show up as
mojibake (`你好` → `浣犲ソ`).

`src/console.c` avoids the code pages entirely by talking to the console with
`ReadConsoleW` / `WriteConsoleW`, which use UTF-16 regardless of the active code
page. Command line arguments are recovered with `CommandLineToArgvW` and
converted to UTF-8. When stdin/stdout is redirected to a pipe or file there is no
console API to use, so it falls back to plain byte I/O — which is what the test
suite drives.

## Tests

No API key and no network access to the real API are needed:
`tests/mock_server.py` implements a minimal `/chat/completions` endpoint that
validates the bearer token, requires a non-empty `messages` array, and supports
both plain and `stream: true` responses.

```sh
make
make test          # or: python tests/run_tests.py
```

14 cases: single question, multi-turn history replay, `--session` resume, system
prompt placement, streaming delta reassembly, streamed turns stored in history,
missing key, 401 error body decoding, `--base-url` without a key, unreachable
endpoint, request options reaching the payload, non-ASCII argument encoding,
pasted-shell-command guard, and `--help` content.

### Verified

| | |
| --- | --- |
| Windows + MinGW-w64 gcc 16.2 | built, run, and **tested against the real DeepSeek API** |
| libcurl 8.11.1 static, schannel backend | in use (a system libcurl works too) |
| Warnings | none under `-Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings -Wstrict-prototypes` |
| Tests | `tests/run_tests.py` 14/14 |
| Linux / macOS | covered by CI, not hand-verified |

`.github/workflows/build.yml` builds and runs the suite on ubuntu, macOS and
Windows.

## Notes

- Requires libcurl 7.61+.
- TLS is provided by the libcurl backend: usually OpenSSL on Linux/macOS, the
  Windows certificate store via schannel in the Windows build.
- Prompts and history are sent to the DeepSeek API; do not paste secrets into a
  conversation.

## License

[MIT](LICENSE)
