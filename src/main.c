/*
 * main.c - DeepSeek command line chat client.
 *
 *   ./ai "你好"                 one-shot question
 *   ./ai                        interactive multi-turn session
 *
 * The API key is read from the DEEPSEEK_API_KEY environment variable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "deepseek.h"

#define INPUT_MAX 8192

/* argv[0] as given on the command line, used for hints and usage text. */
static const char *g_argv0 = "ai";

/* ------------------------------------------------------------------ */
/* Windows: make argv UTF-8                                            */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
/*
 * On Windows, main() receives argv encoded in the active ANSI code page, so a
 * Chinese prompt typed at the console arrives as GBK bytes. The JSON body must
 * be UTF-8, so recover the real UTF-16 command line and convert it.
 *
 * Returns a UTF-8 argv on success, NULL when the conversion is not possible
 * (in which case the original argv is kept).
 */
static char **utf8_argv(int *argc)
{
    LPWSTR *wide;
    char **out;
    int count = 0;
    int i;
    int converted = 0;

    wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == NULL || count <= 0) {
        if (wide != NULL) {
            LocalFree(wide);
        }
        return NULL;
    }

    out = (char **)calloc((size_t)count + 1, sizeof(char *));
    if (out == NULL) {
        LocalFree(wide);
        return NULL;
    }

    for (i = 0; i < count; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);

        if (need <= 0) {
            break;
        }
        out[i] = (char *)malloc((size_t)need);
        if (out[i] == NULL) {
            break;
        }
        if (WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, out[i], need, NULL, NULL) <= 0) {
            free(out[i]);
            out[i] = NULL;
            break;
        }
        converted++;
    }

    LocalFree(wide);

    if (converted < count) {            /* partial conversion: roll back */
        for (i = 0; i < converted; i++) {
            free(out[i]);
        }
        free(out);
        return NULL;
    }

    *argc = count;
    return out;
}
#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    printf("DeepSeek CLI client (C + libcurl + cJSON)\n"
           "\n"
           "Usage:\n"
           "  %s [options] \"your question\"     ask one question and exit\n"
           "  %s [options]                      interactive multi-turn chat\n"
           "\n"
           "Options:\n"
           "  -m, --model <id>        model to use          (default: %s)\n"
           "  -s, --system <text>     system prompt\n"
           "  -t, --temperature <n>   sampling temperature, 0.0 - 2.0 (default: 1.0)\n"
           "  -k, --max-tokens <n>    cap the reply length (0 = let the API decide)\n"
           "      --session <file>    load history from <file> and append the new turns\n"
           "      --base-url <url>    API root            (default: %s)\n"
           "      --stream            stream the reply as it is generated\n"
           "      --stats             print token usage after each answer\n"
           "  -h, --help              show this help\n"
           "\n"
           "Environment:\n"
           "  %s      required API key\n"
           "  %s    optional API root override\n"
           "  %s       optional model override\n"
           "\n"
           "Interactive commands:\n"
           "  /help  /exit  /quit  /reset  /history  /save <file>  /load <file>\n"
           "\n"
           "Note: with no question argument you get an interactive chat prompt,\n"
           "      not a shell. To ask one question from the shell, pass it as an\n"
           "      argument (quotes are stripped by the shell, not by this program).\n",
           prog, prog, DS_DEFAULT_MODEL, DS_DEFAULT_BASE_URL,
           DS_API_KEY_ENV, DS_BASE_URL_ENV, DS_MODEL_ENV);
}

/* Look up an environment variable, ignoring empty values. */
static const char *env_or_null(const char *name)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0') {
        return NULL;
    }
    return value;
}

/* Trailing slashes in the base URL would produce "//chat/completions". */
static char *strip_trailing_slashes(const char *url)
{
    size_t len = strlen(url);

    while (len > 0 && url[len - 1] == '/') {
        len--;
    }
    return str_dup_n(url, len);
}

