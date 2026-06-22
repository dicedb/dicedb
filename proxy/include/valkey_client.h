#ifndef VALKEY_CLIENT_H
#define VALKEY_CLIENT_H

#include <hiredis.h>

/* Initialize the Valkey client connection pool */
int init_valkey_client(const char *hostname, int port);

/* Execute a REST command and get the JSON response back.
   The caller must free the returned string. */
char* valkey_execute_rest_command(const char *cmd, const char *key, const char *val);

/* Close all Valkey connections */
void cleanup_valkey_client();

#endif // VALKEY_CLIENT_H
