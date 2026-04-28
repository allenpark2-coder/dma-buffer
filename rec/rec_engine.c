/* rec/rec_engine.c — Recording Engine: integrates all rec/ sub-modules */
#include "rec_engine.h"
#include "rec_buf.h"
#include "rec_state.h"
#include "rec_writer.h"
#include "rec_trigger.h"
#include "rec_schedule.h"
#include "rec_metrics.h"
#include "rec_defs.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <stdint.h>

/* ─── include vfr_defs.h for VFR_FMT_* constants ─────────────────────────── */
/* rec_engine sits one level above rec/ so include via relative path */
#include "../include/vfr_defs.h"

/* ─── Engine struct ─────────────────────────────────────────────────────────── */
struct rec_engine {
    rec_config_t        cfg;
    rec_buf_t          *ring;
    rec_state_ctx_t     state_ctx;
    rec_writer_t       *writer;
    rec_trigger_t      *trigger;
    rec_codec_config_t  codec_cfg;
    int                 timer_fd;

    /* schedule state */
    rec_slot_mode_t     current_slot;

    /*
     * segment_open_pending: set to true whenever the state machine transitions
     * to a recording state (IN_EVENT from WAIT_KEYFRAME, or at startup for
     * CONTINUOUS mode).  The next keyframe pushed will carry
     * is_segment_boundary = true so the Writer opens a fresh segment / clip.
     */
    bool                segment_open_pending;

    /* Prometheus metrics HTTP endpoint (NULL if metrics_port == 0) */
    rec_metrics_t      *metrics;
};

/* ─── Forward declarations ──────────────────────────────────────────────────── */
static void engine_on_transition(void *ud, rec_state_t from, rec_state_t to);
static bool engine_is_schedule_active(void *ud);
static bool should_write_live(const rec_engine_t *eng);
static int  create_timer_fd(void);

/* ─── Metrics value-provider callbacks ─────────────────────────────────────── */
static rec_state_t metrics_get_state(void *ud)
{
    return rec_engine_get_state((const rec_engine_t *)ud);
}
static uint64_t metrics_get_written_bytes(void *ud)
{
    return rec_engine_get_written_bytes((const rec_engine_t *)ud);
}
static uint32_t metrics_get_dropped_frames(void *ud)
{
    return rec_engine_get_dropped_frames((const rec_engine_t *)ud);
}

/* ─── Trigger callback: called by rec_trigger when a message arrives ─────────── */
static void engine_on_trigger(void *ud, rec_trigger_type_t type, uint64_t ts_ns)
{
    rec_engine_t *eng = ud;

    /* Use monotonic clock for the trigger timestamp */
    rec_state_on_trigger(&eng->state_ctx, type, ts_ns);
}

/* ─── State-machine transition hook ──────────────────────────────────────────── */
static void engine_on_transition(void *ud, rec_state_t from, rec_state_t to)
{
    rec_engine_t *eng = ud;

    fprintf(stderr, "[rec_engine] state %d → %d\n", (int)from, (int)to);

    if (to == REC_STATE_IN_EVENT || to == REC_STATE_WAIT_KEYFRAME) {
        /*
         * When entering IN_EVENT from IDLE (via EXTRACT_PRE), the pre_queue has
         * been filled.  Wake the Writer so it starts draining pre-roll entries.
         *
         * When transitioning to WAIT_KEYFRAME, no pre-roll data was queued, so
         * the Writer wakeup is harmless but has no effect.
         */
        if (to == REC_STATE_IN_EVENT) {
            eng->segment_open_pending = true;
            int efd = rec_writer_get_eventfd(eng->writer);
            if (efd >= 0) {
                uint64_t one = 1;
                ssize_t rc = write(efd, &one, sizeof(one));
                (void)rc;
            }
        }
    }

    if (to == REC_STATE_IDLE) {
        /*
         * Recording stopped.  The Writer will close the current segment when
         * the flush sentinel arrives (triggered by destroy), or it will simply
         * stop receiving frames.  We do not need to explicitly tell the Writer
         * here — stopping the live frame feed is sufficient.
         */
        (void)from;
    }
}

