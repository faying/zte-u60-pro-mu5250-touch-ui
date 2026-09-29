/*
 * agent_client.h - logged-in requests to zte-agent (127.0.0.1:9090).
 *
 * One password lookup, one token and one 401 → log in again → retry rule for
 * every touch-UI module (alerts, eSIM, speed test, network info, scenario,
 * …). Each caller keeps its own response buffer, so a body stays valid
 * until that caller's next request, as before.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AGENT_CLIENT_H
#define AGENT_CLIENT_H

#include <stddef.h>

#define AGENT_PORT_DEFAULT 9090
#define AGENT_IO_MS        1500

/* Override port / password (tests). The device reads both from the files in
 * agent_client.c on first use, including the optional esim.conf override.
 * port <= 0 or pass NULL/"" keeps the current value. Clears the token. */
void agent_client_config(int port, const char *pass);

/* 1 when a password was found (files are read on first use). */
int agent_has_password(void);

/* Logged-in request. json NULL = empty body. Returns the HTTP status
 * (0 = agent unreachable or no usable reply); *body points into buf
 * (NULL on 0). The agent answers 401 before running any handler, so
 * logging in again and repeating the request once is safe for every method. */
int agent_api(const char *method, const char *path, const char *json,
              char *buf, size_t cap, char **body);

/* Same with a caller-chosen per-step timeout (the home screen's quick reads). */
int agent_api_ms(const char *method, const char *path, const char *json,
                 char *buf, size_t cap, int io_ms, char **body);

/* Request without a token (login itself, /api/public/status). */
int agent_http(const char *method, const char *path, const char *json,
               char *buf, size_t cap, int io_ms, char **body);

/* Fire a logged-in request and drop the body. Returns the HTTP status. */
int agent_request(const char *method, const char *path, const char *json);
int agent_post(const char *path);

#endif
