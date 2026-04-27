#include "rec_ts_mux.h"
#include <string.h>
#include <stdint.h>

/* PAT section_length: counts from after section_length field to end of CRC.
 * Fixed for single-program PAT: ts_id(2)+flags(1)+sec_no(1)+last_sec_no(1)
 *                               + program_entry(4) + CRC(4) = 13 */
#define PAT_SECTION_LEN  13

/* PMT section_length = 18:
 *   program_number(2)+flags(1)+sec_no(1)+last_sec_no(1) = 5
 *   PCR_PID+reserved(2) + program_info_length+reserved(2) = 4
 *   ES entry: stream_type(1)+E_PID+reserved(2)+ES_info_len+reserved(2) = 5
 *   CRC(4) = 4
 *   Total = 5+4+5+4 = 18 */
#define PMT_SECTION_LEN  18

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
    s[1] = 0xB0;                /* SSI=1, '0'=0, reserved=11, section_length[11:8]=0 */
    s[2] = PAT_SECTION_LEN;
    s[3] = 0x00; s[4] = 0x01;
    s[5] = 0xC1;
    s[6] = 0x00;
    s[7] = 0x00;
    s[8]  = 0x00; s[9]  = 0x01;
    s[10] = 0xE0 | (REC_TS_PID_PMT >> 8);
    s[11] = REC_TS_PID_PMT & 0xFF;
    /* CRC32 covers table_id through last program entry (12 bytes = PAT_SECTION_LEN - 1 CRC field) */
    uint32_t crc = crc32_mpeg(s, PAT_SECTION_LEN - 1);
    s[12] = (crc >> 24) & 0xFF;
    s[13] = (crc >> 16) & 0xFF;
    s[14] = (crc >>  8) & 0xFF;
    s[15] =  crc        & 0xFF;
    return 188;
}

int rec_ts_write_pmt(uint8_t *buf, size_t buf_size, uint32_t format, uint8_t *cc_pmt)
{
    if (buf_size < 188) return -1;
    memset(buf, 0xFF, 188);

    uint8_t stream_type = (format == 0x34363248u) ? 0x1Bu : 0x24u;

    buf[0] = 0x47;
    buf[1] = 0x40 | (REC_TS_PID_PMT >> 8);
    buf[2] = REC_TS_PID_PMT & 0xFF;
    buf[3] = 0x10 | (*cc_pmt & 0x0F);
    (*cc_pmt)++;

    buf[4] = 0x00;

    uint8_t *s = buf + 5;
    s[0] = 0x02;                /* table_id = PMT */
    s[1] = 0xB0;                /* SSI=1, '0'=0, reserved=11, len[11:8]=0 */
    s[2] = PMT_SECTION_LEN;
    s[3] = 0x00; s[4] = 0x01;  /* program_number = 1 */
    s[5] = 0xC1;                /* reserved=11, version=0, current_next=1 */
    s[6] = 0x00;                /* section_number */
    s[7] = 0x00;                /* last_section_number */
    s[8]  = 0xE0 | (REC_TS_PID_VIDEO >> 8);  /* PCR_PID high */
    s[9]  = REC_TS_PID_VIDEO & 0xFF;          /* PCR_PID low */
    s[10] = 0xF0; s[11] = 0x00;               /* program_info_length = 0 */
    s[12] = stream_type;                       /* stream_type at s[12] = buf[17] */
    s[13] = 0xE0 | (REC_TS_PID_VIDEO >> 8);  /* ES PID high */
    s[14] = REC_TS_PID_VIDEO & 0xFF;          /* ES PID low */
    s[15] = 0xF0; s[16] = 0x00;               /* ES_info_length = 0 */
    /* CRC covers s[0..PMT_SECTION_LEN-5] = s[0..13]... actually s[0..16] = 17 bytes */
    uint32_t crc = crc32_mpeg(s, PMT_SECTION_LEN - 1);  /* 17 bytes */
    s[17] = (crc >> 24) & 0xFF;
    s[18] = (crc >> 16) & 0xFF;
    s[19] = (crc >>  8) & 0xFF;
    s[20] =  crc        & 0xFF;
    return 188;
}

static uint64_t ns_to_90khz(uint64_t ns) { return ns * 9u / 100000u; }

static void encode_pts(uint8_t *out, uint64_t pts_90khz, uint8_t prefix)
{
    out[0] = (prefix << 4) | (uint8_t)((pts_90khz >> 29) & 0x0E) | 0x01;
    out[1] = (uint8_t)((pts_90khz >> 22) & 0xFF);
    out[2] = (uint8_t)((pts_90khz >> 14) & 0xFE) | 0x01;
    out[3] = (uint8_t)((pts_90khz >>  7) & 0xFF);
    out[4] = (uint8_t)((pts_90khz <<  1) & 0xFE) | 0x01;
}