/* ─── Schedule gate used by the state machine ────────────────────────────────── */
static bool engine_is_schedule_active(void *ud)
{
    rec_engine_t *eng = ud;
    if (eng->cfg.default_mode != REC_MODE_SCHEDULED)
        return true;   /* non-scheduled modes are always "active" */
    return eng->current_slot != REC_SLOT_OFF;
}

/* ─── Decide whether a live frame should be written ──────────────────────────── */
static bool should_write_live(const rec_engine_t *eng)
{
    rec_state_t state = rec_state_get(&eng->state_ctx);

    switch (eng->cfg.default_mode) {
    case REC_MODE_CONTINUOUS:
        return true;

    case REC_MODE_EVENT:
        return (state == REC_STATE_IN_EVENT || state == REC_STATE_POST_WAIT);

    case REC_MODE_SCHEDULED:
        if (eng->current_slot == REC_SLOT_CONTINUOUS)
            return true;
        if (eng->current_slot == REC_SLOT_EVENT)
            return (state == REC_STATE_IN_EVENT || state == REC_STATE_POST_WAIT);
        return false;   /* REC_SLOT_OFF */
    }
    return false;
}

/* ─── Create and arm a 1-second repeating timerfd ────────────────────────────── */
static int create_timer_fd(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) return -1;

    struct itimerspec its = {
        .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
        .it_value    = { .tv_sec = 1, .tv_nsec = 0 },
    };
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ─── rec_engine_create ───────────────────────────────────────────────────────── */
rec_engine_t *rec_engine_create(const rec_config_t *cfg)
{
    if (!cfg) return NULL;

    rec_engine_t *eng = calloc(1, sizeof(*eng));
    if (!eng) return NULL;

    eng->cfg = *cfg;

    /* 1. Ring buffer */
    uint32_t ring_sz = cfg->ring_buf_size;
    if (ring_sz < REC_BUF_SIZE_MIN) ring_sz = REC_BUF_SIZE_DEFAULT;
    if (ring_sz > REC_BUF_SIZE_MAX) ring_sz = REC_BUF_SIZE_MAX;

    eng->ring = rec_buf_create(ring_sz);
    if (!eng->ring) goto fail;

    /* 2. Codec config cache */
    atomic_init(&eng->codec_cfg.active_slot, 0);
    atomic_init(&eng->codec_cfg.version, 0);

    /* 3. Writer */
    rec_writer_config_t wcfg = {
        .segment_duration_sec = cfg->segment_duration_sec
                                  ? cfg->segment_duration_sec
                                  : REC_SEGMENT_DURATION_SEC,
        .segment_size_max     = cfg->segment_size_max
                                  ? cfg->segment_size_max
                                  : (uint64_t)REC_SEGMENT_SIZE_MAX,
        .flush_interval_sec   = cfg->flush_interval_sec,
        .mode                 = cfg->default_mode,
    };
    snprintf(wcfg.output_dir,  sizeof(wcfg.output_dir),  "%s", cfg->output_dir);
    snprintf(wcfg.stream_name, sizeof(wcfg.stream_name), "%s", cfg->stream_name);

    eng->writer = rec_writer_create(eng->ring, &eng->codec_cfg, &wcfg);
    if (!eng->writer) goto fail;

    /* 4. State machine */
    rec_state_init(&eng->state_ctx, eng->ring,
                   cfg->pre_record_sec ? cfg->pre_record_sec : 5);

    eng->state_ctx.on_transition = engine_on_transition;
    eng->state_ctx.transition_ud = eng;

    if (cfg->default_mode == REC_MODE_SCHEDULED ||
        cfg->default_mode == REC_MODE_EVENT) {
        eng->state_ctx.is_schedule_active = engine_is_schedule_active;
        eng->state_ctx.schedule_ud        = eng;
    }

    /* 5. Schedule initial slot */
    if (cfg->default_mode == REC_MODE_SCHEDULED) {
        eng->current_slot = rec_schedule_query(&cfg->schedule, time(NULL));
    } else {
        eng->current_slot = REC_SLOT_CONTINUOUS;
    }

    /* 6. Trigger socket */
    if (cfg->stream_name[0]) {
        eng->trigger = rec_trigger_create(cfg->stream_name,
                                          engine_on_trigger, eng);
        /* Non-fatal if trigger fails (e.g., name too long or socket error) */
        if (!eng->trigger)
            fprintf(stderr, "[rec_engine] warning: trigger socket not created\n");
    }

    /* 7. timerfd */
    eng->timer_fd = create_timer_fd();
    if (eng->timer_fd < 0) goto fail;

    /* 8. For CONTINUOUS mode, open the first segment on the next keyframe */
    if (cfg->default_mode == REC_MODE_CONTINUOUS)
        eng->segment_open_pending = true;

    /* 9. Prometheus metrics endpoint (optional) */
    if (cfg->metrics_port > 0) {
        eng->metrics = rec_metrics_create(cfg->stream_name, cfg->metrics_port);
        if (eng->metrics) {
            rec_metrics_set_providers(eng->metrics, eng,
                                      metrics_get_state,
                                      metrics_get_written_bytes,
                                      metrics_get_dropped_frames);
        }
        /* Non-fatal: if metrics creation fails, recording still works */
    }

    return eng;

fail:
    /* partial cleanup */
    if (eng->timer_fd >= 0)        close(eng->timer_fd);
    if (eng->trigger)              rec_trigger_destroy(&eng->trigger);
    if (eng->writer)               rec_writer_destroy(&eng->writer);
    if (eng->ring)                 rec_buf_destroy(&eng->ring);
    free(eng);
    return NULL;
}

