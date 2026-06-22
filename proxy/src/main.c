#include "http_server.h"
#include "reactivity.h"
#include "valkey_client.h"
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT 8080
#define DEFAULT_UPSTREAM_HOST "127.0.0.1"
#define DEFAULT_UPSTREAM_PORT 6379

static int read_port(const char *value, int fallback) {
    char *end;
    long port;

    if (!value || value[0] == '\0') return fallback;
    port = strtol(value, &end, 10);
    if (*end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", value);
        return -1;
    }
    return (int)port;
}

int main(int argc, char **argv) {
    int port = read_port(getenv("DICEDB_PROXY_PORT"), DEFAULT_PORT);
    const char *upstream_host = getenv("DICEDB_HOST");
    int upstream_port = read_port(getenv("DICEDB_PORT"), DEFAULT_UPSTREAM_PORT);

    if (!upstream_host || upstream_host[0] == '\0') {
        upstream_host = DEFAULT_UPSTREAM_HOST;
    }

    if (argc > 1) {
        port = read_port(argv[1], port);
    }
    if (port < 0 || upstream_port < 0) return 1;

    printf("Starting DiceDB REST API Proxy...\n");
    printf("Connecting to upstream DiceDB at %s:%d\n", upstream_host, upstream_port);

    if (init_valkey_client(upstream_host, upstream_port) != 0) {
        fprintf(stderr, "Failed to connect to upstream DiceDB\n");
        return 1;
    }
    init_reactivity(upstream_host, upstream_port);

    if (start_http_server(port) != 0) {
        fprintf(stderr, "Failed to start HTTP server\n");
        cleanup_valkey_client();
        return 1;
    }

    cleanup_valkey_client();
    return 0;
}
