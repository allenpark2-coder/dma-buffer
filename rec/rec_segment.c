#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "rec_segment.h"

#define REC_TS_PACKET_SIZE 188
#define REC_TS_PID_PAT     0x0000u
#define REC_TS_PID_PMT     0x0100u
#define REC_TS_PID_VIDEO   0x0101u

struct rec_segment {
    rec_segment_config_t cfg;
    int                  fd;
    uint64_t             total_written_bytes;
    uint64_t             current_size;
    uint64_t             segment_start_ns;
    time_t               last_flush_wall;
    bool                 rotate_pending;
    uint8_t              cc_pat;
    uint8_t              cc_pmt;
    uint8_t              cc_video;
    char                 current_path[512];
};

static uint32_t rec_segment_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;

    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u)
                ? (crc << 1) ^ 0x04c11db7u
                : (crc << 1);
        }
    }

    return crc;
}

static int rec_segment_write_all(int fd, const void *buf, size_t size)
{
    const uint8_t *p = buf;
    size_t total = 0;

    while (total < size) {
        ssize_t n = write(fd, p + total, size - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        total += (size_t)n;
    }

    return 0;
}

static const char *rec_segment_mode_suffix(rec_mode_t mode)
{
    switch (mode) {
    case REC_MODE_CONTINUOUS: return "cont";
    case REC_MODE_SCHEDULED:  return "sched";
    case REC_MODE_EVENT:      return "event";
    default:                  return "unk";
    }
}

static uint8_t rec_segment_stream_type(uint32_t codec_format)
{
    return codec_format == VFR_FMT_H265 ? 0x24u : 0x1bu;
}

static void rec_segment_encode_pts(uint8_t out[5], uint64_t pts90k)
{
    uint64_t pts = pts90k & ((1ULL << 33) - 1ULL);

    out[0] = (uint8_t)(0x20u | (((pts >> 30) & 0x07u) << 1) | 0x01u);
    out[1] = (uint8_t)(pts >> 22);
    out[2] = (uint8_t)((((pts >> 15) & 0x7fu) << 1) | 0x01u);
    out[3] = (uint8_t)(pts >> 7);
    out[4] = (uint8_t)(((pts & 0x7fu) << 1) | 0x01u);
}

static int rec_segment_write_ts_payload(rec_segment_t *seg,
                                        uint16_t       pid,
                                        uint8_t       *cc,
                                        const uint8_t *payload,
                                        size_t         payload_size)
{
    size_t offset = 0;
    bool first = true;

    while (offset < payload_size) {
        uint8_t packet[REC_TS_PACKET_SIZE];
        memset(packet, 0xff, sizeof(packet));

        size_t remaining = payload_size - offset;
        bool use_adaptation = remaining < 184;
        size_t payload_offset = 4;
        uint8_t afc = 1;

        if (use_adaptation) {
            size_t adaptation_length = 183 - remaining;
            afc = 3;
            packet[4] = (uint8_t)adaptation_length;
            if (adaptation_length > 0) {
                packet[5] = 0x00;
                if (adaptation_length > 1) {
                    memset(packet + 6, 0xff, adaptation_length - 1);
                }
            }
            payload_offset = 5 + adaptation_length;
        }

        packet[0] = 0x47;
        packet[1] = (uint8_t)(((first ? 0x40u : 0x00u) | ((pid >> 8) & 0x1fu)));
        packet[2] = (uint8_t)(pid & 0xffu);
        packet[3] = (uint8_t)((afc << 4) | (*cc & 0x0fu));

        size_t chunk = REC_TS_PACKET_SIZE - payload_offset;
        if (chunk > remaining)
            chunk = remaining;

        memcpy(packet + payload_offset, payload + offset, chunk);

        int rc = rec_segment_write_all(seg->fd, packet, sizeof(packet));
        if (rc < 0)
            return rc;

        seg->current_size += sizeof(packet);
        seg->total_written_bytes += sizeof(packet);
        *cc = (uint8_t)((*cc + 1u) & 0x0fu);
        offset += chunk;
        first = false;
    }

    return 0;
}

static int rec_segment_write_pat(rec_segment_t *seg)
{
    uint8_t section[32];
    uint8_t payload[184];
    size_t len = 0;

    section[len++] = 0x00;
    section[len++] = 0xb0;
    section[len++] = 0x0d;
    section[len++] = 0x00;
    section[len++] = 0x01;
    section[len++] = 0xc1;
    section[len++] = 0x00;
    section[len++] = 0x00;
    section[len++] = 0x00;
    section[len++] = 0x01;
    section[len++] = 0xe1;
    section[len++] = 0x00;

    uint32_t crc = rec_segment_crc32(section, len);
    section[len++] = (uint8_t)(crc >> 24);
    section[len++] = (uint8_t)(crc >> 16);
    section[len++] = (uint8_t)(crc >> 8);
    section[len++] = (uint8_t)crc;

    memset(payload, 0xff, sizeof(payload));
    payload[0] = 0x00;
    memcpy(payload + 1, section, len);

    return rec_segment_write_ts_payload(seg, REC_TS_PID_PAT, &seg->cc_pat,
                                        payload, sizeof(payload));
}

static int rec_segment_write_pmt(rec_segment_t *seg, uint32_t codec_format)
{
    uint8_t section[32];
    uint8_t payload[184];
    size_t len = 0;

    section[len++] = 0x02;
    section[len++] = 0xb0;
    section[len++] = 0x12;
    section[len++] = 0x00;
    section[len++] = 0x01;
    section[len++] = 0xc1;
    section[len++] = 0x00;
    section[len++] = 0x00;
    section[len++] = 0xe1;
    section[len++] = 0x01;
    section[len++] = 0xf0;
    section[len++] = 0x00;
    section[len++] = rec_segment_stream_type(codec_format);
    section[len++] = 0xe1;
    section[len++] = 0x01;
    section[len++] = 0xf0;
    section[len++] = 0x00;

    uint32_t crc = rec_segment_crc32(section, len);
    section[len++] = (uint8_t)(crc >> 24);
    section[len++] = (uint8_t)(crc >> 16);
    section[len++] = (uint8_t)(crc >> 8);
    section[len++] = (uint8_t)crc;

    memset(payload, 0xff, sizeof(payload));
    payload[0] = 0x00;
    memcpy(payload + 1, section, len);

    return rec_segment_write_ts_payload(seg, REC_TS_PID_PMT, &seg->cc_pmt,
                                        payload, sizeof(payload));
}

static int rec_segment_write_pes(rec_segment_t *seg,
                                 const uint8_t *data,
                                 uint32_t       size,
                                 uint64_t       timestamp_ns)
{
    uint8_t pts[5];
    uint8_t *payload;
    uint64_t pts90k = (timestamp_ns * 9ULL) / 100000ULL;
    size_t payload_size = 14u + (size_t)size;

    payload = malloc(payload_size);
    if (!payload)
        return -ENOMEM;

    rec_segment_encode_pts(pts, pts90k);

    payload[0] = 0x00;
    payload[1] = 0x00;
    payload[2] = 0x01;
    payload[3] = 0xe0;
    payload[4] = 0x00;
    payload[5] = 0x00;
    payload[6] = 0x80;
    payload[7] = 0x80;
    payload[8] = 0x05;
    memcpy(payload + 9, pts, sizeof(pts));
    memcpy(payload + 14, data, size);

    int rc = rec_segment_write_ts_payload(seg, REC_TS_PID_VIDEO, &seg->cc_video,
                                          payload, payload_size);
    free(payload);
    return rc;
}

static int rec_segment_ensure_output_dir(const char *path)
{
    if (mkdir(path, 0777) == 0 || errno == EEXIST)
        return 0;
    return -errno;
}

static int rec_segment_open_path(char *path,
                                 size_t      path_size,
                                 const rec_segment_t *seg,
                                 const char *stamp,
                                 uint32_t    suffix_seq)
{
    if (suffix_seq == 0) {
        return snprintf(path, path_size, "%s/%s_%s_%s.ts",
                        seg->cfg.output_dir,
                        seg->cfg.stream_name,
                        stamp,
                        rec_segment_mode_suffix(seg->cfg.mode)) >= (int)path_size
            ? -ENAMETOOLONG
            : 0;
    }

    return snprintf(path, path_size, "%s/%s_%s_%s_%u.ts",
                    seg->cfg.output_dir,
                    seg->cfg.stream_name,
                    stamp,
                    rec_segment_mode_suffix(seg->cfg.mode),
                    suffix_seq) >= (int)path_size
        ? -ENAMETOOLONG
        : 0;
}

static int rec_segment_open_new(rec_segment_t *seg,
                                uint64_t       timestamp_ns,
                                uint32_t       codec_format,
                                const uint8_t *codec_data,
                                uint32_t       codec_size)
{
    time_t now = time(NULL);
    struct tm tm_now;
    char stamp[32];
    char path[512];

    if (codec_size == 0)
        return -EAGAIN;

    int rc = rec_segment_ensure_output_dir(seg->cfg.output_dir);
    if (rc < 0)
        return rc;

    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_now);
    uint32_t suffix_seq = 0;
    for (;;) {
        rc = rec_segment_open_path(path, sizeof(path), seg, stamp, suffix_seq);
        if (rc < 0)
            return rc;

        seg->fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666);
        if (seg->fd >= 0)
            break;

        if (errno != EEXIST)
            return -errno;

        ++suffix_seq;
    }

    snprintf(seg->current_path, sizeof(seg->current_path), "%s", path);
    seg->current_size = 0;
    seg->segment_start_ns = timestamp_ns;
    seg->rotate_pending = false;
    seg->cc_pat = 0;
    seg->cc_pmt = 0;
    seg->cc_video = 0;
    seg->last_flush_wall = now;

    rc = rec_segment_write_pat(seg);
    if (rc < 0)
        goto fail_open;

    rc = rec_segment_write_pmt(seg, codec_format);
    if (rc < 0)
        goto fail_open;

    rc = rec_segment_write_pes(seg, codec_data, codec_size, timestamp_ns);
    if (rc < 0)
        goto fail_open;

    return 0;