/* ─── rec_engine_get_epoll_fds ───────────────────────────────────────────────── */
int rec_engine_get_epoll_fds(rec_engine_t *eng, int *fds_out, int max_fds)
{
    if (!eng || !fds_out || max_fds < 2) return 0;

    int n = 0;
    if (eng->timer_fd >= 0 && n < max_fds)
        fds_out[n++] = eng->timer_fd;

    int trig_fd = eng->trigger ? rec_trigger_get_fd(eng->trigger) : -1;
    if (trig_fd >= 0 && n < max_fds)
        fds_out[n++] = trig_fd;

    int met_fd = eng->metrics ? rec_metrics_get_fd(eng->metrics) : -1;
    if (met_fd >= 0 && n < max_fds)
        fds_out[n++] = met_fd;

    return n;
}

/* ─── rec_engine_handle_event ────────────────────────────────────────────────── */
int rec_engine_handle_event(rec_engine_t *eng, int fd)
{
    if (!eng || fd < 0) return -1;

    /* timerfd: 1-second tick */
    if (fd == eng->timer_fd) {
        uint64_t expirations = 0;
        ssize_t rc = read(eng->timer_fd, &expirations, sizeof(expirations));
        if (rc < 0) return -1;

        /* Drive POST_WAIT countdown */
        rec_state_on_timer_tick(&eng->state_ctx, expirations);

        /* Check schedule boundary */
        if (eng->cfg.default_mode == REC_MODE_SCHEDULED) {
            rec_slot_mode_t new_slot =
                rec_schedule_query(&eng->cfg.schedule, time(NULL));
            if (new_slot != eng->current_slot) {
                rec_slot_mode_t old = eng->current_slot;
                eng->current_slot   = new_slot;

                if (new_slot == REC_SLOT_OFF) {
                    /* Schedule turned recording off */
                    rec_state_force_idle(&eng->state_ctx);
                } else if (new_slot == REC_SLOT_CONTINUOUS &&
                           old == REC_SLOT_OFF) {
                    /* Entering a continuous window */
                    eng->segment_open_pending = true;
                }
                /* SLOT_EVENT: event mode kicks in on next TRIGGER_START */
            }
        }
        return REC_OK;
    }

    /* Trigger socket */
    int trig_fd = eng->trigger ? rec_trigger_get_fd(eng->trigger) : -1;
    if (trig_fd >= 0 && fd == trig_fd) {
        rec_trigger_handle_accept(eng->trigger);
        return REC_OK;
    }

    /* Prometheus metrics scrape */
    int met_fd = eng->metrics ? rec_metrics_get_fd(eng->metrics) : -1;
    if (met_fd >= 0 && fd == met_fd) {
        rec_metrics_serve_one(eng->metrics);
        return REC_OK;
    }

    return -1;  /* unknown fd */
}

