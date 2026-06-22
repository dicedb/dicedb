#include "http_server.h"
#include "valkey_client.h"
#include "auth.h"
#include "reactivity.h"
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

#define PAGE "{\"error\": \"Not Found\"}"
#define UNAUTH_PAGE "{\"error\": \"Unauthorized or Rate Limited\"}"

static volatile sig_atomic_t server_running = 1;

static void stop_server(int signal_number) {
    (void)signal_number;
    server_running = 0;
}

static enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection,
                                            const char *url, const char *method,
                                            const char *version, const char *upload_data,
                                            size_t *upload_data_size, void **con_cls) {
    (void)cls;               /* Unused */
    (void)version;           /* Unused */
    (void)upload_data;       /* Unused */
    (void)upload_data_size;  /* Unused */

    if (NULL == *con_cls) {
        *con_cls = connection;
        return MHD_YES;
    }

    struct MHD_Response *response;
    enum MHD_Result ret;

    char client_ip[INET6_ADDRSTRLEN] = "unknown";
    const union MHD_ConnectionInfo *connection_info = MHD_get_connection_info(
        connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (connection_info && connection_info->client_addr) {
        const struct sockaddr *addr = connection_info->client_addr;
        if (addr->sa_family == AF_INET) {
            inet_ntop(AF_INET, &((const struct sockaddr_in *)addr)->sin_addr,
                      client_ip, sizeof(client_ip));
        } else if (addr->sa_family == AF_INET6) {
            inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)addr)->sin6_addr,
                      client_ip, sizeof(client_ip));
        }
    }

    // Extract Bearer token
    const char *auth_header = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
    char *token = NULL;
    if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0) {
        token = strdup(auth_header + 7);
    }

    // Authenticate and Rate Limit
    if (!auth_and_rate_limit(token, client_ip)) {
        response = MHD_create_response_from_buffer(strlen(UNAUTH_PAGE), (void *)UNAUTH_PAGE, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_UNAUTHORIZED, response);
        MHD_destroy_response(response);
        if (token) free(token);
        return ret;
    }
    if (token) free(token);

    // Handle OBSERVE Reactivity endpoint
    if (strncmp(url, "/OBSERVE/", 9) == 0) {
        const char *key = url + 9;
        return handle_observe_request(connection, key);
    }

    char *json_response = NULL;

    if (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0) {
        char *url_copy = strdup(url);
        char *cmd = NULL;
        char *key = NULL;
        char *val = NULL;

        char *t = strtok(url_copy, "/");
        if (t) {
            cmd = t;
            t = strtok(NULL, "/");
            if (t) {
                key = t;
                t = strtok(NULL, "/");
                if (t) val = t;
            }
        }

        if (cmd) {
            json_response = valkey_execute_rest_command(cmd, key, val);
        }
        free(url_copy);
    }

    if (json_response) {
        response = MHD_create_response_from_buffer(strlen(json_response),
                                                   (void *)json_response,
                                                   MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(response, "Content-Type", "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    } else {
        response = MHD_create_response_from_buffer(strlen(PAGE), (void *)PAGE, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    }

    MHD_destroy_response(response);
    return ret;
}

int start_http_server(int port) {
    struct MHD_Daemon *daemon;
    daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION, port, NULL, NULL,
                              &answer_to_connection, NULL, MHD_OPTION_END);
    if (NULL == daemon) {
        return 1;
    }
    printf("REST API Proxy listening on port %d\n", port);

    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);

    while(server_running) {
        sleep(1);
    }

    MHD_stop_daemon(daemon);
    return 0;
}
