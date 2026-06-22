#include "reactivity.h"
#include <hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *upstream_host;
static int upstream_port;

void init_reactivity(const char *hostname, int port) {
    upstream_host = hostname;
    upstream_port = port;
}

// SSE context per connection
struct sse_context {
    redisContext *rc;
    char *key;
};

// Callback to read SSE chunks.
static ssize_t sse_reader_callback(void *cls, uint64_t pos, char *buf, size_t max) {
    struct sse_context *ctx = (struct sse_context *)cls;
    (void)pos;

    // Block and wait for reply from OBSERVE
    redisReply *reply;
    if (redisGetReply(ctx->rc, (void**)&reply) == REDIS_OK) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements > 0) {
            // OBSERVE pushes arrays
            // Format as SSE: data: {"event": ...} \n\n
            // Simplified string dump for this POC
            char *json = "{\"event\": \"update\"}";
            if (reply->elements > 1 && reply->element[1]->type == REDIS_REPLY_STRING) {
                json = reply->element[1]->str;
            }

            int written = snprintf(buf, max, "data: %s\n\n", json);
            freeReplyObject(reply);
            if (written < 0) return MHD_CONTENT_READER_END_WITH_ERROR;
            return (size_t)written < max ? written : (ssize_t)max;
        }
        freeReplyObject(reply);

        // Return 0 means EOF / close connection? No, for SSE we need to keep waiting.
        // Returning 0 might close the connection in MHD if we aren't suspended.
        // Actually, returning a space or keepalive might be better, but let's just
        // wait for the next event. If we just sleep and retry, it's better.
    }

    // If redisGetReply fails or connection drops
    return MHD_CONTENT_READER_END_OF_STREAM;
}

static void sse_free_callback(void *cls) {
    struct sse_context *ctx = (struct sse_context *)cls;
    if (ctx) {
        if (ctx->rc) redisFree(ctx->rc);
        if (ctx->key) free(ctx->key);
        free(ctx);
    }
}

enum MHD_Result handle_observe_request(struct MHD_Connection *connection, const char *key) {
    struct sse_context *ctx = malloc(sizeof(struct sse_context));
    if (!ctx) return MHD_NO;
    ctx->key = strdup(key);
    ctx->rc = redisConnect(upstream_host, upstream_port);

    if (!ctx->key || ctx->rc == NULL || ctx->rc->err) {
        sse_free_callback(ctx);
        return MHD_NO;
    }

    // Send the OBSERVE command
    redisReply *reply = redisCommand(ctx->rc, "OBSERVE %s", key);
    if (reply) freeReplyObject(reply); // Initial OK/Subscription reply

    struct MHD_Response *response = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, 1024, &sse_reader_callback, ctx, &sse_free_callback);
    if (!response) {
        sse_free_callback(ctx);
        return MHD_NO;
    }

    MHD_add_response_header(response, "Content-Type", "text/event-stream");
    MHD_add_response_header(response, "Cache-Control", "no-cache");
    MHD_add_response_header(response, "Connection", "keep-alive");

    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}
