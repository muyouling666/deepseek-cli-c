/*
 * util.c - growable string buffer, libcurl sink, string helpers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deepseek.h"

void strbuf_init(strbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void strbuf_free(strbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void strbuf_clear(strbuf_t *sb)
{
    sb->len = 0;
    if (sb->data != NULL) {
        sb->data[0] = '\0';
    }
}

/* Make sure there is room for n more bytes plus the NUL terminator. */
static int strbuf_reserve(strbuf_t *sb, size_t n)
{
    size_t need;
    size_t cap;
    char *grown;

    if (sb->cap != 0 && sb->len + n + 1 <= sb->cap) {
        return 0;
    }

    need = sb->len + n + 1;
    cap = (sb->cap != 0) ? sb->cap : 256;
    while (cap < need) {
        cap *= 2;
    }

    grown = (char *)realloc(sb->data, cap);
    if (grown == NULL) {
        return -1;
    }
    sb->data = grown;
    sb->cap = cap;
    return 0;
}

int strbuf_append_n(strbuf_t *sb, const char *data, size_t n)
{
    if (data == NULL || n == 0) {
        return 0;
    }
    if (strbuf_reserve(sb, n) != 0) {
        return -1;
    }
    memcpy(sb->data + sb->len, data, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int strbuf_append(strbuf_t *sb, const char *text)
{
    if (text == NULL) {
        return 0;
    }
    return strbuf_append_n(sb, text, strlen(text));
}

/* libcurl calls this with the CURLOPT_WRITEDATA pointer we supplied. The
 * signature matches the curl_write_callback typedef declared in deepseek.h. */
size_t write_to_strbuf(char *contents, size_t size, size_t nmemb, void *userp)
{
    strbuf_t *sb = (strbuf_t *)userp;
    size_t total = size * nmemb;

    if (strbuf_append_n(sb, contents, total) != 0) {
        return 0; /* tells curl the write failed -> CURLE_WRITE_ERROR */
    }
    return total;
}

char *str_trim(char *s)
{
    char *end;

    if (s == NULL) {
        return NULL;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    end = s + strlen(s);
    while (end > s) {
        char c = end[-1];

        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        end--;
    }
    *end = '\0';
    return s;
}

char *str_dup_n(const char *s, size_t n)
{
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    copy = (char *)malloc(n + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

const char *path_basename(const char *path)
{
    const char *last;

    if (path == NULL) {
        return "";
    }
    last = path;
    for (; *path != '\0'; path++) {
        if (*path == '/' || *path == '\\') {
            last = path + 1;
        }
    }
    return last;
}

char *json_escape(const char *s)
{
    strbuf_t sb;
    size_t i;

    if (s == NULL) {
        return NULL;
    }

    strbuf_init(&sb);
    for (i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        char tmp[8];

        switch (c) {
        case '"':  strbuf_append(&sb, "\\\""); break;
        case '\\': strbuf_append(&sb, "\\\\"); break;
        case '\n': strbuf_append(&sb, "\\n");  break;
        case '\r': strbuf_append(&sb, "\\r");  break;
        case '\t': strbuf_append(&sb, "\\t");  break;
        case '\b': strbuf_append(&sb, "\\b");  break;
        case '\f': strbuf_append(&sb, "\\f");  break;
        default:
            if (c < 0x20) {
                /* Remaining control characters need \u00XX notation. */
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)c);
                strbuf_append(&sb, tmp);
            } else {
                strbuf_append_n(&sb, (const char *)&c, 1);
            }
            break;
        }
    }

    if (sb.data == NULL) {
        /* Empty input: hand back an empty string rather than NULL. */
        return str_dup_n("", 0);
    }
    return sb.data;
}
