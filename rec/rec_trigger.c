#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "rec_trigger.h"

struct rec_trigger {
    int              listen_fd;
    rec_trigger_cb_t cb;
    void            *cb_ctx;
    char             stream_name[VFR_SOCKET_NAME_MAX];
};

static bool rec_trigger_valid_type(uint32_t event_type)
{
    return event_type == REC_TRIGGER_START || event_type == REC_TRIGGER_STOP;
}

static int rec_trigger_recv_full(int fd, void *buf, size_t size)
{
    size_t total = 0;
    uint8_t *dst = buf;

    while (total < size) {
        ssize_t n = recv(fd, dst + total, size - total, 0);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += (size_t)n;
    }

    return 0;
}

rec_trigger_t *rec_trigger_create(const char       *stream_name,
                                  rec_trigger_cb_t  cb,
                                  void             *cb_ctx)
{
    if (!stream_name || !cb || stream_name[0] == '\0')
        return NULL;
    if (strnlen(stream_name, VFR_SOCKET_NAME_MAX) >= VFR_SOCKET_NAME_MAX)
        return NULL;

    rec_trigger_t *trig = calloc(1, sizeof(*trig));
    if (!trig)
        return NULL;

    snprintf(trig->stream_name, sizeof(trig->stream_name), "%s", stream_name);
    trig->cb = cb;
    trig->cb_ctx = cb_ctx;

    trig->listen_fd = socket(AF_UNIX,
                             SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             0);
    if (trig->listen_fd < 0) {
        free(trig);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    int path_len = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                            "/vfr/event/%s", stream_name);
    if (path_len <= 0 || path_len >= (int)(sizeof(addr.sun_path) - 1)) {
        close(trig->listen_fd);
        free(trig);
        return NULL;
    }

    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                     + 1 + (size_t)path_len);

    if (bind(trig->listen_fd, (struct sockaddr *)&addr, addr_len) < 0) {
        close(trig->listen_fd);
        free(trig);
        return NULL;
    }

    if (listen(trig->listen_fd, 4) < 0) {
        close(trig->listen_fd);
        free(trig);
        return NULL;
    }

    return trig;
}

void rec_trigger_destroy(rec_trigger_t **trig)
{
    if (!trig || !*trig)
        return;
    close((*trig)->listen_fd);
    free(*trig);
    *trig = NULL;
}

int rec_trigger_get_fd(const rec_trigger_t *trig)
{
    return trig ? trig->listen_fd : -1;
}

int rec_trigger_handle_readable(rec_trigger_t *trig)
{
    if (!trig)
        return -1;

    bool drained_any = false;

    for (;;) {
        int conn = accept4(trig->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (conn < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return drained_any ? 0 : -1;
            return -1;
        }

        vfr_event_msg_t msg;
        memset(&msg, 0, sizeof(msg));

        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 100000,
        };
        (void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int rc = rec_trigger_recv_full(conn, &msg, sizeof(msg));
        close(conn);
        drained_any = true;

        if (rc < 0) {
            VFR_LOGW("[rec_trigger] short read ignored");
            continue;
        }

        msg.stream_name[VFR_SOCKET_NAME_MAX - 1] = '\0';
        msg.label[sizeof(msg.label) - 1] = '\0';

        if (msg.magic != VFR_EVENT_MAGIC) {
            VFR_LOGW("[rec_trigger] invalid magic ignored");
            continue;
        }

        if (strncmp(msg.stream_name, trig->stream_name,
                    VFR_SOCKET_NAME_MAX) != 0) {
            VFR_LOGW("[rec_trigger] stream mismatch ignored: got=%s want=%s",
                     msg.stream_name, trig->stream_name);
            continue;
        }

        if (!rec_trigger_valid_type(msg.event_type)) {
            VFR_LOGW("[rec_trigger] unknown event type %u ignored",
                     msg.event_type);
            continue;
        }

        trig->cb(trig->cb_ctx,
                 (rec_trigger_type_t)msg.event_type,
                 msg.timestamp_ns,
                 msg.confidence,
                 msg.label);
    }
}
