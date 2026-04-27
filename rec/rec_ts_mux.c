#include "rec_ts_mux.h"
#include <string.h>
#include <stdint.h>

static uint32_t crc32_mpeg(const uint8_t *data, int len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
    }
    return crc;
}

int rec_ts_write_pat(uint8_t *buf, size_t buf_size, uint8_t *cc_pat)
{
    if (buf_size < 188) return -1;
    memset(buf, 0xFF, 188);

    buf[0] = 0x47;
    buf[1] = 0x40;
    buf[2] = 0x00;
    buf[3] = 0x10 | (*cc_pat & 0x0F);
    (*cc_pat)++;

    buf[4] = 0x00;

    uint8_t *s = buf + 5;
    s[0] = 0x00;
    s[1] = 0xB0 | 0x00;
    s[2] = 13;
    s[3] = 0x00; s[4] = 0x01;
    s[5] = 0xC1;
    s[6] = 0x00;
    s[7] = 0x00;
    s[8]  = 0x00; s[9]  = 0x01;
    s[10] = 0xE0 | (REC_TS_PID_PMT >> 8);
    s[11] = REC_TS_PID_PMT & 0xFF;
    uint32_t crc = crc32_mpeg(s, 12);
    s[12] = (crc >> 24) & 0xFF;
    s[13] = (crc >> 16) & 0xFF;
    s[14] = (crc >>  8) & 0xFF;
    s[15] =  crc        & 0xFF;
    return 188;
}

int rec_ts_write_pmt(uint8_t *buf, size_t buf_size, uint32_t format, uint8_t *cc_pmt)
{
    (void)buf; (void)buf_size; (void)format; (void)cc_pmt;
    return -1;
}

int rec_ts_write_pes(uint8_t *buf, size_t buf_size,
                     const uint8_t *payload, uint32_t payload_size,
                     uint64_t pts_ns, uint64_t pcr_ns,
                     bool is_keyframe, uint8_t *cc_video)
{
    (void)buf; (void)buf_size; (void)payload; (void)payload_size;
    (void)pts_ns; (void)pcr_ns; (void)is_keyframe; (void)cc_video;
    return -1;
}
