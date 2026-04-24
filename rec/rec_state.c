#define _GNU_SOURCE
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "rec_state.h"

struct rec_state_machine {
    rec_state_t     state;
    rec_mode_t      mode;
    rec_schedule_t  schedule;
    uint32_t        pre_record_sec;
    uint32_t        post_record_sec;
    int             post_remaining_sec;
    rec_slot_mode_t current_slot;

    rec_buf_t      *buf;
    int             writer_eventfd;
    uint32_t        active_pre_gen;

    rec_debounce_t  debounce;

    rec_state_cb_t  on_change;
    void           *cb_ctx;
};

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

static void transition(rec_state_machine_t *sm, rec_state_t new_state)
{
    if (sm->state == new_state)
        return;

    rec_state_t old = sm->state;
    sm->state = new_state;
    VFR_LOGI("[rec_state] %s -> %s", state_name(old), state_name(new_state));
    if (sm->on_change)
        sm->on_change(sm->cb_ctx, old, new_state);
}

static void wake_writer(rec_state_machine_t *sm)
{
    if (sm->writer_eventfd >= 0) {
        uint64_t one = 1;
        (void)write(sm->writer_eventfd, &one, sizeof(one));
    }
}

static void abort_active_pre_roll(rec_state_machine_t *sm)
{
    if (sm->active_pre_gen == REC_PRE_GEN_NONE)
        return;

    rec_buf_abort_pre_roll(sm->buf, sm->active_pre_gen);
    sm->active_pre_gen = REC_PRE_GEN_NONE;
}

static void do_extract_pre(rec_state_machine_t *sm, uint64_t now_ns)
{
    transition(sm, REC_STATE_EXTRACT_PRE);

    uint64_t target_ns =
        (sm->pre_record_sec > 0 &&
         now_ns > (uint64_t)sm->pre_record_sec * 1000000000ULL)
            ? now_ns - (uint64_t)sm->pre_record_sec * 1000000000ULL
            : 0;

    uint32_t pre_gen = REC_PRE_GEN_NONE;
    int rc = rec_buf_extract_from_keyframe(sm->buf, target_ns, &pre_gen);

    if (rc == REC_NEED_WAIT_KEYFRAME) {
        sm->active_pre_gen = REC_PRE_GEN_NONE;
        transition(sm, REC_STATE_WAIT_KEYFRAME);
        return;
    }

    if (rc == REC_OK) {
        sm->active_pre_gen = pre_gen;
        wake_writer(sm);
        transition(sm, REC_STATE_IN_EVENT);
        return;
    }

    sm->active_pre_gen = REC_PRE_GEN_NONE;
    VFR_LOGW("[rec_state] extract_from_keyframe error %d, aborting to IDLE", rc);
    transition(sm, REC_STATE_IDLE);
}

static bool schedule_allows(rec_state_machine_t *sm, time_t now_wall)
{
    if (sm->mode == REC_MODE_CONTINUOUS)
        return true;

    rec_slot_mode_t slot = rec_schedule_query(&sm->schedule, now_wall);
    return slot != REC_SLOT_OFF;
}

rec_state_machine_t *rec_state_create(const rec_state_config_t *cfg,
                                      rec_buf_t                *buf,
                                      int                       writer_eventfd,
                                      rec_state_cb_t            on_change,
                                      void                     *cb_ctx)
{
    if (!cfg || !buf)
        return NULL;

    rec_state_machine_t *sm = calloc(1, sizeof(*sm));
    if (!sm)
        return NULL;

    sm->state = REC_STATE_IDLE;
    sm->mode = cfg->mode;
    sm->schedule = cfg->schedule;
    sm->pre_record_sec = cfg->pre_record_sec;
    sm->post_record_sec = cfg->post_record_sec;
    sm->post_remaining_sec = 0;
    sm->buf = buf;
    sm->writer_eventfd = writer_eventfd;
    sm->active_pre_gen = REC_PRE_GEN_NONE;
    sm->on_change = on_change;
    sm->cb_ctx = cb_ctx;

    rec_debounce_init(&sm->debounce, REC_DEBOUNCE_MS);
    sm->current_slot = rec_schedule_query(&sm->schedule, time(NULL));

    return sm;
}