/* cJSON value -> printable text (numbers become heap strings). */
static char *json_scalar_text(const cJSON *node)
{
    if (node == NULL) {
        return NULL;
    }
    if (cJSON_IsString(node)) {
        return str_dup_n(node->valuestring, strlen(node->valuestring));
    }
    if (cJSON_IsNumber(node) || cJSON_IsBool(node)) {
        return cJSON_PrintUnformatted(node);
    }
    return NULL;
}

/* Role of a history entry, or "" when it does not look like a message. */
static const char *message_role(const cJSON *message)
{
    const cJSON *role = cJSON_IsObject(message)
                            ? cJSON_GetObjectItemCaseSensitive(message, "role")
                            : NULL;

    return cJSON_IsString(role) ? role->valuestring : "";
}

static const char *finish_reason_text(const char *reason)
{
    if (reason == NULL) {
        return "";
    }
    if (strcmp(reason, "length") == 0) {
        return "  [reply truncated: max_tokens reached]";
    }
    if (strcmp(reason, "content_filter") == 0) {
        return "  [reply stopped by the content filter]";
    }
    return "";
}

/*
 * Print the API error object, e.g.
 *   {"error":{"message":"...","type":"invalid_request_error","code":"..."}}
 */
static void report_api_error(const cJSON *root)
{
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *message;
    const cJSON *type;
    const cJSON *code;

    if (!cJSON_IsObject(error)) {
        char *dumped = cJSON_PrintUnformatted(root);

        fprintf(stderr, "error: unexpected response: %s\n",
                dumped != NULL ? dumped : "(unprintable)");
        free(dumped);
        return;
    }

    message = cJSON_GetObjectItemCaseSensitive(error, "message");
    type = cJSON_GetObjectItemCaseSensitive(error, "type");
    code = cJSON_GetObjectItemCaseSensitive(error, "code");

    fprintf(stderr, "error: %s\n",
            cJSON_IsString(message) ? message->valuestring : "unknown API error");
    if (cJSON_IsString(type)) {
        fprintf(stderr, "       type: %s\n", type->valuestring);
    }
    if (cJSON_IsString(code)) {
        fprintf(stderr, "       code: %s\n", code->valuestring);
    }
}

static void print_stats(const cJSON *usage)
{
    const cJSON *prompt;
    const cJSON *completion;
    const cJSON *total;

    if (!cJSON_IsObject(usage)) {
        return;
    }
    prompt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
    completion = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
    total = cJSON_GetObjectItemCaseSensitive(usage, "total_tokens");

    fprintf(stderr, "[tokens] prompt=%.0f completion=%.0f total=%.0f\n",
            cJSON_IsNumber(prompt) ? prompt->valuedouble : 0.0,
            cJSON_IsNumber(completion) ? completion->valuedouble : 0.0,
            cJSON_IsNumber(total) ? total->valuedouble : 0.0);
}

/* ------------------------------------------------------------------ */
/* response handling                                                   */
/* ------------------------------------------------------------------ */

/*
 * Fill answer / finish_reason / usage from a complete (non-streaming)
 * response object. Returns 0 on success.
 */
static int extract_completion(const cJSON *response, char **answer,
                              const char **finish_reason, const cJSON **usage)
{
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(response, "choices");
    const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *message = cJSON_IsObject(first)
                               ? cJSON_GetObjectItemCaseSensitive(first, "message")
                               : NULL;
    const cJSON *content = cJSON_IsObject(message)
                               ? cJSON_GetObjectItemCaseSensitive(message, "content")
                               : NULL;
    const cJSON *reason = cJSON_IsObject(first)
                              ? cJSON_GetObjectItemCaseSensitive(first, "finish_reason")
                              : NULL;

    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        fprintf(stderr, "error: response has no choices[]\n");
        report_api_error(response);
        return -1;
    }
    if (content == NULL) {
        fprintf(stderr, "error: response has no choices[0].message.content\n");
        report_api_error(response);
        return -1;
    }

    *answer = json_scalar_text(content);
    if (*answer == NULL) {
        fprintf(stderr, "error: choices[0].message.content is not printable\n");
        return -1;
    }
    *finish_reason = cJSON_IsString(reason) ? reason->valuestring : NULL;
    *usage = cJSON_GetObjectItemCaseSensitive(response, "usage");
    return 0;
}

