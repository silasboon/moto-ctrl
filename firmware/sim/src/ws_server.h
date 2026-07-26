#pragma once

/*
 * Minimal self-contained WebSocket (RFC 6455) server for the MOTO-CTRL host
 * simulator — no external library (the handshake's SHA-1 + base64 are
 * implemented inline). Single active connection at a time: the simulator
 * stands in for one bike talking to one client, which is all the app and CI
 * need. The accept/read loop itself is single-threaded and synchronous, but
 * ws_send() and ws_server_send_to_active() are safe to call concurrently
 * from another thread (the ticker thread pushes status/event-log frames
 * while the read loop blocks waiting for the next client message).
 *
 * Assumes each application message arrives as a single, unfragmented binary
 * frame (the MOTO-CTRL protocol frames are small). Ping is answered with
 * pong; text/continuation frames are ignored.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opens a connection: return a per-connection userdata pointer (e.g. a
 * freshly allocated mc_session_t) that on_message/on_close receive back. */
typedef void *(*ws_on_open_fn)(void *user);

/* One complete inbound binary message. `conn` is the opaque handle to pass
 * to ws_send()/ws_conn_userdata(). */
typedef void (*ws_on_message_fn)(void *user, void *conn, const uint8_t *data, size_t len);

/* Connection closed; free whatever on_open allocated. */
typedef void (*ws_on_close_fn)(void *user, void *userdata);

typedef struct {
    ws_on_open_fn on_open;
    ws_on_message_fn on_message;
    ws_on_close_fn on_close;
    void *user;
} ws_callbacks_t;

/* The userdata returned by on_open for this connection. */
void *ws_conn_userdata(void *conn);

/* Sends one binary message to the connection. Thread-safe with respect to
 * other ws_send()/ws_server_send_to_active() calls. */
void ws_send(void *conn, const uint8_t *data, size_t len);

/* Sends one binary message to whichever connection is currently active, if
 * any. Returns false if there is no active connection. Unlike ws_send(),
 * this doesn't require a `conn` handle, so it's the one a background thread
 * (no `conn` of its own) uses to push unsolicited frames. Thread-safe. */
bool ws_server_send_to_active(const uint8_t *data, size_t len);

/* Shuts down the active connection's socket (if any), unblocking the read
 * loop's blocking read() so it observes a close and runs on_close/teardown
 * itself. Used to simulate a forced BLE disconnect or an MCU reboot (which
 * drops the link) from a thread other than the one running the read loop.
 * Safe to call with no active connection (no-op). */
void ws_server_close_active(void);

/* Runs the server on 127.0.0.1:port. Blocks forever serving connections.
 * Returns non-zero on fatal setup error (e.g. bind failure). Prints a
 * "listening" line to stdout once ready. */
int ws_server_run(uint16_t port, const ws_callbacks_t *cb);
