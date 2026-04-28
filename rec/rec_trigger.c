/* rec/rec_trigger.c — Unix abstract-namespace socket listener */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rec_trigger.h"
#include "../ipc/vfr_ipc_types.h"

struct rec_trigger {
    int    listen_fd;
    void (*on_trigger)(void *ud, rec_trigger_type_t type, uint64_t ts_ns);
    void  *ud;
};

static socklen_t make_addr(const char *stream_name, struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    /* Abstract namespace: sun_path[0] = '\0', remainder is the name */
    int n = snprintf(addr->sun_path + 1, sizeof(addr->sun_path) - 1,
                     "/vfr/event/%s", stream_name);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

rec_trigger_t *rec_trigger_create(
    const char *stream_name,
    void (*on_trigger)(void *ud, rec_trigger_type_t type, uint64_t ts_ns),
    void *ud)
{
    if (!stream_name || !on_trigger)
        return NULL;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;

    struct sockaddr_un addr;
    socklen_t addrlen = make_addr(stream_name, &addr);

    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        close(fd);
        return NULL;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return NULL;
    }

    rec_trigger_t *t = malloc(sizeof(*t));
    if (!t) {
        close(fd);
        return NULL;
    }
    t->listen_fd  = fd;
    t->on_trigger = on_trigger;
    t->ud         = ud;
    return t;
}

void rec_trigger_destroy(rec_trigger_t **t)
{
    if (!t || !*t)
        return;
    close((*t)->listen_fd);
    free(*t);
    *t = NULL;
}

int rec_trigger_get_fd(const rec_trigger_t *t)
{
    return t ? t->listen_fd : -1;
}

void rec_trigger_handle_accept(rec_trigger_t *t)
{
    if (!t)
        return;

    int conn = accept4(t->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (conn < 0)
        return;

    /* Bound blocking time: a slow/malicious sender must not stall the
     * epoll event loop.  1-second timeout is generous for a local IPC
     * message of a few bytes. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    vfr_event_msg_t msg;
    ssize_t n = recv(conn, &msg, sizeof(msg), MSG_WAITALL);
    close(conn);

    if (n != (ssize_t)sizeof(msg) || msg.magic != VFR_EVENT_MAGIC)
        return;

    t->on_trigger(t->ud, (rec_trigger_type_t)msg.event_type, msg.timestamp_ns);
}
