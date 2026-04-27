#ifndef REC_TS_MUX_H
#define REC_TS_MUX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define REC_TS_PACKET_SIZE    188
#define REC_TS_PID_PAT        0x0000
#define REC_TS_PID_PMT        0x0100
#define REC_TS_PID_VIDEO      0x0101

/* Caller allocates this many bytes for rec_ts_write_pes output */
#define REC_TS_PES_BUF_SIZE(payload_size) \
    (((uint32_t)(payload_size) / 184u + 2u) * 188u)

/*
 * All functions return the number of bytes written into buf,
 * or -1 if buf_size is too small.
 * Continuity counters are 4-bit; functions mask and increment in place.
 */

int rec_ts_write_pat(uint8_t *buf, size_t buf_size, uint8_t *cc_pat);

int rec_ts_write_pmt(uint8_t *buf, size_t buf_size,
                     uint32_t format,
                     uint8_t *cc_pmt);

int rec_ts_write_pes(uint8_t *buf, size_t buf_size,
                     const uint8_t *payload, uint32_t payload_size,
                     uint64_t pts_ns, uint64_t pcr_ns,
                     bool is_keyframe,
                     uint8_t *cc_video);

#endif /* REC_TS_MUX_H */
