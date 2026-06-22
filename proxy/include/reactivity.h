#ifndef REACTIVITY_H
#define REACTIVITY_H

#include <microhttpd.h>

/* Handle an SSE /OBSERVE request */
enum MHD_Result handle_observe_request(struct MHD_Connection *connection, const char *key);

void init_reactivity(const char *hostname, int port);

#endif // REACTIVITY_H
