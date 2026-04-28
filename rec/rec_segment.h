#ifndef REC_SEGMENT_H
#define REC_SEGMENT_H

#include <stdint.h>
#include <stddef.h>
#include "rec_defs.h"

typedef struct rec_segment rec_segment_t;

rec_segment_t *rec_segment_open(const char *output_dir,
                                const char *stream_name,
                                rec_mode_t  mode,
                                uint32_t    max_duration_sec,
                                uint64_t    max_size_bytes,
                                uint32_t    flush_interval_sec);

int rec_segment_write(rec_segment_t *seg,
                      const uint8_t *ts_buf, size_t size,
                      uint64_t duration_ns);

void rec_segment_close(rec_segment_t *seg);

uint64_t rec_segment_written_bytes(const rec_segment_t *seg);

#endif /* REC_SEGMENT_H */
