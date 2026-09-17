/*
 * deepseek.h - shared declarations for the DeepSeek CLI client.
 *
 * Project : DeepSeek command line chat client
 * Stack   : C99 + libcurl (HTTP) + cJSON (JSON)
 */
#ifndef DEEPSEEK_H
#define DEEPSEEK_H

#include <stddef.h>

#include <curl/curl.h>

#include "cJSON.h"

/* ------------------------------------------------------------------ */
/* Build configuration                                                 */
/* ------------------------------------------------------------------ */

#ifndef DS_DEFAULT_BASE_URL
#define DS_DEFAULT_BASE_URL "https://api.deepseek.com"
#endif

#ifndef DS_DEFAULT_MODEL
#define DS_DEFAULT_MODEL "deepseek-chat"
#endif

#define DS_API_KEY_ENV "DEEPSEEK_API_KEY"
#define DS_BASE_URL_ENV "DEEPSEEK_BASE_URL"
#define DS_MODEL_ENV "DEEPSEEK_MODEL"

/* Runtime options collected from the command line. */
typedef struct {
    const char *model;       /* model id sent to the API                */
    const char *system;      /* system prompt, may be NULL              */
    const char *base_url;    /* API root, no trailing slash             */
    const char *session;     /* persist/restore history in this file    */
    double temperature;      /* sampling temperature                    */
    int max_tokens;          /* 0 => omit from the request              */
    int stream;              /* 1 => ask the API for SSE streaming      */
    int show_stats;          /* 1 => report token usage                 */
} options_t;

/* ------------------------------------------------------------------ */
/* util.c - growable string buffer and length-safe string helpers      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

void strbuf_init(strbuf_t *sb);
void strbuf_free(strbuf_t *sb);
/* Append a NUL terminated string / raw bytes; 0 on success, -1 on OOM. */
int strbuf_append(strbuf_t *sb, const char *text);
int strbuf_append_n(strbuf_t *sb, const char *data, size_t n);
void strbuf_clear(strbuf_t *sb);

/* libcurl write callback, used with a strbuf_t as userdata. Its signature is
 * identical to the curl_write_callback typedef from curl.h. */
size_t write_to_strbuf(char *contents, size_t size, size_t nmemb, void *userp);

/* Trim leading/trailing whitespace in place, returns the new start. */
char *str_trim(char *s);

/* Duplicate n bytes as a NUL terminated heap string. */
char *str_dup_n(const char *s, size_t n);

/* Last path component of a path (handles both '/' and '\'). */
const char *path_basename(const char *path);

/* Escape a string for use inside a JSON string literal (for the session file). */
char *json_escape(const char *s);

/* ------------------------------------------------------------------ */
/* console.c - Windows Unicode console I/O                             */
/* ------------------------------------------------------------------ */

/*
 * On Windows the console input code page is often the legacy ANSI one (936,
 * 1252, ...), so a prompt typed at the console would reach us as non-UTF-8
 * bytes and the API would reject it. These helpers talk to the console with
 * the wide character API instead, which is encoding agnostic, and fall back
 * to ordinary byte I/O when stdin/stdout is redirected (pipes, files).
 */
void console_init(void);

/* Write UTF-8 text to stdout, correct whatever the console code page is. */
void ds_write(const char *text);

/* Same, for a length delimited (not NUL terminated) buffer. */
void ds_write_n(const char *data, size_t len);

/* Read one line of input as UTF-8. Returns NULL on EOF. */
char *ds_read_line(char *buffer, size_t size);

/* ------------------------------------------------------------------ */
/* http.c - libcurl transport                                          */
/* ------------------------------------------------------------------ */

/* curl_global_init wrapper; returns 0 on success. */
int http_init(void);
void http_cleanup(void);

/*
 * POST a JSON body and collect the response.
 *
 *   url          - full endpoint URL
 *   api_key      - bearer token, may be NULL (no Authorization header)
 *   body         - request payload
 *   write_fn     - sink for the response bytes; NULL means "collect the whole
 *                  body into body_out" (write_to_strbuf)
 *   write_data   - userdata for write_fn, or the strbuf_t to fill when
 *                  write_fn is NULL
 *   http_code    - receives the HTTP status, 0 when the transport failed
 *   err          - receives a curl error message on transport failure
 *
 * Returns 0 when the request completed at the transport level (the caller
 * still has to check *http_code), -1 when curl itself failed.
 */
int http_post_json(const char *url, const char *api_key, const char *body,
                   curl_write_callback write_fn, void *write_data,
                   long *http_code, char *err, size_t errlen);

/* ------------------------------------------------------------------ */
/* conversation.c - multi-turn history                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    cJSON **messages;   /* array of {role, content} objects */
    size_t count;
    size_t cap;
} conversation_t;

void conv_init(conversation_t *c);
void conv_free(conversation_t *c);
void conv_reset(conversation_t *c);

/* Append a message. Returns 0 on success, -1 on OOM. */
int conv_add(conversation_t *c, const char *role, const char *content);

/* Build the {"model":..., "messages":[...]} payload; caller owns the result. */
cJSON *conv_build_request(const conversation_t *c, const options_t *opt);

/* Deep copy of message i, or NULL when out of range. Caller owns it. */
cJSON *conv_get(const conversation_t *c, size_t index);

/* Save/load the history as JSON. Returns 0 on success, -1 on failure. */
int conv_save(const conversation_t *c, const char *path, char *err, size_t errlen);
int conv_load(conversation_t *c, const char *path, char *err, size_t errlen);

#endif /* DEEPSEEK_H */
