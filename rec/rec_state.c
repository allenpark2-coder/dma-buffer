/* rec/rec_state.c — Recorder 5-state machine */
#include <stdio.h>
#include <string.h>
#include "rec_state.h"

static const char *state_name(rec_state_t s)
{
    switch (s) {
    case REC_STATE_IDLE:          return "IDLE";
    case REC_STATE_EXTRACT_PRE:   return "EXTRACT_PRE";
    case REC_STATE_WAIT_KEYFRAME: return "WAIT_KEYFRAME";
    case REC_STATE_IN_EVENT:      return "IN_EVENT";
    case REC_STATE_POST_WAIT:     return "POST_WAIT";
    default:                      return "UNKNOWN";
    }
}

static void do_transition(rec_state_ctx_t *ctx, rec_state_t new_state)
{
    rec_state_t old = ctx->state;
    if (old == new_state)
        return;
    ctx->state = new_state;
    if (ctx->on_transition) {
        ctx->on_transition(ctx->transition_ud, old, new_state);
    } else {
        fprintf(stderr, "[rec_state] %s → %s\n",
                state_name(old), state_name(new_state));
    }
}

void rec_state_init(rec_state_ctx_t *ctx, rec_buf_t *buf, uint32_t pre_sec)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state   = REC_STATE_IDLE;
    ctx->buf     = buf;
    ctx->pre_sec = pre_sec;
    rec_debounce_init(&ctx->debounce, REC_DEBOUNCE_MS);
}

void rec_state_on_trigger(rec_state_ctx_t *ctx, rec_trigger_type_t type,
                           uint64_t ts_ns)
{
    if (!rec_debounce_filter(&ctx->debounce, type, ts_ns))
        return;

    if (type == REC_TRIGGER_START) {
        if (ctx->is_schedule_active &&
            !ctx->is_schedule_active(ctx->schedule_ud))
            return;

        switch (ctx->state) {
        case REC_STATE_IDLE: {
            uint32_t batch_gen = 0;
            int count = 0;
            if (ctx->buf)
                count = rec_buf_extract_from_keyframe(ctx->buf, ts_ns,
                                                      ctx->pre_sec, &batch_gen);
            if (count > 0) {
                ctx->last_batch_gen = batch_gen;
                do_transition(ctx, REC_STATE_IN_EVENT);
            } else {
                do_transition(ctx, REC_STATE_WAIT_KEYFRAME);
            }
            break;
        }
        case REC_STATE_POST_WAIT:
            ctx->pending_trigger    = true;
            ctx->post_remaining_sec = REC_POST_RECORD_SEC_DEFAULT;
            do_transition(ctx, REC_STATE_IN_EVENT);
            break;
        default:
            break;
        }

    } else { /* REC_TRIGGER_STOP */
        switch (ctx->state) {
        case REC_STATE_IN_EVENT:
            ctx->post_remaining_sec = REC_POST_RECORD_SEC_DEFAULT;
            ctx->pending_trigger    = false;
            do_transition(ctx, REC_STATE_POST_WAIT);
            break;
        case REC_STATE_WAIT_KEYFRAME:
            do_transition(ctx, REC_STATE_IDLE);
            break;
        default:
            break;
        }
    }
}

void rec_state_on_timer_tick(rec_state_ctx_t *ctx, uint64_t expirations)
{
    if (ctx->state != REC_STATE_POST_WAIT)
        return;

    ctx->post_remaining_sec -= (int)expirations;
    if (ctx->post_remaining_sec <= 0 && !ctx->pending_trigger)
        do_transition(ctx, REC_STATE_IDLE);
}

void rec_state_on_keyframe(rec_state_ctx_t *ctx)
{
    if (ctx->state != REC_STATE_WAIT_KEYFRAME)
        return;
    do_transition(ctx, REC_STATE_IN_EVENT);
}

void rec_state_force_idle(rec_state_ctx_t *ctx)
{
    if (ctx->buf && ctx->last_batch_gen != 0) {
        rec_buf_abort_pre(ctx->buf, ctx->last_batch_gen);
        ctx->last_batch_gen = 0;
    }
    do_transition(ctx, REC_STATE_IDLE);
}

rec_state_t rec_state_get(const rec_state_ctx_t *ctx)
{
    return ctx->state;
}
