#include "ws_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Portable case-insensitive substring search (avoids the non-standard
 * strcasestr and its per-platform feature-test macros). */
static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nlen && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return hay;
    }
    return NULL;
}

/* ============================ SHA-1 ============================ */
/* Minimal SHA-1, used only for the WebSocket opening handshake. */

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buf[64];
    size_t buflen;
} sha1_ctx;

static uint32_t rol32(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = c->state[0], b = c->state[1], d = c->state[2], e = c->state[3], f = c->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t t, k;
        if (i < 20) { t = (b & d) | (~b & e); k = 0x5A827999; }
        else if (i < 40) { t = b ^ d ^ e; k = 0x6ED9EBA1; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e); k = 0x8F1BBCDC; }
        else { t = b ^ d ^ e; k = 0xCA62C1D6; }
        uint32_t tmp = rol32(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol32(b, 30); b = a; a = tmp;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += d; c->state[3] += e; c->state[4] += f;
}

static void sha1_init(sha1_ctx *c)
{
    c->state[0] = 0x67452301; c->state[1] = 0xEFCDAB89; c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476; c->state[4] = 0xC3D2E1F0;
    c->count = 0; c->buflen = 0;
}

static void sha1_update(sha1_ctx *c, const uint8_t *data, size_t len)
{
    c->count += (uint64_t)len * 8;
    while (len > 0) {
        size_t n = 64 - c->buflen;
        if (n > len) n = len;
        memcpy(c->buf + c->buflen, data, n);
        c->buflen += n; data += n; len -= n;
        if (c->buflen == 64) { sha1_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->count;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) sha1_update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, lenbuf, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

/* ============================ base64 ============================ */

static void base64_encode(const uint8_t *in, size_t len, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = t[(v >> 18) & 0x3F];
        out[o++] = t[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? t[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? t[v & 0x3F] : '=';
    }
    out[o] = '\0';
}

/* ============================ connection ============================ */

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_MSG 8192

typedef struct {
    int fd;
    void *userdata;
} ws_conn_t;

void *ws_conn_userdata(void *conn) { return ((ws_conn_t *)conn)->userdata; }

/* Guards the raw frame write (header + payload as one unit) so a background
 * thread's unsolicited push (ws_server_send_to_active) can never interleave
 * with the read loop's response to a client request. Guards g_active
 * separately so a send-in-progress doesn't block connection teardown from
 * clearing it. */
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_active_lock = PTHREAD_MUTEX_INITIALIZER;
static ws_conn_t *g_active = NULL;

static int read_fully(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_fully(int fd, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, buf + sent, n - sent);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static void send_frame_locked(int fd, const uint8_t *data, size_t len)
{
    uint8_t header[10];
    size_t hlen = 0;
    header[0] = 0x82; /* FIN + binary */
    if (len < 126) {
        header[1] = (uint8_t)len;
        hlen = 2;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) header[2 + i] = (uint8_t)(len >> (56 - i * 8));
        hlen = 10;
    }
    pthread_mutex_lock(&g_write_lock);
    if (write_fully(fd, header, hlen) == 0 && len > 0) {
        write_fully(fd, data, len);
    }
    pthread_mutex_unlock(&g_write_lock);
}

void ws_send(void *conn, const uint8_t *data, size_t len)
{
    send_frame_locked(((ws_conn_t *)conn)->fd, data, len);
}

bool ws_server_send_to_active(const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&g_active_lock);
    int fd = (g_active != NULL) ? g_active->fd : -1;
    pthread_mutex_unlock(&g_active_lock);
    if (fd < 0) {
        return false;
    }
    send_frame_locked(fd, data, len);
    return true;
}

void ws_server_close_active(void)
{
    pthread_mutex_lock(&g_active_lock);
    if (g_active != NULL) {
        /* shutdown(), not close(): fd ownership stays with the read-loop
         * thread, which will observe read() return and call close() itself
         * during its own teardown. Calling close() here from a different
         * thread would race that thread's use of the same fd. */
        shutdown(g_active->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_active_lock);
}

/* Reads the HTTP upgrade request, replies with the 101 handshake. Returns 0
 * on success. */
static int do_handshake(int fd)
{
    char req[2048];
    size_t total = 0;
    /* Read until the end of headers (\r\n\r\n). */
    while (total < sizeof(req) - 1) {
        ssize_t r = read(fd, req + total, sizeof(req) - 1 - total);
        if (r <= 0) return -1;
        total += (size_t)r;
        req[total] = '\0';
        if (strstr(req, "\r\n\r\n") != NULL) break;
    }

    const char *key_hdr = ci_strstr(req, "Sec-WebSocket-Key:");
    if (key_hdr == NULL) return -1;
    key_hdr += strlen("Sec-WebSocket-Key:");
    while (*key_hdr == ' ') key_hdr++;
    char key[128];
    size_t klen = 0;
    while (*key_hdr && *key_hdr != '\r' && *key_hdr != '\n' && klen < sizeof(key) - 1) {
        key[klen++] = *key_hdr++;
    }
    key[klen] = '\0';

    char concat[256];
    snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
    uint8_t digest[20];
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, (const uint8_t *)concat, strlen(concat));
    sha1_final(&c, digest);
    char accept[64];
    base64_encode(digest, 20, accept);

    char resp[256];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
    return write_fully(fd, (const uint8_t *)resp, (size_t)n);
}

/* Reads one frame. Returns opcode (>=0) and fills msg/msg_len for data
 * frames, or -1 on error/close. */
static int read_frame(int fd, uint8_t *msg, size_t cap, size_t *msg_len)
{
    uint8_t h[2];
    if (read_fully(fd, h, 2) != 0) return -1;
    int opcode = h[0] & 0x0F;
    int masked = h[1] & 0x80;
    uint64_t len = h[1] & 0x7F;

    if (len == 126) {
        uint8_t ext[2];
        if (read_fully(fd, ext, 2) != 0) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (read_fully(fd, ext, 8) != 0) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > cap) return -1; /* message too large for our buffer */

    uint8_t mask[4] = {0};
    if (masked && read_fully(fd, mask, 4) != 0) return -1;

    if (len > 0 && read_fully(fd, msg, (size_t)len) != 0) return -1;
    if (masked) {
        for (uint64_t i = 0; i < len; i++) msg[i] ^= mask[i % 4];
    }
    *msg_len = (size_t)len;
    return opcode;
}

static void serve_connection(int fd, const ws_callbacks_t *cb)
{
    if (do_handshake(fd) != 0) {
        close(fd);
        return;
    }

    ws_conn_t conn = { .fd = fd, .userdata = NULL };
    if (cb->on_open != NULL) {
        conn.userdata = cb->on_open(cb->user);
    }

    uint8_t *msg = malloc(WS_MAX_MSG);
    if (msg == NULL) {
        close(fd);
        return;
    }

    pthread_mutex_lock(&g_active_lock);
    g_active = &conn;
    pthread_mutex_unlock(&g_active_lock);

    for (;;) {
        size_t len = 0;
        int opcode = read_frame(fd, msg, WS_MAX_MSG, &len);
        if (opcode < 0 || opcode == 0x8 /* close */) {
            break;
        }
        if (opcode == 0x9 /* ping */) {
            /* Reply pong with the same payload. */
            uint8_t hdr[2] = { 0x8A, (uint8_t)(len < 126 ? len : 0) };
            write_fully(fd, hdr, 2);
            if (len < 126 && len > 0) write_fully(fd, msg, len);
            continue;
        }
        if (opcode == 0x2 /* binary */ && cb->on_message != NULL) {
            cb->on_message(cb->user, &conn, msg, len);
        }
        /* text (0x1) / continuation (0x0): ignored */
    }

    pthread_mutex_lock(&g_active_lock);
    g_active = NULL;
    pthread_mutex_unlock(&g_active_lock);

    free(msg);
    if (cb->on_close != NULL) {
        cb->on_close(cb->user, conn.userdata);
    }
    close(fd);
}

int ws_server_run(uint16_t port, const ws_callbacks_t *cb)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 4) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("moto-ctrl-sim listening ws://127.0.0.1:%u\n", (unsigned)port);
    fflush(stdout);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) continue;
        serve_connection(fd, cb);
    }
}
