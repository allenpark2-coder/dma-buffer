#ifndef REC_WRITER_H
#define REC_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "rec_defs.h"
#include "rec_buf.h"

typedef struct {
    uint8_t  *data;
    uint32_t  size;
    uint64_t  timestamp_ns;
    uint64_t  seq_num;
    bool      is_keyframe;
    bool      is_segment_boundary;
    bool      is_shutdown_sentinel;
} rec_write_item_t;

typedef struct {
    rec_write_item_t  items[REC_WRITE_QUEUE_DEPTH];
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
} rec_write_queue_t;

#define REC_CODEC_CONFIG_MAX_SIZE  512

typedef struct {
    uint8_t  data[REC_CODEC_CONFIG_MAX_SIZE];
    uint32_t size;
    uint32_t format;
} rec_codec_blob_t;

typedef struct {
    rec_codec_blob_t  slots[2];
    _Atomic uint32_t  active_slot;
    _Atomic uint32_t  version;
} rec_codec_config_t;

typedef struct {
    uint32_t   segment_duration_sec;
    uint64_t   segment_size_max;
    uint32_t   flush_interval_sec;
    rec_mode_t mode;
    char       output_dir[256];
    char       stream_name[64];
} rec_writer_config_t;

typedef struct rec_writer rec_writer_t;

rec_writer_t *rec_writer_create(rec_buf_t          *ring,
                                rec_codec_config_t *codec_cfg,
                                const rec_writer_config_t *cfg);

int  rec_writer_get_eventfd(const rec_writer_t *w);
int  rec_writer_enqueue(rec_writer_t *w, rec_write_item_t item);
void rec_writer_publish_codec_config(rec_writer_t *w,
                                     const uint8_t *data, uint32_t size,
                                     uint32_t format);
void rec_writer_destroy(rec_writer_t **w);

uint64_t rec_writer_get_written_bytes(const rec_writer_t *w);
uint32_t rec_writer_get_dropped_frames(const rec_writer_t *w);

#endif /* REC_WRITER_H */
