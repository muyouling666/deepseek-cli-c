/*
 * conversation.c - the multi-turn history.
 *
 * The history is a plain C array of cJSON objects, each shaped like
 * {"role":"user","content":"..."}. It is serialised into the
 * "messages" array of the request payload on every call, which is what
 * makes the dialogue multi-turn.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deepseek.h"

void conv_init(conversation_t *c)
{
    c->messages = NULL;
    c->count = 0;
    c->cap = 0;
}

void conv_free(conversation_t *c)
{
    size_t i;

    for (i = 0; i < c->count; i++) {
        cJSON_Delete(c->messages[i]);
    }
    free(c->messages);
    conv_init(c);
}

void conv_reset(conversation_t *c)
{
    conv_free(c);
}

static int conv_push(conversation_t *c, cJSON *message)
{
    if (c->count == c->cap) {
        size_t cap = (c->cap != 0) ? c->cap * 2 : 8;
        cJSON **grown = (cJSON **)realloc(c->messages, cap * sizeof(cJSON *));

        if (grown == NULL) {
            return -1;
        }
        c->messages = grown;
        c->cap = cap;
    }
    c->messages[c->count++] = message;
    return 0;
}

int conv_add(conversation_t *c, const char *role, const char *content)
{
    cJSON *message = cJSON_CreateObject();

    if (message == NULL) {
        return -1;
    }
    if (cJSON_AddStringToObject(message, "role", role) == NULL ||
        cJSON_AddStringToObject(message, "content", content != NULL ? content : "") == NULL) {
        cJSON_Delete(message);   /* unparented so far, safe to free */
        return -1;
    }
    if (conv_push(c, message) != 0) {
        cJSON_Delete(message);
        return -1;
    }
    return 0;
}

cJSON *conv_get(const conversation_t *c, size_t index)
{
    if (index >= c->count) {
        return NULL;
    }
    return cJSON_Duplicate(c->messages[index], 1);
}

cJSON *conv_build_request(const conversation_t *c, const options_t *opt)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    size_t i;

    if (root == NULL || messages == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        return NULL;
    }

    /* cJSON_AddStringToObject returns the new node, cJSON_AddItemToObject a
     * boolean, so check them separately. */
    if (cJSON_AddStringToObject(root, "model", opt->model) == NULL) {
        cJSON_Delete(messages);
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_AddItemToObject(root, "messages", messages)) {
        cJSON_Delete(messages);      /* not parented: free by hand */
        cJSON_Delete(root);
        return NULL;
    }

    /*
     * Attach the array to the root before filling it: from here on
     * cJSON_Delete(root) owns every element.
     */
    for (i = 0; i < c->count; i++) {
        cJSON *copy = cJSON_Duplicate(c->messages[i], 1);

        if (copy == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(messages, copy);
    }

    if (opt->temperature >= 0.0) {
        cJSON_AddNumberToObject(root, "temperature", opt->temperature);
    }
    if (opt->max_tokens > 0) {
        cJSON_AddNumberToObject(root, "max_tokens", (double)opt->max_tokens);
    }
    if (opt->stream) {
        cJSON_AddBoolToObject(root, "stream", 1);
    }

    return root;
}

/* --------------------------------------------------------------- */
/* Session persistence                                              */
/* --------------------------------------------------------------- */

int conv_save(const conversation_t *c, const char *path, char *err, size_t errlen)
{
    strbuf_t out;
    FILE *fp;
    size_t i;

    strbuf_init(&out);
    strbuf_append(&out, "{\n  \"messages\": [\n");

    for (i = 0; i < c->count; i++) {
        const cJSON *message = c->messages[i];
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
        char *esc_role;
        char *esc_content;

        if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
            continue;
        }
        esc_role = json_escape(role->valuestring);
        esc_content = json_escape(content->valuestring);
        if (esc_role == NULL || esc_content == NULL) {
            free(esc_role);
            free(esc_content);
            strbuf_free(&out);
            if (err != NULL) {
                snprintf(err, errlen, "out of memory");
            }
            return -1;
        }

        strbuf_append(&out, "    {\"role\": \"");
        strbuf_append(&out, esc_role);
        strbuf_append(&out, "\", \"content\": \"");
        strbuf_append(&out, esc_content);
        strbuf_append(&out, "\"}");
        strbuf_append(&out, (i + 1 < c->count) ? ",\n" : "\n");

        free(esc_role);
        free(esc_content);
    }

    strbuf_append(&out, "  ]\n}\n");

    fp = fopen(path, "wb");
    if (fp == NULL) {
        strbuf_free(&out);
        if (err != NULL) {
            snprintf(err, errlen, "cannot open '%s' for writing", path);
        }
        return -1;
    }
    if (out.data != NULL && fwrite(out.data, 1, out.len, fp) != out.len) {
        fclose(fp);
        strbuf_free(&out);
        if (err != NULL) {
            snprintf(err, errlen, "write failed for '%s'", path);
        }
        return -1;
    }
    fclose(fp);
    strbuf_free(&out);
    return 0;
}

int conv_load(conversation_t *c, const char *path, char *err, size_t errlen)
{
    strbuf_t in;
    FILE *fp;
    char chunk[4096];
    size_t got;
    cJSON *root;
    const cJSON *messages;
    const cJSON *message;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (err != NULL) {
            snprintf(err, errlen, "cannot open '%s' for reading", path);
        }
        return -1;
    }

    strbuf_init(&in);
    while ((got = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (strbuf_append_n(&in, chunk, got) != 0) {
            fclose(fp);
            strbuf_free(&in);
            if (err != NULL) {
                snprintf(err, errlen, "out of memory");
            }
            return -1;
        }
    }
    fclose(fp);

    if (in.data == NULL) {
        strbuf_free(&in);
        if (err != NULL) {
            snprintf(err, errlen, "'%s' is empty", path);
        }
        return -1;
    }

    root = cJSON_Parse(in.data);
    strbuf_free(&in);
    if (root == NULL) {
        if (err != NULL) {
            snprintf(err, errlen, "'%s' is not valid JSON", path);
        }
        return -1;
    }

    messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!cJSON_IsArray(messages)) {
        cJSON_Delete(root);
        if (err != NULL) {
            snprintf(err, errlen, "'%s' has no \"messages\" array", path);
        }
        return -1;
    }

    conv_reset(c);
    cJSON_ArrayForEach(message, messages) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");

        if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
            continue;
        }
        if (conv_add(c, role->valuestring, content->valuestring) != 0) {
            cJSON_Delete(root);
            if (err != NULL) {
                snprintf(err, errlen, "out of memory");
            }
            return -1;
        }
    }

    cJSON_Delete(root);
    return 0;
}
