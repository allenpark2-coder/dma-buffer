#ifndef REC_SEGMENT_H
#define REC_SEGMENT_H

#include <stdbool.h>
#include <stdint.h>
#include "rec_defs.h"

typedef struct rec_segment rec_segment_t;

typedef struct {
    char       output_dir[256];
    char       stream_name[VFR_SOCKET_NAME_MAX];
    rec_mode_t mode;
    uint32_t   duration_sec;
    uint64_t   size_max;
    uint32_t   flush_interval_sec;
} rec_segment_config_t;

rec_segment_t *rec_segment_create(const rec_segment_config_t *cfg);
void rec_segment_destroy(rec_segment_t **seg);

int rec_segment_write_access_unit(rec_segment_t  *seg,
                                  const uint8_t  *data,
                                  uint32_t        size,
                                  uint64_t        timestamp_ns,
                                  bool            is_keyframe,
                                  bool            is_segment_boundary,
                                  uint32_t        codec_format,
                                  const uint8_t  *codec_data,
                                  uint32_t        codec_size);

int rec_segment_flush(rec_segment_t *seg);
int rec_segment_close(rec_segment_t *seg);

uint64_t rec_segment_get_written_bytes(const rec_segment_t *seg);
const char *rec_segment_get_current_path(const rec_segment_t *seg);
bool rec_segment_is_open(const rec_segment_t *seg);

#endif /* REC_SEGMENT_H */
