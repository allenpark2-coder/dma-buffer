#include "rec_segment.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

struct rec_segment {
    int      fd;
    uint64_t written_bytes;
    uint64_t cumulative_ns;
    uint64_t last_flush_ns;
    uint32_t max_duration_sec;
    uint64_t max_size_bytes;
    uint32_t flush_interval_sec;
};

rec_segment_t *rec_segment_open(const char *output_dir,
                                const char *stream_name,
                                rec_mode_t  mode,
                                uint32_t    max_duration_sec,
                                uint64_t    max_size_bytes,
                                uint32_t    flush_interval_sec)
{
    rec_segment_t *seg = calloc(1, sizeof(*seg));
    if (!seg) return NULL;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    const char *mode_str = (mode == REC_MODE_EVENT) ? "event" : "cont";
    char path[512];
    snprintf(path, sizeof(path),
             "%s/%s_%04d%02d%02d_%02d%02d%02d_%s.ts",
             output_dir, stream_name,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             mode_str);

    seg->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (seg->fd < 0) { free(seg); return NULL; }

    seg->max_duration_sec   = max_duration_sec;
    seg->max_size_bytes     = max_size_bytes;
    seg->flush_interval_sec = flush_interval_sec;
    return seg;
}

int rec_segment_write(rec_segment_t *seg,
                      const uint8_t *ts_buf, size_t size,
                      uint64_t duration_ns)
{
    /* Loop to handle short writes (e.g. ENOSPC / interrupted write on flash). */
    size_t remaining = size;
    const uint8_t *ptr = ts_buf;
    while (remaining > 0) {
        ssize_t written = write(seg->fd, ptr, remaining);
        if (written <= 0) return -1;   /* error or unexpected EOF */
        ptr       += (size_t)written;
        remaining -= (size_t)written;
    }
    seg->written_bytes += (uint64_t)size;
    seg->cumulative_ns += duration_ns;
    seg->last_flush_ns += duration_ns;

    if (seg->flush_interval_sec > 0) {
        uint64_t flush_threshold = (uint64_t)seg->flush_interval_sec * 1000000000ull;
        if (seg->last_flush_ns >= flush_threshold) {
            fdatasync(seg->fd);
            seg->last_flush_ns = 0;
        }
    }

    uint64_t dur_threshold = (uint64_t)seg->max_duration_sec * 1000000000ull;
    if (seg->cumulative_ns  >= dur_threshold)       return 1;
    if (seg->written_bytes  >= seg->max_size_bytes) return 1;
    return 0;
}

void rec_segment_close(rec_segment_t *seg)
{
    if (!seg) return;
    fdatasync(seg->fd);
    close(seg->fd);
    free(seg);
}

uint64_t rec_segment_written_bytes(const rec_segment_t *seg)
{
    return seg ? seg->written_bytes : 0;
}
