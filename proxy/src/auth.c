#include "auth.h"
#include "valkey_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE_LIMIT_MAX 100
#define RATE_LIMIT_WINDOW_SECONDS "60"

static bool increment_rate_limit(const char *scope, const char *identity) {
    char key[512];
    char *result;
    int count = 0;

    if (!identity || snprintf(key, sizeof(key), "proxy:rate_limit:%s:%s", scope,
                              identity) >= (int)sizeof(key)) {
        return false;
    }

    result = valkey_execute_rest_command("INCR", key, NULL);
    if (!result || sscanf(result, "{\"result\": %d}", &count) != 1) {
        free(result);
        return false;
    }
    free(result);

    if (count == 1) {
        result = valkey_execute_rest_command("EXPIRE", key,
                                             RATE_LIMIT_WINDOW_SECONDS);
        free(result);
    }
    return count <= RATE_LIMIT_MAX;
}

bool auth_and_rate_limit(const char *token, const char *client_ip) {
    if (!token || token[0] == '\0') return false;

    // 1. Verify token exists in DiceDB (e.g. GET proxy:auth:tokens:<token>)
    char auth_key[512];
    if (snprintf(auth_key, sizeof(auth_key), "proxy:auth:tokens:%s", token) >=
        (int)sizeof(auth_key)) {
        return false;
    }

    char *auth_res = valkey_execute_rest_command("GET", auth_key, NULL);
    if (!auth_res) return false;

    if (strstr(auth_res, "\"result\": null") != NULL ||
        strstr(auth_res, "\"error\"") != NULL) {
        free(auth_res);
        return false;
    }
    free(auth_res);

    return increment_rate_limit("token", token) &&
           increment_rate_limit("ip", client_ip ? client_ip : "unknown");
}