fail_open:
    (void)close(seg->fd);
    (void)unlink(seg->current_path);
    seg->fd = -1;
    seg->current_size = 0;
    seg->segment_start_ns = 0;
    seg->rotate_pending = false;
    seg->cc_pat = 0;
    seg->cc_pmt = 0;
    seg->cc_video = 0;
    seg->current_path[0] = '\0';
    return rc;
}

static bool rec_segment_needs_rotate(const rec_segment_t *seg, uint64_t timestamp_ns)
{
    if (seg->fd < 0)
        return false;

    if (seg->cfg.size_max > 0 && seg->current_size >= seg->cfg.size_max)
        return true;

    if (seg->cfg.duration_sec > 0 &&
        timestamp_ns >= seg->segment_start_ns &&
        timestamp_ns - seg->segment_start_ns >=
            (uint64_t)seg->cfg.duration_sec * 1000000000ULL)
        return true;

    return false;
}

rec_segment_t *rec_segment_create(const rec_segment_config_t *cfg)
{
    if (!cfg || cfg->output_dir[0] == '\0' || cfg->stream_name[0] == '\0')
        return NULL;

    rec_segment_t *seg = calloc(1, sizeof(*seg));
    if (!seg)
        return NULL;

    seg->cfg = *cfg;
    seg->fd = -1;
    return seg;
}

