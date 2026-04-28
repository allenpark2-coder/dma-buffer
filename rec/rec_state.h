/* rec/rec_state.h — 5-state Recorder state machine */
#ifndef REC_STATE_H
#define REC_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "rec_defs.h"
#include "rec_buf.h"
#include "rec_debounce.h"

typedef struct rec_state_ctx {
    rec_state_t    state;
    rec_buf_t     *buf;            /* ring buffer — used for EXTRACT_PRE */
    uint32_t       pre_sec;        /* pre-roll window in seconds */
    uint32_t       last_batch_gen; /* batch_gen of the active pre-roll (0 = none) */
    int            post_remaining_sec;  /* POST_WAIT countdown */
    bool           pending_trigger;     /* new START received while in POST_WAIT */

    /* Schedule gate: if non-NULL, returns true if recording is allowed.
     * NULL means always allow. */
    bool (*is_schedule_active)(void *ud);
    void *schedule_ud;

    rec_debounce_t debounce;

    /* Transition log: if NULL, prints to stderr. */
    void (*on_transition)(void *ud, rec_state_t from, rec_state_t to);
    void *transition_ud;
} rec_state_ctx_t;

void rec_state_init(rec_state_ctx_t *ctx, rec_buf_t *buf, uint32_t pre_sec);

void rec_state_on_trigger(rec_state_ctx_t *ctx, rec_trigger_type_t type,
                           uint64_t ts_ns);
void rec_state_on_timer_tick(rec_state_ctx_t *ctx, uint64_t expirations);
void rec_state_on_keyframe(rec_state_ctx_t *ctx);
void rec_state_force_idle(rec_state_ctx_t *ctx);
rec_state_t rec_state_get(const rec_state_ctx_t *ctx);

#endif /* REC_STATE_H */
