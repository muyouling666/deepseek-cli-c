/*
 * http.c - libcurl wrapper: one POST request, response collected in memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "deepseek.h"

int http_init(void)
{
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);

    if (rc != CURLE_OK) {
        fprintf(stderr, "error: curl_global_init failed: %s\n", curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

void http_cleanup(void)
{
    curl_global_cleanup();
}

int http_post_json(const char *url, const char *api_key, const char *body,
                   curl_write_callback write_fn, void *write_data,
                   long *http_code, char *err, size_t errlen)
{
    CURL *curl;
    CURLcode rc;
    struct curl_slist *headers = NULL;
    curl_write_callback sink;
    char auth_header[512];
    int result = -1;

    *http_code = 0;
    if (err != NULL && errlen > 0) {
        err[0] = '\0';
    }
    sink = (write_fn != NULL) ? write_fn : write_to_strbuf;

    curl = curl_easy_init();
    if (curl == NULL) {
        if (err != NULL) {
            snprintf(err, errlen, "curl_easy_init failed");
        }
        return -1;
    }

    /* --- headers ---------------------------------------------------- */
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    if (api_key != NULL && api_key[0] != '\0') {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth_header);
    }

    /* --- request ---------------------------------------------------- */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "deepseek-cli/1.0");

    /* --- behaviour -------------------------------------------------- */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* let curl gunzip */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_data);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err);

    rc = curl_easy_perform(curl);

    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
        result = 0;
    } else if (err != NULL && err[0] == '\0') {
        snprintf(err, errlen, "%s", curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}
