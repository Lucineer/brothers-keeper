/**
 * keeper-client.c — C SDK implementation for brothers-keeper key server
 *
 * Uses raw TCP sockets (no libcurl dependency) to talk to the keeper.
 * Keeper runs on localhost:9437.
 */

#define _POSIX_C_SOURCE 200809L
#include "keeper-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ═══ HTTP helper — minimal HTTP/1.1 client ═══ */

static int http_post(const char *host, int port, const char *path,
                     const char *body, const char *session_header,
                     char *response, int max_len) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -2;
    }

    /* Build request */
    char req[4096];
    int pos = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n",
        path, host, port, (int)strlen(body));

    if (session_header && session_header[0]) {
        pos += snprintf(req + pos, sizeof(req) - pos,
            "X-Keeper-Session: %s\r\n", session_header);
    }
    pos += snprintf(req + pos, sizeof(req) - pos, "\r\n%s", body);

    send(sock, req, strlen(req), 0);

    /* Read response */
    int total = 0;
    int body_start = -1;
    while (total < max_len - 1) {
        int n = recv(sock, response + total, max_len - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    response[total] = 0;
    close(sock);

    /* Find body (after \r\n\r\n) */
    char *body_ptr = strstr(response, "\r\n\r\n");
    if (!body_ptr) return -3;
    body_ptr += 4;

    /* Move body to start of buffer */
    int body_len = strlen(body_ptr);
    memmove(response, body_ptr, body_len + 1);

    /* Check status code */
    int status = 0;
    if (sscanf(response, "%*s %d", &status) == 0) status = 200;

    /* Actually parse from original response */
    const char *status_line = response; /* Already moved body */
    /* We moved body already, so just check if body starts with { */
    return (body_ptr[0] == '{') ? 0 : -3;
}

/* Same for GET */
static int http_get(const char *host, int port, const char *path,
                    const char *session_header,
                    char *response, int max_len) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -2;
    }

    char req[2048];
    int pos = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n",
        path, host, port);

    if (session_header && session_header[0]) {
        pos += snprintf(req + pos, sizeof(req) - pos,
            "X-Keeper-Session: %s\r\n", session_header);
    }
    pos += snprintf(req + pos, sizeof(req) - pos, "\r\n");

    send(sock, req, strlen(req), 0);

    int total = 0;
    while (total < max_len - 1) {
        int n = recv(sock, response + total, max_len - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    response[total] = 0;
    close(sock);

    char *body_ptr = strstr(response, "\r\n\r\n");
    if (!body_ptr) return -3;
    body_ptr += 4;
    memmove(response, body_ptr, strlen(body_ptr) + 1);
    return 0;
}

/* ═══ Simple JSON extraction ═══ */

static const char *json_get_string(const char *json, const char *key, char *out, int max_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) { if (out) out[0] = 0; return NULL; }

    pos += strlen(search);
    while (*pos == ' ' || *pos == ':' || *pos == '\t') pos++;
    if (*pos != '"') { if (out) out[0] = 0; return NULL; }
    pos++; /* skip opening quote */

    int i = 0;
    while (*pos && *pos != '"' && i < max_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = 0;
    return out;
}

static float json_get_float(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0.0f;

    pos += strlen(search);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    return strtof(pos, NULL);
}

/* ═══ API ═══ */

int keeper_init(KeeperClient *client, const char *agent_name, const char *auth_token) {
    if (!client || !agent_name || !auth_token) return -1;
    memset(client, 0, sizeof(*client));
    strncpy(client->agent_name, agent_name, sizeof(client->agent_name) - 1);
    strncpy(client->auth_token, auth_token, sizeof(client->auth_token) - 1);
    client->authenticated = 0;
    return 0;
}

int keeper_authenticate(KeeperClient *client) {
    if (!client) return -1;

    char body[256];
    snprintf(body, sizeof(body),
        "{\"agent\":\"%s\",\"token\":\"%s\"}",
        client->agent_name, client->auth_token);

    char response[2048];
    int rc = http_post(KEEPER_HOST, KEEPER_DEFAULT_PORT, "/auth", body,
                       NULL, response, sizeof(response));
    if (rc != 0) {
        snprintf(client->last_error, sizeof(client->last_error),
            "auth failed: HTTP error %d", rc);
        return rc;
    }

    /* Parse session token */
    char session[KEEPER_SESSION_LEN + 1];
    if (json_get_string(response, "session", session, sizeof(session))) {
        strncpy(client->session, session, KEEPER_SESSION_LEN);
        client->authenticated = 1;
        client->daily_limit_usd = json_get_float(response, "daily_limit_usd");
        client->used_today_usd = json_get_float(response, "used_today_usd");
        client->remaining_usd = json_get_float(response, "remaining_usd");
        return 0;
    }

    /* Check for error */
    char error[KEEPER_MAX_ERROR];
    if (json_get_string(response, "error", error, sizeof(error))) {
        strncpy(client->last_error, error, sizeof(client->last_error));
    }
    return -3;
}

int keeper_get_key(KeeperClient *client, const char *provider,
                   char *key_out, int key_max_len) {
    return keeper_get_key_budgeted(client, provider, key_out, key_max_len, 0.001f);
}

int keeper_get_key_budgeted(KeeperClient *client, const char *provider,
                            char *key_out, int key_max_len,
                            float estimated_cost_usd) {
    if (!client || !provider || !key_out) return -1;

    if (!client->authenticated) {
        snprintf(client->last_error, sizeof(client->last_error),
            "not authenticated — call keeper_authenticate first");
        return -2;
    }

    char body[256];
    snprintf(body, sizeof(body),
        "{\"estimated_cost_usd\":%.6f}", estimated_cost_usd);

    char path[128];
    snprintf(path, sizeof(path), "/key/%s", provider);

    char response[4096];
    int rc = http_post(KEEPER_HOST, KEEPER_DEFAULT_PORT, path, body,
                       client->session, response, sizeof(response));
    if (rc != 0) {
        snprintf(client->last_error, sizeof(client->last_error),
            "key request failed: %d", rc);
        return rc;
    }

    /* Parse key */
    if (json_get_string(response, "key", key_out, key_max_len)) {
        client->remaining_usd = json_get_float(response, "remaining_usd");
        return 0;
    }

    /* Check error */
    char error[KEEPER_MAX_ERROR];
    if (json_get_string(response, "error", error, sizeof(error))) {
        strncpy(client->last_error, error, sizeof(client->last_error));
    }
    return -3;
}

int keeper_report_usage(KeeperClient *client, const char *provider,
                        float cost_usd) {
    if (!client || !provider) return -1;
    /* Usage is tracked server-side when key is requested.
       This is for post-call actual cost adjustment. */
    (void)provider;
    (void)cost_usd;
    /* Future: POST /usage endpoint */
    return 0;
}

int keeper_check_budget(KeeperClient *client) {
    if (!client || !client->authenticated) return -1;

    char path[128];
    snprintf(path, sizeof(path), "/budget/%s", client->agent_name);

    char response[1024];
    int rc = http_get(KEEPER_HOST, KEEPER_DEFAULT_PORT, path,
                      client->session, response, sizeof(response));
    if (rc != 0) return rc;

    client->daily_limit_usd = json_get_float(response, "daily_limit_usd");
    client->used_today_usd = json_get_float(response, "used_today_usd");
    client->remaining_usd = json_get_float(response, "remaining_usd");
    return 0;
}

int keeper_register(const char *agent_name, float daily_limit_usd,
                    char *auth_token_out, int token_max_len) {
    if (!agent_name || !auth_token_out) return -1;

    char body[256];
    snprintf(body, sizeof(body),
        "{\"agent\":\"%s\",\"daily_limit_usd\":%.2f}",
        agent_name, daily_limit_usd);

    char path[128];
    snprintf(path, sizeof(path), "/register/%s", agent_name);

    char response[1024];
    int rc = http_post(KEEPER_HOST, KEEPER_DEFAULT_PORT, path, body,
                       NULL, response, sizeof(response));
    if (rc != 0) return rc;

    char token[64];
    if (json_get_string(response, "token", token, sizeof(token))) {
        strncpy(auth_token_out, token, token_max_len);
        return 0;
    }
    return -3;
}

/* ═══ Tests ═══ */

int keeper_client_test(void) {
    int failures = 0;

    /* Test 1: Init */
    KeeperClient client;
    int rc = keeper_init(&client, "test-agent", "test-token");
    if (rc != 0) { failures++; printf("FAIL init\n"); }
    if (strcmp(client.agent_name, "test-agent") != 0) { failures++; printf("FAIL agent name\n"); }
    printf("  Init: OK\n");

    /* Test 2: JSON parsing */
    const char *json = "{\"key\":\"sk-abc123\",\"remaining_usd\":0.95,\"session\":\"sess123\"}";
    char val[64];

    rc = json_get_string(json, "key", val, sizeof(val));
    if (rc && strcmp(val, "sk-abc123") == 0) printf("  JSON string: OK\n");
    else { failures++; printf("FAIL json string: '%s'\n", val); }

    float f = json_get_float(json, "remaining_usd");
    if (f > 0.9f && f < 1.0f) printf("  JSON float: OK (%.2f)\n", f);
    else { failures++; printf("FAIL json float: %f\n", f); }

    rc = json_get_string(json, "nonexistent", val, sizeof(val));
    if (!rc) printf("  JSON missing key: OK\n");
    else { failures++; printf("FAIL json missing: '%s'\n", val); }

    /* Test 3: Null safety */
    rc = keeper_init(NULL, "a", "b");
    if (rc != -1) { failures++; printf("FAIL null init\n"); }

    rc = keeper_authenticate(NULL);
    if (rc != -1) { failures++; printf("FAIL null auth\n"); }

    rc = keeper_get_key(NULL, "test", val, sizeof(val));
    if (rc != -1) { failures++; printf("FAIL null get_key\n"); }

    /* Test 4: Auth without server (should fail gracefully) */
    rc = keeper_authenticate(&client);
    if (rc != 0) {
        printf("  Auth without server: failed as expected (%d: %s)\n",
               rc, client.last_error);
    } else {
        /* Server might actually be running! */
        printf("  Auth: connected to server (unexpected but OK)\n");
    }

    /* Test 5: Get key without auth (should fail) */
    char key[256];
    rc = keeper_get_key(&client, "deepinfra", key, sizeof(key));
    if (rc != 0) {
        printf("  Key without auth: failed as expected (%d)\n", rc);
    }

    printf("keeper_client_test: %d failures\n", failures);
    return failures;
}
