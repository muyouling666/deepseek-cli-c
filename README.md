# ai — DeepSeek 命令行对话客户端（C + libcurl + cJSON）

中文说明 | [English](README.en.md)

一个用 C 写的 DeepSeek 命令行客户端：命令行传入问题，用 **libcurl** 向 DeepSeek API 发
POST 请求，用 **cJSON** 解析返回的 JSON，取出 `choices[0].message.content` 打印出来；
交互模式下把历史消息保存在一个 JSON 数组里，实现多轮对话；API Key 从环境变量
`DEEPSEEK_API_KEY` 读取；使用 **Makefile** 编译。

```
$ export DEEPSEEK_API_KEY=sk-xxxxxxxx
$ make
$ ./ai "你好"
你好！有什么可以帮你的吗？

$ ./ai
DeepSeek CLI - model deepseek-chat
Type a message and press Enter. /help for commands, /exit to quit.

you> 用一句话解释什么是递归
ai > 递归就是函数在定义中调用自己……
you> 再举个例子
ai > 比如计算阶乘……
```

## 目录结构

```
Makefile                  编译规则（make / make deps / make test / make clean）
src/deepseek.h            公共声明：options_t / conversation_t / strbuf_t
src/main.c                参数解析、请求流程、交互式 REPL、流式打印
src/http.c                libcurl 封装：一次 POST，响应收进内存
src/conversation.c        多轮历史（cJSON 数组）、会话文件保存/读取
src/console.c             Windows 控制台 Unicode 输入输出（详见下文）
src/util.c                可增长字符串缓冲、libcurl 回调、字符串工具
vendor/cJSON.c, .h        cJSON 1.7.18（JSON 解析/生成）
scripts/fetch-deps.sh     没有系统 libcurl 时，把 cJSON + libcurl 编译到 build/
scripts/build-curl-mingw.ps1  Windows 无 POSIX shell 时的 libcurl 构建脚本
scripts/Setup-Build.ps1   Windows 一键准备工具链并生成 build/local.mk
tests/mock_server.py      本地假的 DeepSeek API，用于离线测试
tests/run_tests.py        14 个端到端测试用例
.github/workflows/build.yml  CI：ubuntu / macOS / Windows 三平台编译并测试
LICENSE                   MIT
```

## 编译

### Linux / macOS

```sh
sudo apt install build-essential libcurl4-openssl-dev   # Debian/Ubuntu
brew install curl                                       # macOS
make
```

### Windows

