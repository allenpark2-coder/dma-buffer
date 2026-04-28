/* rec/rec_trigger.h — AI-process → Recorder Unix socket listener */
#ifndef REC_TRIGGER_H
#define REC_TRIGGER_H

#include <stdint.h>
#include "rec_defs.h"

typedef struct rec_trigger rec_trigger_t;

/*
 * rec_trigger_create():
 *   Binds a SOCK_STREAM socket at abstract path \0/vfr/event/<stream_name>.
 *   on_trigger is called for each valid vfr_event_msg_t received.
 *   Returns NULL on failure (errno set).
 *   The fd is SOCK_NONBLOCK — caller must poll (epoll) it.
 */
rec_trigger_t *rec_trigger_create(
    const char *stream_name,
    void (*on_trigger)(void *ud, rec_trigger_type_t type, uint64_t ts_ns),
    void *ud);

void rec_trigger_destroy(rec_trigger_t **t);

/* rec_trigger_get_fd(): returns listening fd to register with epoll. */
int rec_trigger_get_fd(const rec_trigger_t *t);

/*
 * rec_trigger_handle_accept():
 *   Call when epoll reports EPOLLIN on the listen fd.
 *   Accepts one connection, reads sizeof(vfr_event_msg_t), validates magic,
 *   calls on_trigger, closes connection.
 *   Short reads or bad magic are silently discarded.
 */
void rec_trigger_handle_accept(rec_trigger_t *t);

#endif /* REC_TRIGGER_H */