/* ─── rec_engine_push_frame ──────────────────────────────────────────────────── */
int rec_engine_push_frame(rec_engine_t *eng,
                          const uint8_t *data, uint32_t size,
                          uint64_t timestamp_ns, uint64_t seq_num,
                          bool is_keyframe, uint32_t format,
                          bool is_codec_config)
{
    if (!eng || !data || size == 0) return -1;

    /* Codec config (SPS/PPS/VPS): publish to cache, do not push to ring */
    if (is_codec_config) {
        rec_writer_publish_codec_config(eng->writer, data, size, format);
        return REC_OK;
    }

    /* Always push to pre-roll ring buffer */
    int rc = rec_buf_push(eng->ring, data, size, timestamp_ns, seq_num, is_keyframe);
    if (rc != REC_OK) return rc;

    /* If waiting for first keyframe, check now */
    if (rec_state_get(&eng->state_ctx) == REC_STATE_WAIT_KEYFRAME && is_keyframe) {
        rec_state_on_keyframe(&eng->state_ctx);
        eng->segment_open_pending = true;
    }

    /* Decide whether to feed this frame to the writer */
    if (!should_write_live(eng)) return REC_OK;

    /* Allocate frame copy for the write queue (write queue owns the memory) */
    uint8_t *copy = malloc(size);
    if (!copy) return -1;
    memcpy(copy, data, size);

    /*
     * Mark the frame as a segment boundary if:
     *   - segment_open_pending is set (first frame of a new clip/continuous session)
     *   - AND this frame is a keyframe (required to open a valid TS segment)
     */
    bool is_seg_boundary = false;
    if (eng->segment_open_pending && is_keyframe) {
        is_seg_boundary           = true;
        eng->segment_open_pending = false;
    }

    rec_write_item_t item = {
        .data               = copy,
        .size               = size,
        .timestamp_ns       = timestamp_ns,
        .seq_num            = seq_num,
        .is_keyframe        = is_keyframe,
        .is_segment_boundary = is_seg_boundary,
        .is_shutdown_sentinel = false,
    };

    rc = rec_writer_enqueue(eng->writer, item);
    if (rc != REC_OK) {
        free(copy);
        /* On QUEUE_FULL, writer already incremented dropped_frames */
    }
    return rc;
}

/* ─── rec_engine_destroy ─────────────────────────────────────────────────────── */
void rec_engine_destroy(rec_engine_t **engp)
{
    if (!engp || !*engp) return;
    rec_engine_t *eng = *engp;

    /*
     * Shutdown order per §10.1:
     *  1. Writer: enqueue sentinel + join thread  (via rec_writer_destroy)
     *  2. Close timer_fd
     *  3. Destroy trigger socket
     *  4. Destroy ring buffer
     *  5. Free engine
     */
    rec_writer_destroy(&eng->writer);

    if (eng->timer_fd >= 0) {
        close(eng->timer_fd);
        eng->timer_fd = -1;
    }

    rec_trigger_destroy(&eng->trigger);
    rec_metrics_destroy(&eng->metrics);
    rec_buf_destroy(&eng->ring);

    free(eng);
    *engp = NULL;
}

/* ─── Metrics / state ────────────────────────────────────────────────────────── */
rec_state_t rec_engine_get_state(const rec_engine_t *eng)
{
    return eng ? rec_state_get(&eng->state_ctx) : REC_STATE_IDLE;
}

uint64_t rec_engine_get_written_bytes(const rec_engine_t *eng)
{
    return eng ? rec_writer_get_written_bytes(eng->writer) : 0;
}

uint32_t rec_engine_get_dropped_frames(const rec_engine_t *eng)
{
    return eng ? rec_writer_get_dropped_frames(eng->writer) : 0;
}