/* --------------------------- streaming ---------------------------- */

typedef struct {
    strbuf_t line;      /* bytes of the SSE line being assembled   */
    strbuf_t answer;    /* concatenated delta.content              */
    cJSON *usage;       /* last usage object seen                  */
    char finish[32];    /* last finish_reason                      */
    int header_skipped; /* whether the raw HTTP status line is gone */
    int done;           /* saw "data: [DONE]"                      */
    int error;          /* out of memory                           */
    int failed;         /* server reported an error mid-stream     */
} sse_state_t;

static void sse_state_free(sse_state_t *st)
{
    strbuf_free(&st->line);
    strbuf_free(&st->answer);
    cJSON_Delete(st->usage);
    st->usage = NULL;
}

/* Handle one complete SSE line. */
static void sse_line(sse_state_t *st, char *line)
{
    cJSON *event;
    const cJSON *choices;
    const cJSON *first;
    const cJSON *delta;
    const cJSON *content;
    const cJSON *reason;
    const cJSON *usage;

    line = str_trim(line);
    if (line[0] == '\0' || strncmp(line, "data:", 5) != 0) {
        return;     /* comments, "event:" lines, blank keep-alives */
    }
    line = str_trim(line + 5);
    if (strcmp(line, "[DONE]") == 0) {
        st->done = 1;
        return;
    }

    event = cJSON_Parse(line);
    if (event == NULL) {
        return;     /* ignore malformed keep-alive chunks */
    }

    /* Some backends report a failure mid-stream instead of sending a delta. */
    if (cJSON_GetObjectItemCaseSensitive(event, "error") != NULL) {
        report_api_error(event);
        st->failed = 1;
        cJSON_Delete(event);
        return;
    }

    choices = cJSON_GetObjectItemCaseSensitive(event, "choices");
    first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    delta = cJSON_IsObject(first)
                ? cJSON_GetObjectItemCaseSensitive(first, "delta")
                : NULL;
    content = cJSON_IsObject(delta)
                  ? cJSON_GetObjectItemCaseSensitive(delta, "content")
                  : NULL;

    if (cJSON_IsString(content) && content->valuestring[0] != '\0') {
        if (strbuf_append(&st->answer, content->valuestring) != 0) {
            st->error = 1;
        }
        ds_write(content->valuestring);
        fflush(stdout);
    }

    reason = cJSON_IsObject(first)
                 ? cJSON_GetObjectItemCaseSensitive(first, "finish_reason")
                 : NULL;
    if (cJSON_IsString(reason)) {
        snprintf(st->finish, sizeof(st->finish), "%s", reason->valuestring);
    }

    usage = cJSON_GetObjectItemCaseSensitive(event, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON_Delete(st->usage);
        st->usage = cJSON_Duplicate(usage, 1);
    }

    cJSON_Delete(event);
}

/*
 * libcurl sink for a "stream": true response. A proxy may prepend an HTTP
 * status line ("HTTP/1.1 200 Connection established") before the body, so
 * anything before the first "data:" line is dropped.
 */
