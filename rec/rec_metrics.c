/* rec/rec_metrics.c — Prometheus metrics endpoint for the Recording Engine */
#include "rec_metrics.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ─── Internal struct ──────────────────────────────────────────────────────── */
struct rec_metrics {
    char    stream_name[64];
    int     listen_fd;

    /* Value providers — set by caller via rec_metrics_set_providers() */
    void     *ud;
    rec_state_t  (*get_state)         (void *ud);
    uint64_t     (*get_written_bytes) (void *ud);
    uint32_t     (*get_dropped_frames)(void *ud);
};

/* ─── rec_metrics_create ───────────────────────────────────────────────────── */
rec_metrics_t *rec_metrics_create(const char *stream_name, uint16_t port)
{
    if (!stream_name) return NULL;

    rec_metrics_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    snprintf(m->stream_name, sizeof(m->stream_name), "%s", stream_name);
    m->listen_fd = -1;

    if (port == 0)
        return m;   /* disabled — no listen socket */

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        free(m);
        return NULL;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        free(m);
        return NULL;
    }

    m->listen_fd = fd;
    return m;
}

/* ─── rec_metrics_destroy ──────────────────────────────────────────────────── */
void rec_metrics_destroy(rec_metrics_t **mp)
{
    if (!mp || !*mp) return;
    rec_metrics_t *m = *mp;
    if (m->listen_fd >= 0) {
        close(m->listen_fd);
        m->listen_fd = -1;
    }
    free(m);
    *mp = NULL;
}

/* ─── rec_metrics_set_providers ────────────────────────────────────────────── */
void rec_metrics_set_providers(
    rec_metrics_t *m,
    void          *ud,
    rec_state_t  (*get_state)         (void *ud),
    uint64_t     (*get_written_bytes) (void *ud),
    uint32_t     (*get_dropped_frames)(void *ud))
{
    if (!m) return;
    m->ud                 = ud;
    m->get_state          = get_state;
    m->get_written_bytes  = get_written_bytes;
    m->get_dropped_frames = get_dropped_frames;
}

/* ─── rec_metrics_get_fd ───────────────────────────────────────────────────── */
int rec_metrics_get_fd(const rec_metrics_t *m)
{
    return m ? m->listen_fd : -1;
}

/* ─── rec_metrics_format ───────────────────────────────────────────────────── */
int rec_metrics_format(rec_metrics_t *m, char *buf, size_t buflen)
{
    if (!m || !buf || buflen == 0) return -1;

    /* Read current values via callbacks (fall back to 0 if not set) */
    rec_state_t state   = m->get_state          ? m->get_state(m->ud)          : REC_STATE_IDLE;
    uint64_t    written = m->get_written_bytes  ? m->get_written_bytes(m->ud)  : 0;
    uint32_t    dropped = m->get_dropped_frames ? m->get_dropped_frames(m->ud) : 0;

    const char *s = m->stream_name;
    char *p = buf;
    size_t left = buflen;
    int total = 0;

#define APPEND(fmt, ...) \
    do { \
        int _r = snprintf(p, left, fmt, ##__VA_ARGS__); \
        if (_r < 0 || (size_t)_r >= left) return -1; \
        p += _r; left -= (size_t)_r; total += _r; \
    } while (0)

    /* rec_written_bytes_total */
    APPEND("# HELP rec_written_bytes_total"
           " Total bytes written to .ts segment files\n");
    APPEND("# TYPE rec_written_bytes_total counter\n");
    APPEND("rec_written_bytes_total{stream=\"%s\"} %llu\n\n",
           s, (unsigned long long)written);

    /* rec_drop_frames_total */
    APPEND("# HELP rec_drop_frames_total"
           " Total frames dropped due to write-queue overflow\n");
    APPEND("# TYPE rec_drop_frames_total counter\n");
    APPEND("rec_drop_frames_total{stream=\"%s\"} %u\n\n",
           s, (unsigned int)dropped);

    /* rec_state */
    APPEND("# HELP rec_state"
           " Current recorder state"
           " (0=IDLE 1=EXTRACT_PRE 2=WAIT_KEYFRAME 3=IN_EVENT 4=POST_WAIT)\n");
    APPEND("# TYPE rec_state gauge\n");
    APPEND("rec_state{stream=\"%s\"} %d\n", s, (int)state);

#undef APPEND

    return total;
}

/* ─── rec_metrics_serve_one ────────────────────────────────────────────────── */
int rec_metrics_serve_one(rec_metrics_t *m)
{
    if (!m || m->listen_fd < 0) return -1;

    int client = accept4(m->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) return -1;   /* EAGAIN or error */

    /* Drain the HTTP request so the kernel can send a FIN (not RST) when we
     * close.  A client that sends a large request (pipelining / big headers)
     * would fill the recv buffer; close() on a non-empty buffer sends RST,
     * discarding our response.  MSG_DONTWAIT prevents stalling the event loop. */
    char req_buf[512];
    while (recv(client, req_buf, sizeof(req_buf), MSG_DONTWAIT) > 0)
        ;  /* drain until EAGAIN or error */

    char body[4096];
    int body_len = rec_metrics_format(m, body, sizeof(body));
    if (body_len < 0) {
        snprintf(body, sizeof(body), "# ERROR: buffer overflow\n");
        body_len = (int)strlen(body);
    }

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len);

    ssize_t sent = send(client, header, (size_t)hlen, MSG_NOSIGNAL);
    if (sent > 0)
        send(client, body, (size_t)body_len, MSG_NOSIGNAL);

    close(client);
    return 0;
}
