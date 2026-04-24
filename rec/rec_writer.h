#ifndef REC_WRITER_H
#define REC_WRITER_H

#include <stdbool.h>
#include <stdint.h>
#include "rec_buf.h"
#include "rec_segment.h"

#define REC_CODEC_CONFIG_MAX_SIZE 512

typedef struct {
    uint8_t  *data;
    uint32_t  size;
    uint64_t  timestamp_ns;
    bool      is_keyframe;
    bool      is_segment_boundary;
    bool      is_shutdown_sentinel;
} rec_write_item_t;

typedef struct {
    rec_write_item_t  items[REC_WRITE_QUEUE_DEPTH];
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
} rec_write_queue_t;

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

typedef struct rec_writer rec_writer_t;

typedef struct {
    char       output_dir[256];
    char       stream_name[VFR_SOCKET_NAME_MAX];
    rec_mode_t mode;
    uint32_t   segment_duration_sec;
    uint64_t   segment_size_max;
    uint32_t   flush_interval_sec;
} rec_writer_config_t;

void rec_codec_config_init(rec_codec_config_t *cfg);
int  rec_codec_config_publish(rec_codec_config_t *cfg,
                              uint32_t            format,
                              const uint8_t      *data,
                              uint32_t            size);
bool rec_codec_config_snapshot(const rec_codec_config_t *cfg,
                               rec_codec_blob_t         *out,
                               uint32_t                 *out_version);

rec_writer_t *rec_writer_create(const rec_writer_config_t *cfg, rec_buf_t *buf);
void rec_writer_destroy(rec_writer_t **writer);

int rec_writer_get_eventfd(const rec_writer_t *writer);
int rec_writer_publish_codec_config(rec_writer_t *writer,
                                    uint32_t      format,
                                    const uint8_t *data,
                                    uint32_t      size);
int rec_writer_enqueue_live(rec_writer_t *writer,
                            const uint8_t *data,
                            uint32_t       size,
                            uint64_t       timestamp_ns,
                            bool           is_keyframe,
                            bool           is_segment_boundary);

uint64_t rec_writer_get_written_bytes(const rec_writer_t *writer);
uint32_t rec_writer_get_dropped_frames(const rec_writer_t *writer);
bool     rec_writer_is_overflow_dropping(const rec_writer_t *writer);

#endif /* REC_WRITER_H */