static size_t stream_sink(char *ptr, size_t size, size_t nmemb, void *userdata)
{    sse_state_t *st = (sse_state_t *)userdata;
    size_t total = size * nmemb;
    size_t i;

    if (st->error != 0) {
        return 0;
    }

    for (i = 0; i < total; i++) {
        char c = ptr[i];

        if (c == '\n') {
            if (st->header_skipped) {
                st->line.data[st->line.len] = '\0';
                sse_line(st, st->line.data);
            } else if (st->line.len >= 5 &&
                       strncmp(st->line.data, "data:", 5) == 0) {
                st->header_skipped = 1;
                st->line.data[st->line.len] = '\0';
                sse_line(st, st->line.data);
            }
            strbuf_clear(&st->line);
        } else if (c != '\r') {
            if (strbuf_append_n(&st->line, &c, 1) != 0) {
                st->error = 1;
                return 0;
            }
        }
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* one round trip                                                      */
/* ------------------------------------------------------------------ */

static void rollback_to(conversation_t *conv, size_t mark)
{
    while (conv->count > mark) {
        cJSON_Delete(conv->messages[conv->count - 1]);
        conv->count--;
    }
}

/*
 * Send the history and append the reply to it.
 *
 * turn_mark is conv->count as it was *before* the caller added the user
 * message; on failure the history is rolled back to it, so a failed or
 * half-finished turn never pollutes the context window.
 *
 * Returns 0 on success, -1 on failure (already reported on stderr).
 */
static int ask_deepseek(conversation_t *conv, const options_t *opt,
                        const char *api_key, const char *url, size_t turn_mark)
{
    cJSON *request;
    cJSON *response = NULL;
    char *payload;
    char err[CURL_ERROR_SIZE];
    char *answer = NULL;
    const char *finish_reason = NULL;
    const cJSON *usage = NULL;
    strbuf_t body;
    sse_state_t stream;
    long http_code = 0;
    int status = -1;

    err[0] = '\0';
    strbuf_init(&body);
    memset(&stream, 0, sizeof(stream));

    /* 1. history -> request JSON ---------------------------------- */
    request = conv_build_request(conv, opt);
    if (request == NULL) {
        fprintf(stderr, "error: out of memory while building the request\n");
        goto cleanup;
    }
    payload = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (payload == NULL) {
        fprintf(stderr, "error: out of memory while serialising the request\n");
        goto cleanup;
    }

    /* 2. POST via libcurl ----------------------------------------- */
    if (opt->stream) {
        strbuf_init(&stream.line);
        strbuf_init(&stream.answer);

        if (http_post_json(url, api_key, payload, stream_sink, &stream,
                           &http_code, err, sizeof(err)) != 0) {
            if (stream.failed) {
                goto cleanup;               /* already reported on stderr */
            }
            fprintf(stderr, "error: request to %s failed: %s\n", url,
                    err[0] != '\0' ? err : "unknown transport error");
            free(payload);
            goto cleanup;
        }
        free(payload);

        if (http_code < 200 || http_code >= 300) {
            /* The body is the raw error response, not a stream of deltas. */
            fprintf(stderr, "error: HTTP %ld from %s\n", http_code, url);
            if (stream.answer.len > 0) {
                cJSON *error_body = cJSON_Parse(stream.answer.data);

                if (error_body != NULL) {
                    report_api_error(error_body);
                    cJSON_Delete(error_body);
                } else {
                    fprintf(stderr, "       %.300s\n", stream.answer.data);
                }
            }
            goto cleanup;
        }
        if (stream.failed || stream.error) {
            goto cleanup;
        }
        if (stream.answer.data != NULL) {
            ds_write("\n");
        }
        answer = stream.answer.data;
        stream.answer.data = NULL;      /* ownership moves to answer */
        finish_reason = stream.finish[0] != '\0' ? stream.finish : NULL;
        usage = stream.usage;
    } else {
        if (http_post_json(url, api_key, payload, NULL, &body,
                           &http_code, err, sizeof(err)) != 0) {
            fprintf(stderr, "error: request to %s failed: %s\n", url,
                    err[0] != '\0' ? err : "unknown transport error");
            free(payload);
            goto cleanup;
        }
        free(payload);

        if (body.data == NULL) {
            fprintf(stderr, "error: empty response from the API (HTTP %ld)\n", http_code);
            goto cleanup;
        }

        response = cJSON_Parse(body.data);
        if (response == NULL) {
            fprintf(stderr, "error: cannot parse the API response (HTTP %ld): %.300s\n",
                    http_code, body.data);
            goto cleanup;
        }
        if (http_code < 200 || http_code >= 300) {
            fprintf(stderr, "error: HTTP %ld from %s\n", http_code, url);
            report_api_error(response);
            goto cleanup;
        }
        if (extract_completion(response, &answer, &finish_reason, &usage) != 0) {
            goto cleanup;
        }
        ds_write(answer);
        ds_write("\n");
    }

    /* 3. remember the turn so the next call has the full context --- */
    if (answer == NULL || answer[0] == '\0') {
        fprintf(stderr, "warning: the model returned an empty reply\n");
    }
    if (conv_add(conv, "assistant", answer != NULL ? answer : "") != 0) {
        fprintf(stderr, "error: out of memory while storing the reply\n");
        goto cleanup;
    }

    if (finish_reason != NULL && strcmp(finish_reason, "stop") != 0) {
        fprintf(stderr, "%s\n", finish_reason_text(finish_reason));
    }
    if (opt->show_stats) {
        print_stats(usage);
    }

    status = 0;

cleanup:
    /*
     * Keep the history consistent: a failed turn is rolled back entirely, so
     * a broken request cannot leave a dangling user message in the context.
     * On success the history keeps both the user turn and the reply.
     */
    if (status != 0) {
        rollback_to(conv, turn_mark);
    }
    free(answer);
    cJSON_Delete(response);
    strbuf_free(&body);
    sse_state_free(&stream);
    return status;
}

/* ------------------------------------------------------------------ */
/* interactive mode                                                    */
/* ------------------------------------------------------------------ */

static void print_history(const conversation_t *conv)
{
    size_t i;

    printf("--- history: %u message(s) ---\n", (unsigned int)conv->count);
    for (i = 0; i < conv->count; i++) {
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(conv->messages[i], "content");
        const char *text = cJSON_IsString(content) ? content->valuestring : "";

        printf("[%u] %-9s %.120s%s\n", (unsigned int)i,
               message_role(conv->messages[i]),
               text, strlen(text) > 120 ? "..." : "");
    }
    printf("-------------------------------\n");
}

/* Read one line from the console (UTF-8) or from a pipe. */
static char *read_line(char *buffer, size_t size)
{
    if (ds_read_line(buffer, size) == NULL) {
        return NULL;
    }
    return buffer;
}

/*
 * Heuristic for the classic copy/paste mistake of pasting a shell command
 * (".\ai.exe \"hi\"" or "ai hi") into the chat prompt instead of the shell.
 * A real question never starts with the program name.
 */
static int looks_like_invocation(const char *text, const char *prog)
{
    const char *p = text;
    size_t len = 0;

    while (*p == ' ' || *p == '\t' || *p == '.' || *p == '/' || *p == '\\') {
        p++;
    }
    while (p[len] != '\0' && p[len] != ' ' && p[len] != '\t' &&
           p[len] != '"' && p[len] != '\'') {
        len++;
    }
    if (len == 0) {
        return 0;
    }
    if (strncmp(p, prog, len) == 0 && prog[len] == '\0') {
        return 1;                       /* the token is exactly our program name */
    }
    /* Also catch a bare "ai" / "ai.exe" when argv[0] was a full path. */
    if (strncmp(p, "ai", len) == 0 && (len == 2 || (len == 6 && strncmp(p, "ai.exe", 6) == 0))) {
        return 1;
    }
    return 0;
}

/* Returns 1 when the REPL should stop. */
static int handle_command(conversation_t *conv, char *line)
{
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) {
        return 1;
    }
    if (strcmp(line, "/help") == 0) {
        printf("commands: /help /exit /quit /reset /history /save <file> /load <file>\n");
        return 0;
    }
    if (strcmp(line, "/reset") == 0) {
        conv_reset(conv);
        printf("history cleared\n");
        return 0;
    }
    if (strcmp(line, "/history") == 0) {
        print_history(conv);
        return 0;
    }
    if (strncmp(line, "/save ", 6) == 0) {
        char *path = str_trim(line + 6);
        char err[256];

        err[0] = '\0';
        if (conv_save(conv, path, err, sizeof(err)) == 0) {
            printf("history saved to %s\n", path);
        } else {
            fprintf(stderr, "error: %s\n", err);
        }
        return 0;
    }
    if (strncmp(line, "/load ", 6) == 0) {
        char *path = str_trim(line + 6);
        char err[256];

        err[0] = '\0';
        if (conv_load(conv, path, err, sizeof(err)) == 0) {
            printf("history loaded from %s (%u messages)\n", path,
                   (unsigned int)conv->count);
        } else {
            fprintf(stderr, "error: %s\n", err);
        }
        return 0;
    }

    fprintf(stderr, "unknown command: %s (try /help)\n", line);
    return 0;
}

static int interactive(conversation_t *conv, const options_t *opt,
                       const char *api_key, const char *url)
{
    char line[INPUT_MAX];
    char hint[512];
    const char *prog = path_basename(g_argv0);

    snprintf(hint, sizeof(hint),
             "  this prompt is a chat with the model, not a shell.\n"
             "  to ask a question straight from the shell, press Ctrl+C and run:\n"
             "      .\\%s \"your question\"\n", prog);

    ds_write("DeepSeek CLI - model ");
    ds_write(opt->model);
    ds_write("\nType a message and press Enter. /help for commands, /exit to quit.\n\n");

    for (;;) {
        char *text;
        size_t turn_mark;

        ds_write("you> ");
        fflush(stdout);
        if (read_line(line, sizeof(line)) == NULL) {
            ds_write("\n");
            break;
        }
        text = str_trim(line);
        if (text[0] == '\0') {
            continue;
        }
        if (text[0] == '/') {
            if (handle_command(conv, text)) {
                break;
            }
            continue;
        }

        /* Catch the common copy/paste mistake of pasting a shell command. */
        if (looks_like_invocation(text, prog)) {
            ds_write(hint);
            continue;
        }

        turn_mark = conv->count;
        if (conv_add(conv, "user", text) != 0) {
            fprintf(stderr, "error: out of memory\n");
            return -1;
        }
        ds_write("ai > ");
        fflush(stdout);
        if (ask_deepseek(conv, opt, api_key, url, turn_mark) != 0) {
            fprintf(stderr, "(turn discarded, history is unchanged)\n");
        }
        ds_write("\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    options_t opt;
    conversation_t conv;
    const char *api_key;
    char *base_url = NULL;
    char *prompt = NULL;
    char *url = NULL;
    int i;
    int status = 0;
    int base_url_given = 0;

    g_argv0 = argv[0];
    console_init();

#ifdef _WIN32
    /* Windows hands us code page encoded arguments; the API needs UTF-8. */
    {
        char **utf8 = utf8_argv(&argc);

        if (utf8 != NULL) {
            argv = utf8;
        }
    }
#endif

    /* --- options, environment defaults ---------------------------- */
    api_key = env_or_null(DS_API_KEY_ENV);
    opt.model = env_or_null(DS_MODEL_ENV);
    if (opt.model == NULL) {
        opt.model = DS_DEFAULT_MODEL;
    }
    opt.system = NULL;
    opt.session = NULL;
    opt.base_url = env_or_null(DS_BASE_URL_ENV);
    if (opt.base_url == NULL) {
        opt.base_url = DS_DEFAULT_BASE_URL;
    }
    opt.temperature = 1.0;
    opt.max_tokens = 0;
    opt.stream = 0;
    opt.show_stats = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if ((strcmp(arg, "-m") == 0 || strcmp(arg, "--model") == 0) && next != NULL) {
            opt.model = next;
            i++;
        } else if ((strcmp(arg, "-s") == 0 || strcmp(arg, "--system") == 0) && next != NULL) {
            opt.system = next;
            i++;
        } else if ((strcmp(arg, "-t") == 0 || strcmp(arg, "--temperature") == 0) && next != NULL) {
            opt.temperature = atof(next);
            i++;
        } else if ((strcmp(arg, "-k") == 0 || strcmp(arg, "--max-tokens") == 0) && next != NULL) {
            opt.max_tokens = atoi(next);
            i++;
        } else if (strcmp(arg, "--session") == 0 && next != NULL) {
            opt.session = next;
            i++;
        } else if (strcmp(arg, "--base-url") == 0 && next != NULL) {
            opt.base_url = next;
            base_url_given = 1;
            i++;
        } else if (strcmp(arg, "--stream") == 0) {
            opt.stream = 1;
        } else if (strcmp(arg, "--stats") == 0) {
            opt.show_stats = 1;
        } else if (arg[0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n\n", arg);
            print_usage(argv[0]);
            return 2;
        } else {
            prompt = argv[i];   /* the first bare argument is the question */
            break;
        }
    }

    if (opt.temperature < 0.0) {
        opt.temperature = -1.0;     /* sentinel: omit it from the payload */
    }
    if (opt.max_tokens < 0) {
        opt.max_tokens = 0;
    }

    base_url = strip_trailing_slashes(opt.base_url);
    if (base_url == NULL) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    url = (char *)malloc(strlen(base_url) + sizeof("/chat/completions") + 1);
    if (url == NULL) {
        fprintf(stderr, "error: out of memory\n");
        free(base_url);
        return 1;
    }
    sprintf(url, "%s/chat/completions", base_url);

    if (api_key == NULL) {
        if (base_url_given) {
            /*
             * A local or self hosted endpoint may not need a key, so do not
             * refuse to start: send the request without one and report
             * whatever the server answers.
             */
            fprintf(stderr, "warning: %s is not set, sending the request "
                            "without an Authorization header\n", DS_API_KEY_ENV);
        } else {
            fprintf(stderr,
                    "error: %s is not set.\n"
                    "       Linux/macOS: export %s=sk-...\n"
                    "       Windows cmd: set %s=sk-...\n"
                    "       PowerShell : $env:%s=\"sk-...\"\n",
                    DS_API_KEY_ENV, DS_API_KEY_ENV, DS_API_KEY_ENV, DS_API_KEY_ENV);
            free(base_url);
            free(url);
            return 1;
        }
        api_key = "";
    }

    if (http_init() != 0) {
        free(base_url);
        free(url);
        return 1;
    }

    conv_init(&conv);

    /* Restore a previous session before anything else is appended. */
    if (opt.session != NULL) {
        char err[256];

        err[0] = '\0';
        if (conv_load(&conv, opt.session, err, sizeof(err)) == 0) {
            fprintf(stderr, "(resumed %u messages from %s)\n",
                    (unsigned int)conv.count, opt.session);
        } else {
            fprintf(stderr, "(starting a new session: %s)\n", err);
        }
    }
    if (opt.system != NULL) {
        /* A system prompt must come first, otherwise it is ignored. */
        if (conv.count > 0 && strcmp(message_role(conv.messages[0]), "system") == 0) {
            cJSON_Delete(conv.messages[0]);
            memmove(conv.messages, conv.messages + 1,
                    (conv.count - 1) * sizeof(cJSON *));
            conv.count--;
        }
        if (conv_add(&conv, "system", opt.system) != 0) {
            fprintf(stderr, "error: out of memory\n");
            status = 1;
            goto done;
        }
    }

    if (prompt != NULL && prompt[0] != '\0') {
        size_t turn_mark = conv.count;

        if (conv_add(&conv, "user", prompt) != 0) {
            fprintf(stderr, "error: out of memory\n");
            status = 1;
            goto done;
        }
        status = (ask_deepseek(&conv, &opt, api_key, url, turn_mark) == 0) ? 0 : 1;
    } else {
        status = (interactive(&conv, &opt, api_key, url) == 0) ? 0 : 1;
    }

    if (opt.session != NULL && conv.count > 0) {
        char err[256];

        err[0] = '\0';
        if (conv_save(&conv, opt.session, err, sizeof(err)) != 0) {
            fprintf(stderr, "warning: could not save the session: %s\n", err);
        }
    }

done:
    conv_free(&conv);
    http_cleanup();
    free(base_url);
    free(url);
    return status;
}
