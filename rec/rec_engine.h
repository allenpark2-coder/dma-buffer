/* rec/rec_engine.h — Recording Engine public API (integrates all rec/ modules) */
#ifndef REC_ENGINE_H
#define REC_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "rec_defs.h"
#include "rec_schedule.h"

typedef struct rec_engine rec_engine_t;

/* Configuration passed to rec_engine_create(). */
typedef struct {
    char              stream_name[64];   /* socket name + file name prefix */
    rec_mode_t        default_mode;      /* CONTINUOUS / SCHEDULED / EVENT */
    rec_schedule_t    schedule;          /* used when mode == SCHEDULED */
    uint32_t          pre_record_sec;    /* pre-roll window (EVENT mode) */
    uint32_t          post_record_sec;   /* post-roll window (EVENT mode) */
    uint32_t          segment_duration_sec;
    uint64_t          segment_size_max;
    uint32_t          ring_buf_size;     /* 0 → REC_BUF_SIZE_DEFAULT */
    uint32_t          flush_interval_sec;
    char              output_dir[256];
} rec_config_t;

/*
 * rec_engine_create(): Allocate and initialise all sub-modules.
 * Returns NULL on failure.
 */
rec_engine_t *rec_engine_create(const rec_config_t *cfg);

/*
 * rec_engine_get_epoll_fds():
 *   Fill fds_out[] with file descriptors that the caller must register with
 *   EPOLL_CTL_ADD (EPOLLIN) before calling rec_engine_handle_event().
 *   Returns the number of fds written (≤ max_fds).
 *   max_fds must be ≥ REC_MAX_EPOLL_FDS.
 *
 *   Caller owns the epoll instance; the engine never touches it.
 *   Call EPOLL_CTL_DEL on all returned fds BEFORE calling rec_engine_destroy().
 */
int rec_engine_get_epoll_fds(rec_engine_t *eng, int *fds_out, int max_fds);

/*
 * rec_engine_handle_event():
 *   Dispatch an fd that epoll reported as readable.
 *   Returns REC_OK or a negative REC_ERR_* code.
 */
int rec_engine_handle_event(rec_engine_t *eng, int fd);

/*
 * rec_engine_push_frame():
 *   Feed one encoded frame into the engine.
 *
 *   is_codec_config: true for SPS/PPS/VPS NALUs (published to codec-config
 *     cache; not counted as a recordable frame).
 *   is_keyframe: true for IDR/I-frames.
 *   format: VFR_FMT_H264 or VFR_FMT_H265.
 *
 *   The engine always pushes non-config frames into the pre-roll ring buffer.
 *   Whether the frame is also queued for file writing depends on the current
 *   mode and state machine state.
 *
 *   Returns REC_OK, REC_ERR_QUEUE_FULL, or REC_ERR_WRITER_STUCK.
 */
int rec_engine_push_frame(rec_engine_t *eng,
                          const uint8_t *data, uint32_t size,
                          uint64_t timestamp_ns, uint64_t seq_num,
                          bool is_keyframe, uint32_t format,
                          bool is_codec_config);

/*
 * rec_engine_destroy():
 *   Graceful shutdown: flush writer, close fds, free all memory.
 *   Pre-condition: caller has already removed all engine fds from epoll.
 *   Sets *eng = NULL.
 */
void rec_engine_destroy(rec_engine_t **eng);

/* Metrics / state accessors */
rec_state_t rec_engine_get_state(const rec_engine_t *eng);
uint64_t    rec_engine_get_written_bytes(const rec_engine_t *eng);
uint32_t    rec_engine_get_dropped_frames(const rec_engine_t *eng);

#endif /* REC_ENGINE_H */