void rec_state_destroy(rec_state_machine_t **sm)
{
    if (!sm || !*sm)
        return;
    free(*sm);
    *sm = NULL;
}

void rec_state_on_trigger(rec_state_machine_t *sm,
                          rec_trigger_type_t   type,
                          uint64_t             now_ns,
                          time_t               now_wall)
{
    if (!sm)
        return;
    if (now_wall == 0)
        now_wall = time(NULL);

    if (type == REC_TRIGGER_START) {
        if (!schedule_allows(sm, now_wall)) {
            VFR_LOGD("[rec_state] TRIGGER_START ignored: schedule off");
            return;
        }

        if (!rec_debounce_allow_start(&sm->debounce, now_ns)) {
            VFR_LOGD("[rec_state] TRIGGER_START debounced");
            return;
        }

        switch (sm->state) {
        case REC_STATE_IDLE:
            do_extract_pre(sm, now_ns);
            break;

        case REC_STATE_POST_WAIT:
            sm->post_remaining_sec = 0;
            transition(sm, REC_STATE_IN_EVENT);
            break;

        case REC_STATE_IN_EVENT:
        case REC_STATE_WAIT_KEYFRAME:
        case REC_STATE_EXTRACT_PRE:
            break;
        }

        return;
    }

    if (!rec_debounce_allow_stop(&sm->debounce, now_ns)) {
        VFR_LOGD("[rec_state] TRIGGER_STOP debounced");
        return;
    }

    switch (sm->state) {
    case REC_STATE_IN_EVENT:
        sm->post_remaining_sec = (int)sm->post_record_sec;
        transition(sm, REC_STATE_POST_WAIT);
        break;

    case REC_STATE_WAIT_KEYFRAME:
        transition(sm, REC_STATE_IDLE);
        break;

    case REC_STATE_IDLE:
    case REC_STATE_EXTRACT_PRE:
    case REC_STATE_POST_WAIT:
        break;
    }
}

void rec_state_on_timer(rec_state_machine_t *sm,
                        uint64_t             expirations,
                        bool                 pending_trigger,
                        time_t               now_wall)
{
    if (!sm || expirations == 0)
        return;
    if (now_wall == 0)
        now_wall = time(NULL);

    if (sm->state == REC_STATE_POST_WAIT) {
        sm->post_remaining_sec -= (int)expirations;
        if (sm->post_remaining_sec <= 0) {
            sm->post_remaining_sec = 0;
            if (!pending_trigger)
                transition(sm, REC_STATE_IDLE);
        }
    }

    rec_slot_mode_t slot = rec_schedule_query(&sm->schedule, now_wall);
    if (slot == sm->current_slot)
        return;

    sm->current_slot = slot;

    if (slot == REC_SLOT_OFF) {
        switch (sm->state) {
        case REC_STATE_IN_EVENT:
            sm->post_remaining_sec = (int)sm->post_record_sec;
            transition(sm, REC_STATE_POST_WAIT);
            break;

        case REC_STATE_EXTRACT_PRE:
        case REC_STATE_WAIT_KEYFRAME:
        case REC_STATE_POST_WAIT:
            transition(sm, REC_STATE_IDLE);
            break;

        case REC_STATE_IDLE:
            break;
        }
    }
}

bool rec_state_on_keyframe(rec_state_machine_t *sm)
{
    if (!sm || sm->state != REC_STATE_WAIT_KEYFRAME)
        return false;

    transition(sm, REC_STATE_IN_EVENT);
    return true;
}

void rec_state_force_idle(rec_state_machine_t *sm)
{
    if (!sm)
        return;

    abort_active_pre_roll(sm);
    sm->post_remaining_sec = 0;
    transition(sm, REC_STATE_IDLE);
}

void rec_state_set_schedule(rec_state_machine_t *sm,
                            const rec_schedule_t *sched)
{
    if (!sm || !sched)
        return;
    sm->schedule = *sched;
}

rec_state_t rec_state_get(const rec_state_machine_t *sm)
{
    return sm ? sm->state : REC_STATE_IDLE;
}

int rec_state_get_post_remaining(const rec_state_machine_t *sm)
{
    return sm ? sm->post_remaining_sec : 0;
}
