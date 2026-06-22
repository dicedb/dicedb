#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>

/* Validates a bearer token and applies per-token and per-IP rate limits. */
bool auth_and_rate_limit(const char *token, const char *client_ip);

#endif // AUTH_H