void rec_segment_destroy(rec_segment_t **seg)
{
    if (!seg || !*seg)
        return;

    (void)rec_segment_close(*seg);
    free(*seg);
    *seg = NULL;
}

int rec_segment_flush(rec_segment_t *seg)
{
    if (!seg || seg->fd < 0)
        return 0;

    if (fdatasync(seg->fd) < 0)
        return -errno;

    seg->last_flush_wall = time(NULL);
    return 0;
}

int rec_segment_close(rec_segment_t *seg)
{
    if (!seg || seg->fd < 0)
        return 0;

    int rc = rec_segment_flush(seg);
    if (close(seg->fd) < 0 && rc == 0)
        rc = -errno;

    seg->fd = -1;
    seg->current_size = 0;
    seg->segment_start_ns = 0;
    seg->rotate_pending = false;
    seg->current_path[0] = '\0';
    return rc;
}

int rec_segment_write_access_unit(rec_segment_t  *seg,
                                  const uint8_t  *data,
                                  uint32_t        size,
                                  uint64_t        timestamp_ns,
                                  bool            is_keyframe,
                                  bool            is_segment_boundary,
                                  uint32_t        codec_format,
                                  const uint8_t  *codec_data,
                                  uint32_t        codec_size)
{
    if (!seg || !data || size == 0)
        return -EINVAL;

    if (seg->fd < 0) {
        if (!is_keyframe)
            return -EAGAIN;

        int rc = rec_segment_open_new(seg, timestamp_ns, codec_format,
                                      codec_data, codec_size);
        if (rc < 0)
            return rc;
    } else if ((is_segment_boundary || seg->rotate_pending) && is_keyframe) {
        int rc = rec_segment_close(seg);
        if (rc < 0)
            return rc;

        rc = rec_segment_open_new(seg, timestamp_ns, codec_format,
                                  codec_data, codec_size);
        if (rc < 0)
            return rc;
    }

    int rc = rec_segment_write_pes(seg, data, size, timestamp_ns);
    if (rc < 0)
        return rc;

    if (rec_segment_needs_rotate(seg, timestamp_ns))
        seg->rotate_pending = true;

    if (seg->cfg.flush_interval_sec > 0) {
        time_t now = time(NULL);
        if (now - seg->last_flush_wall >= (time_t)seg->cfg.flush_interval_sec) {
            rc = rec_segment_flush(seg);
            if (rc < 0)
                return rc;
        }
    }

    return 0;
}

uint64_t rec_segment_get_written_bytes(const rec_segment_t *seg)
{
    return seg ? seg->total_written_bytes : 0;
}

const char *rec_segment_get_current_path(const rec_segment_t *seg)
{
    return seg ? seg->current_path : "";
}

bool rec_segment_is_open(const rec_segment_t *seg)
{
    return seg && seg->fd >= 0;
}