推荐 MSYS2（MinGW-w64 工具链 + libcurl 开发包）：

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-curl make
make
```

只有 gcc、没有 POSIX shell（Git Bash / MSYS2 的 `make deps` 跑不了）时，用 PowerShell
脚本准备依赖：

```powershell
powershell -File scripts\Setup-Build.ps1 -Gcc C:\mingw64\bin\gcc.exe -CurlSource C:\src\curl-8.11.1
# 或者： make setup SETUP_ARGS="-Gcc C:\mingw64\bin\gcc.exe"
```

也可以在 MSVC 下编译（把 `src/*.c vendor/cJSON.c` 一起编译并链接 libcurl）。

### 没有安装 libcurl 时

`make deps` 会在 `build/` 里从源码编译 cJSON 和 libcurl（Windows 上使用系统自带的
schannel 作为 TLS 后端，不需要 OpenSSL），然后写出 `build/deps.mk`，之后的 `make`
会自动使用它：

```sh
make deps && make
```

生成的 Windows 可执行文件只依赖系统 DLL（ws2_32 / secur32 / crypt32 / bcrypt /
shell32），可以直接拷走运行。

## 命令行选项

| 选项 | 说明 |
| --- | --- |
| `-m, --model <id>` | 模型，默认 `deepseek-chat`（可换 `deepseek-reasoner`） |
| `-s, --system <text>` | 系统提示词，放在消息数组最前面 |
| `-t, --temperature <n>` | 采样温度，默认 1.0；设为负数则不下发该字段 |
| `-k, --max-tokens <n>` | 限制回复长度，0 表示由服务端决定 |
| `--session <file>` | 读取历史并在结束后写回（跨进程多轮对话） |
| `--base-url <url>` | API 根地址，默认 `https://api.deepseek.com` |
| `--stream` | 流式输出（`stream: true`，按 SSE 增量打印） |
| `--stats` | 打印 token 用量 |
| `-h, --help` | 帮助 |

环境变量：`DEEPSEEK_API_KEY`（必填）、`DEEPSEEK_BASE_URL`、`DEEPSEEK_MODEL`。

交互模式内置命令：`/help`、`/exit`、`/reset`、`/history`、`/save <file>`、
`/load <file>`。

## 实现要点

**1. 发请求**（`src/http.c`）——libcurl easy 接口，`CURLOPT_POST` +
`CURLOPT_POSTFIELDS`，`Authorization: Bearer $DEEPSEEK_API_KEY` 放在
`CURLOPT_HTTPHEADER` 里；响应通过 `CURLOPT_WRITEFUNCTION` 写进可增长缓冲区，
`CURLOPT_ACCEPT_ENCODING ""` 让 curl 自动解压。

**2. 拼 JSON**（`src/conversation.c`）——历史是 `cJSON *messages[]`，每次请求前
`cJSON_Duplicate` 成 `{"model":..., "messages":[...]}`；`cJSON_PrintUnformatted`
序列化成单行字符串作为请求体。

**3. 解析响应**（`src/main.c`）——`cJSON_Parse` 后沿
`choices[0].message.content` 取值；HTTP 非 2xx 时解析 `error.message` /
`error.type` / `error.code` 并打印；解析失败或字段缺失都给出明确错误而不是崩溃。

**4. 多轮对话**——每轮把 `assistant` 的回复追加进同一个历史数组，下一次请求把整个
数组一起发出去，模型因此能看到完整上下文。失败的一轮会回滚，历史保持一致。

**5. 流式输出**（`--stream`）——自己按行切分 SSE，取每个 chunk 的
`choices[0].delta.content` 边收边打印，同时拼接成完整回复存入历史；代理在响应体前
插入的 `HTTP/1.1 200 Connection established` 之类的内容会被跳过。

## 测试

不需要真实 API Key，`tests/mock_server.py` 实现了 `/chat/completions` 的
最小可用版本（校验 Bearer 头与 key、校验 `messages`、支持 `stream`）：

```sh
make
python tests/run_tests.py
```

覆盖 14 个用例：单轮问答、多轮历史回放、`--session` 续聊、系统提示词位置、流式增量
拼接、流式历史落库、缺少 Key、401 错误体解析、--base-url 无 Key、连不上主机、
各选项是否真的进入请求体、非 ASCII 参数编码、误粘贴拦截、`--help` 内容。

CI（`.github/workflows/build.yml`）在 ubuntu / macOS / Windows 三个平台编译并跑这套
测试；测试全部走本地 mock，不需要 API Key，也不会真的调用 DeepSeek。

### 已验证环境

| 项目 | 情况 |
| --- | --- |
| Windows + MinGW-w64 gcc 16.2 | 实际编译、运行、**调用真实 DeepSeek API 对话成功** |
| libcurl 8.11.1 静态库 + schannel | 使用中（也可直接用系统 libcurl） |
| 编译告警 | `-Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings -Wstrict-prototypes` 下零告警 |
| 测试 | `tests/run_tests.py` 14/14 通过 |
| Linux / macOS | 由 CI 覆盖，未在本机手工验证 |

## 常见问题

**1. 报 `HTTP 400 ... messages[0].content: invalid unicode code point`**

Windows 控制台有**两个代码页**：`chcp` 显示的通常是输出代码页（现代终端是
65001/UTF-8），而**输入**代码页往往还是旧的 ANSI 页（中文系统是 936/GBK）。
用 `fgets` 读到的中文就是 GBK 字节，不是合法 UTF-8，服务端直接拒绝。

程序已处理：Windows 上输入走 `ReadConsoleW`（控制台用 UTF-16，与应用代码页无关），
输出走 `WriteConsoleW`，因此**不需要**手动 `chcp 65001`。
如果仍然遇到该报错，说明输入来自重定向的文件或管道，请确认该文件是 UTF-8 编码：

```powershell
.\ai.exe < questions.txt        # questions.txt 必须是 UTF-8
```

**2. 回复里的中文显示成 `浣犲ソ` 这种乱码**

这是「UTF-8 字节被当成 GBK 渲染」的典型特征，属于终端显示问题。程序已用
`WriteConsoleW` 直接向控制台写 UTF-16，正常情况下不会再出现；若在很旧的
conhost 或第三方终端里仍有问题，可先执行 `chcp 65001`，或改用 Windows Terminal。

**3. 在交互模式里粘贴了 `.\ai.exe "你好"`**

带引号的那串会被当成聊天内容发给模型，服务端返回
`Failed to parse the request body as JSON`。程序现在会识别这种误粘贴并给出提示，
不再发送。要单次提问请在 **shell** 里运行，不要进交互模式后再输入。

**4. `cannot open output file ai.exe: Permission denied`**

上一次运行的程序还开着。关掉那个窗口，或执行
`Get-Process ai | Stop-Process -Force` 后重新 `make`。

**5. 连接失败 `Failed to connect`**

网络或代理问题。使用代理时设置环境变量（libcurl 认这些）：

```powershell
$env:HTTPS_PROXY="http://127.0.0.1:7890"
```

## 说明

- 需要 libcurl 7.61+（用到 `CURLOPT_ACCEPT_ENCODING` 等常规选项）。
- 代码为 C99。Windows 下的编码处理见 `src/console.c`：命令行参数通过
  `CommandLineToArgvW` + `WideCharToMultiByte` 转 UTF-8，交互输入通过
  `ReadConsoleW` 读取，输出通过 `WriteConsoleW` 写出，所以中文在 GBK 代码页的
  控制台上也能正常工作；stdin/stdout 被重定向时自动回退到普通字节 I/O。
- HTTPS 由 libcurl 后端负责：Linux/macOS 通常是 OpenSSL，Windows 构建用的是系统
  schannel，证书来自 Windows 证书库。
- 提示词和历史会发往 DeepSeek API，请勿在对话里放敏感信息。

## 参与开发

```sh
make            # 编译
make test       # 14 个端到端用例（本地 mock，无需 API Key）
make info       # 打印实际使用的编译/链接参数
make deps       # 没有系统 libcurl 时，从源码构建到 build/
```

提交前请确认 `-Wall -Wextra` 下没有新增告警，并保持 `make test` 全绿。

## License

[MIT](LICENSE)