static void write_pcr(uint8_t *out, uint64_t pcr_ns)
{
    uint64_t base = ns_to_90khz(pcr_ns);
    uint64_t ext  = (pcr_ns * 27u / 1000u) % 300u;
    out[0] = (base >> 25) & 0xFF;
    out[1] = (base >> 17) & 0xFF;
    out[2] = (base >>  9) & 0xFF;
    out[3] = (base >>  1) & 0xFF;
    out[4] = ((base & 1u) << 7) | 0x7E | (uint8_t)((ext >> 8) & 0x01);
    out[5] = ext & 0xFF;
}

int rec_ts_write_pes(uint8_t *buf, size_t buf_size,
                     const uint8_t *payload, uint32_t payload_size,
                     uint64_t pts_ns, uint64_t pcr_ns,
                     bool is_keyframe, uint8_t *cc_video)
{
    /* First packet layout:
     *   4 bytes TS header
     *   8 bytes adaptation field (length=7, PCR_flag=1, 6 bytes PCR)
     *   14 bytes PES header (start_code + stream_id + len + flags + PTS)
     *   162 bytes payload (188 - 4 - 8 - 14 = 162)
     */
    const uint32_t adapt_size   = 8;   /* always carry PCR */
    const uint32_t pes_hdr_size = 14;
    const uint32_t first_cap    = 188 - 4 - adapt_size - pes_hdr_size; /* 162 */

    uint32_t total_packets;
    if (payload_size <= first_cap) {
        total_packets = 1;
    } else {
        uint32_t after_first = payload_size - first_cap;
        total_packets = 1 + (after_first + 183) / 184;
    }
    size_t total_bytes = (size_t)total_packets * 188;
    if (buf_size < total_bytes) return -1;

    uint8_t *p = buf;
    const uint8_t *src = payload;

    /* ── First packet ─────────────────────────────────── */
    p[0] = 0x47;
    p[1] = 0x40 | (uint8_t)(REC_TS_PID_VIDEO >> 8);
    p[2] = (uint8_t)(REC_TS_PID_VIDEO & 0xFF);
    p[3] = 0x30 | (*cc_video & 0x0F);
    (*cc_video)++;

    p[4] = adapt_size - 1;          /* adaptation_field_length = 7 */
    p[5] = 0x10;                    /* PCR_flag = 1 */
    if (is_keyframe) p[5] |= 0x40; /* random_access_indicator */
    write_pcr(p + 6, pcr_ns);

    uint8_t *pes = p + 4 + adapt_size;  /* = p + 12 */
    pes[0] = 0x00; pes[1] = 0x00; pes[2] = 0x01; pes[3] = 0xE0;
    pes[4] = 0x00; pes[5] = 0x00;  /* PES_packet_length = 0 (unbounded) */
    pes[6] = 0x81;                  /* marker=10, original_or_copy=1, other flags=0 */
    pes[7] = 0x80;                  /* PTS_DTS_flags = 10 (PTS only) */
    pes[8] = 0x05;                  /* PES_header_data_length */
    encode_pts(pes + 9, ns_to_90khz(pts_ns), 0x02);

    uint8_t *data_start = pes + pes_hdr_size;  /* = p + 26 */
    uint32_t copy1 = (payload_size < first_cap) ? payload_size : first_cap;
    memcpy(data_start, src, copy1);
    src += copy1;
    if (copy1 < first_cap)
        memset(data_start + copy1, 0xFF, first_cap - copy1);

    uint32_t remaining = payload_size - copy1;
    p += 188;

    /* ── Continuation packets ─────────────────────────── */
    while (remaining > 0) {
        uint32_t chunk = (remaining < 184) ? remaining : 184;
        p[0] = 0x47;
        p[1] = (uint8_t)(REC_TS_PID_VIDEO >> 8);
        p[2] = (uint8_t)(REC_TS_PID_VIDEO & 0xFF);

        if (chunk < 184) {
            uint32_t stuffing = 184 - chunk;
            p[3] = 0x30 | (*cc_video & 0x0F);
            p[4] = stuffing - 1;           /* adaptation_field_length */
            if (stuffing >= 2) {
                p[5] = 0x00;               /* flags byte (only valid when AFL >= 1) */
                if (stuffing > 2)
                    memset(p + 6, 0xFF, stuffing - 2);  /* stuffing bytes */
            }
            memcpy(p + 4 + stuffing, src, chunk);
        } else {
            p[3] = 0x10 | (*cc_video & 0x0F);
            memcpy(p + 4, src, 184);
        }
        (*cc_video)++;
        src += chunk;
        remaining -= chunk;
        p += 188;
    }

    return (int)total_bytes;
}
