/*
 * console.c - Windows Unicode console I/O, with a portable fallback.
 *
 * Why this exists
 * ---------------
 * A Windows console has two code pages. `chcp` usually reports the output one
 * (frequently 65001/UTF-8 in modern terminals) while the *input* code page is
 * still the legacy ANSI one (936 GBK on a Chinese system, 1252 in the West).
 * Reading a typed prompt with fgets() therefore yields GBK bytes, which are not
 * valid UTF-8, and the API answers:
 *
 *   HTTP 400 ... messages[0].content: invalid unicode code point
 *
 * Writing has the mirror problem: our UTF-8 output is rendered as GBK and the
 * user sees mojibake ("浣犲ソ" instead of "你好").
 *
 * Talking to the console through ReadConsoleW/WriteConsoleW avoids the code
 * pages completely: the console works in UTF-16 and does the rendering itself.
 * When stdin/stdout is a pipe or a file there is no console API to use, so we
 * fall back to plain byte I/O (which is exactly what the test suite drives).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deepseek.h"

#ifdef _WIN32
/* windows.h must not drag in winsock.h, which would clash with winsock2.h
 * (pulled in through curl.h). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#endif

#ifdef _WIN32

static HANDLE g_stdout;         /* console handle, or INVALID_HANDLE_VALUE */
static HANDLE g_stdin;

/* Is this stdio stream attached to a real console (not a pipe or a file)? */
static HANDLE console_handle(FILE *stream, DWORD *mode)
{
    int fd = _fileno(stream);
    intptr_t raw;

    if (fd < 0) {
        return INVALID_HANDLE_VALUE;
    }
    raw = _get_osfhandle(fd);
    if (raw == -1) {
        return INVALID_HANDLE_VALUE;
    }
    if (!GetConsoleMode((HANDLE)raw, mode)) {
        return INVALID_HANDLE_VALUE;   /* redirected */
    }
    return (HANDLE)raw;
}

void console_init(void)
{
    DWORD mode;

    g_stdout = console_handle(stdout, &mode);
    g_stdin = console_handle(stdin, &mode);

    /*
     * Also put the console code pages on UTF-8 where possible. This is not
     * needed for the wide character calls below, but it keeps the console
     * consistent for anything that still goes through stdio. Failures are
     * harmless on consoles that do not support 65001.
     */
    if (g_stdout != INVALID_HANDLE_VALUE) {
        SetConsoleOutputCP(CP_UTF8);
    }
    if (g_stdin != INVALID_HANDLE_VALUE) {
        SetConsoleCP(CP_UTF8);
    }
}

void ds_write(const char *text)
{
    size_t len;
    int widelen;
    WCHAR *wide;

    if (text == NULL) {
        return;
    }
    if (g_stdout == INVALID_HANDLE_VALUE) {
        fputs(text, stdout);           /* redirected: emit UTF-8 bytes as is */
        return;
    }

    /* The C buffer and WriteConsoleW must not interleave out of order. */
    fflush(stdout);

    len = strlen(text);
    if (len == 0) {
        return;
    }
    if (len > (size_t)0x7FFFFFFF) {
        return;
    }

    widelen = MultiByteToWideChar(CP_UTF8, 0, text, (int)len, NULL, 0);
    if (widelen <= 0) {
        fputs(text, stdout);
        return;
    }
    wide = (WCHAR *)malloc((size_t)widelen * sizeof(WCHAR));
    if (wide == NULL) {
        fputs(text, stdout);
        return;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, text, (int)len, wide, widelen) > 0) {
        DWORD written = 0;

        WriteConsoleW(g_stdout, wide, (DWORD)widelen, &written, NULL);
    }
    free(wide);
}

void ds_write_n(const char *data, size_t len)
{
    char stack[512];
    char *copy;

    if (data == NULL || len == 0) {
        return;
    }
    if (len < sizeof(stack)) {
        memcpy(stack, data, len);
        stack[len] = '\0';
        ds_write(stack);
        return;
    }
    copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, data, len);
    copy[len] = '\0';
    ds_write(copy);
    free(copy);
}

char *ds_read_line(char *buffer, size_t size)
{
    if (buffer == NULL || size < 2) {
        return NULL;
    }

    if (g_stdin == INVALID_HANDLE_VALUE) {
        return fgets(buffer, (int)size, stdin);   /* pipe or file: raw bytes */
    }

    /*
     * Read UTF-16 from the console until Enter. Only the first `size` UTF-8
     * bytes are kept, so a very long paste is truncated rather than overrun.
     */
    {
        DWORD read = 0;
        size_t used = 0;
        char utf8[8];

        for (;;) {
            WCHAR wc;

            if (!ReadConsoleW(g_stdin, &wc, 1, &read, NULL) || read == 0) {
                if (used == 0) {
                    return NULL;                 /* EOF */
                }
                break;
            }
            if (wc == L'\r') {
                continue;                        /* wait for the '\n' */
            }
            if (wc == L'\n') {
                break;
            }

            if (wc < 0x80) {
                utf8[0] = (char)wc;
                utf8[1] = '\0';
            } else {
                int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8,
                                            (int)sizeof(utf8) - 1, NULL, NULL);

                if (n <= 0) {
                    continue;                    /* lone surrogate: skip */
                }
                utf8[n] = '\0';
            }

            {
                size_t n = strlen(utf8);

                if (used + n < size) {
                    memcpy(buffer + used, utf8, n);
                    used += n;
                }
            }
        }
        buffer[used] = '\0';
        return buffer;
    }
}

#else /* !_WIN32 */

/* POSIX terminals are UTF-8 in practice, so byte I/O is already correct. */
void console_init(void)
{
}

void ds_write(const char *text)
{
    if (text != NULL) {
        fputs(text, stdout);
    }
}

void ds_write_n(const char *data, size_t len)
{
    if (data != NULL && len > 0) {
        fwrite(data, 1, len, stdout);
    }
}

char *ds_read_line(char *buffer, size_t size)
{
    if (buffer == NULL || size < 2) {
        return NULL;
    }
    return fgets(buffer, (int)size, stdin);
}

#endif /* _WIN32 */
