#include "valkey_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static redisContext *global_rc = NULL;
static pthread_mutex_t global_rc_mutex = PTHREAD_MUTEX_INITIALIZER;

int init_valkey_client(const char *hostname, int port) {
    global_rc = redisConnect(hostname, port);
    if (global_rc == NULL || global_rc->err) {
        if (global_rc) {
            fprintf(stderr, "Connection error: %s\n", global_rc->errstr);
            redisFree(global_rc);
            global_rc = NULL;
        } else {
            fprintf(stderr, "Connection error: can't allocate redis context\n");
        }
        return -1;
    }
    return 0;
}

char* valkey_execute_rest_command(const char *cmd, const char *key, const char *val) {
    pthread_mutex_lock(&global_rc_mutex);
    if (!global_rc) {
        pthread_mutex_unlock(&global_rc_mutex);
        return NULL;
    }

    redisReply *reply;
    if (val) {
        reply = redisCommand(global_rc, "%s %s %s", cmd, key, val);
    } else if (key) {
        reply = redisCommand(global_rc, "%s %s", cmd, key);
    } else {
        reply = redisCommand(global_rc, "%s", cmd);
    }

    if (!reply) {
        pthread_mutex_unlock(&global_rc_mutex);
        return NULL;
    }

    char *result = NULL;
    char buffer[1024];

    if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS) {
        snprintf(buffer, sizeof(buffer), "{\"result\": \"%s\"}", reply->str);
        result = strdup(buffer);
    } else if (reply->type == REDIS_REPLY_INTEGER) {
        snprintf(buffer, sizeof(buffer), "{\"result\": %lld}", reply->integer);
        result = strdup(buffer);
    } else if (reply->type == REDIS_REPLY_NIL) {
        result = strdup("{\"result\": null}");
    } else if (reply->type == REDIS_REPLY_ERROR) {
        snprintf(buffer, sizeof(buffer), "{\"error\": \"%s\"}", reply->str);
        result = strdup(buffer);
    } else {
        result = strdup("{\"result\": \"OK\"}");
    }

    freeReplyObject(reply);
    pthread_mutex_unlock(&global_rc_mutex);
    return result;
}

void cleanup_valkey_client() {
    pthread_mutex_lock(&global_rc_mutex);
    if (global_rc) {
        redisFree(global_rc);
        global_rc = NULL;
    }
    pthread_mutex_unlock(&global_rc_mutex);
}
