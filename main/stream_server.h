// SPDX-License-Identifier: MIT
#pragma once

/* Starts the HTTP stream server on STREAM_PORT. */
void stream_server_start(void);

/* Number of currently connected listeners. */
int stream_server_client_count(void);
