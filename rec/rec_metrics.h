/* rec/rec_metrics.h — Prometheus metrics endpoint for the Recording Engine
 *
 * Exposes three metrics:
 *   rec_written_bytes_total   — counter: bytes written to .ts segment files
 *   rec_drop_frames_total     — counter: frames dropped due to write-queue overflow
 *   rec_state                 — gauge:   current state machine state
 *                               (0=IDLE 1=EXTRACT_PRE 2=WAIT_KEYFRAME
 *                                3=IN_EVENT 4=POST_WAIT)
 *
 * HTTP endpoint: minimal TCP socket (HTTP/1.0, no keep-alive).
 * Usage:
 *   rec_metrics_t *m = rec_metrics_create("cam0", 9200);
 *   rec_metrics_set_providers(m, eng,
 *       eng_get_state, eng_get_written_bytes, eng_get_dropped_frames);
 *   int fd = rec_metrics_get_fd(m);   // add to caller's epoll (EPOLLIN)
 *   // on EPOLLIN: rec_metrics_serve_one(m);
 *   rec_metrics_destroy(&m);          // close listen fd before this
 */
#ifndef REC_METRICS_H
#define REC_METRICS_H

#include <stdint.h>
#include <stddef.h>
#include "rec_defs.h"

typedef struct rec_metrics rec_metrics_t;

/*
 * rec_metrics_create():
 *   stream_name — Prometheus label value embedded in every metric line.
 *   port        — TCP port to listen on (0 = disabled, get_fd returns -1).
 * Returns NULL on failure.
 */
rec_metrics_t *rec_metrics_create(const char *stream_name, uint16_t port);

/*
 * rec_metrics_destroy():
 *   Closes the listen fd (if open), frees memory, sets *m = NULL.
 *   Idempotent.
 */
void rec_metrics_destroy(rec_metrics_t **m);

/*
 * rec_metrics_set_providers():
 *   Register callbacks that rec_metrics_serve_one() calls to read live values.
 *   Must be called before the first serve_one().
 *   All three callbacks are required.
 */
void rec_metrics_set_providers(
    rec_metrics_t *m,
    void          *ud,
    rec_state_t  (*get_state)         (void *ud),
    uint64_t     (*get_written_bytes) (void *ud),
    uint32_t     (*get_dropped_frames)(void *ud));

/*
 * rec_metrics_get_fd():
 *   Returns the non-blocking TCP listen fd (add to epoll, EPOLLIN).
 *   Returns -1 if port == 0 or creation failed.
 */
int rec_metrics_get_fd(const rec_metrics_t *m);

/*
 * rec_metrics_serve_one():
 *   Accept one connection, write HTTP/1.0 + Prometheus text body, close.
 *   Non-blocking: returns -1 immediately on EAGAIN/EWOULDBLOCK.
 * Returns: 0 = served one request, -1 = nothing to do / error.
 */
int rec_metrics_serve_one(rec_metrics_t *m);

/*
 * rec_metrics_format():
 *   Write Prometheus text to buf without any network I/O.
 *   Useful for testing.
 * Returns: bytes written (excluding NUL), or -1 if buf is too small.
 */
int rec_metrics_format(rec_metrics_t *m, char *buf, size_t buflen);

#endif /* REC_METRICS_H */
