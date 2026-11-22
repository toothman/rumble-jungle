// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Starts the HTTP stream server on STREAM_PORT. */
void stream_server_start(void);

/* Queues one encoded ADTS frame for every listener. Never blocks. */
void stream_server_broadcast(const uint8_t *frame, size_t len);

/* Number of currently connected listeners. */
int stream_server_client_count(void);
