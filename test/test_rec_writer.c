#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include "rec_ts_mux.h"

static void assert_ts_header(const uint8_t *pkt, uint16_t pid,
                              bool pusi, uint8_t cc)
{
    assert(pkt[0] == 0x47);
    uint16_t actual_pid = ((uint16_t)(pkt[1] & 0x1F) << 8) | pkt[2];
    assert(actual_pid == pid);
    bool actual_pusi = !!(pkt[1] & 0x40);
    assert(actual_pusi == pusi);
    uint8_t actual_cc = pkt[3] & 0x0F;
    assert(actual_cc == (cc & 0x0F));
}

static void test_pat_structure(void)
{
    uint8_t buf[188];
    uint8_t cc = 0;
    int n = rec_ts_write_pat(buf, sizeof(buf), &cc);
    assert(n == 188);
    assert(cc == 1);
    assert_ts_header(buf, REC_TS_PID_PAT, true, 0);
    assert(buf[4] == 0x00);
    assert(buf[5] == 0x00);
    assert(buf[6] & 0x80);
    assert(buf[13] == 0x00 && buf[14] == 0x01);
    uint16_t pmt_pid = ((uint16_t)(buf[15] & 0x1F) << 8) | buf[16];
    assert(pmt_pid == REC_TS_PID_PMT);
    for (int i = 21; i < 188; i++)
        assert(buf[i] == 0xFF);
    printf("PASS: test_pat_structure\n");
}

static void test_pat_cc_increment(void)
{
    uint8_t buf[188];
    uint8_t cc = 0;
    rec_ts_write_pat(buf, sizeof(buf), &cc);
    rec_ts_write_pat(buf, sizeof(buf), &cc);
    assert((buf[3] & 0x0F) == 1);
    printf("PASS: test_pat_cc_increment\n");
}

/* ── T3: PMT packet structure ────────────────────────────────── */
#ifndef VFR_FMT_H264
#define VFR_FMT_H264 0x34363248u
#define VFR_FMT_H265 0x35363248u
#endif

static void test_pmt_structure(void)
{
    uint8_t buf[188];
    uint8_t cc = 0;
    int n = rec_ts_write_pmt(buf, sizeof(buf), VFR_FMT_H264, &cc);
    assert(n == 188);
    assert(cc == 1);

    assert_ts_header(buf, REC_TS_PID_PMT, true, 0);
    assert(buf[4] == 0x00);      /* pointer field */
    assert(buf[5] == 0x02);      /* table_id = PMT */
    assert(buf[6] & 0x80);       /* section_syntax_indicator */
    assert(buf[18] == 0x1B);     /* H.264 stream type */

    uint16_t epid = ((uint16_t)(buf[19] & 0x1F) << 8) | buf[20];
    assert(epid == REC_TS_PID_VIDEO);

    printf("PASS: test_pmt_structure\n");
}

/* ── T4: PES — single frame, multiple packets ────────────────── */
static void test_pes_basic(void)
{
    uint32_t payload_size = 300;
    size_t buf_sz = REC_TS_PES_BUF_SIZE(payload_size);
    uint8_t *buf = malloc(buf_sz);
    assert(buf != NULL);

    uint8_t payload[300];
    memset(payload, 0xAB, sizeof(payload));

    uint8_t cc = 0;
    int n = rec_ts_write_pes(buf, buf_sz, payload, payload_size,
                             90000000ull, 90000000ull, true, &cc);
    assert(n >= 188 * 2);
    assert(n % 188 == 0);

    assert_ts_header(buf, REC_TS_PID_VIDEO, true, 0);
    assert((buf[3] & 0x30) == 0x30);  /* adaptation + payload */

    int found = 0;
    for (int i = 4; i < 188 - 3; i++) {
        if (buf[i] == 0x00 && buf[i+1] == 0x00 && buf[i+2] == 0x01) {
            assert(buf[i+3] == 0xE0);
            found = 1;
            break;
        }
    }
    assert(found);

    for (int p = 0; p < n; p += 188)
        assert(buf[p] == 0x47);

    free(buf);
    printf("PASS: test_pes_basic\n");
}

/* ── T5: PES — CC increments across packets ──────────────────── */
static void test_pes_cc(void)
{
    uint32_t payload_size = 400;
    size_t buf_sz = REC_TS_PES_BUF_SIZE(payload_size);
    uint8_t *buf = malloc(buf_sz);
    uint8_t payload[400]; memset(payload, 0x55, sizeof(payload));
    uint8_t cc = 5;
    int n = rec_ts_write_pes(buf, buf_sz, payload, payload_size,
                             0, 0, false, &cc);
    assert(n >= 188 * 3);
    assert((buf[3] & 0x0F) == 5);
    assert((buf[188+3] & 0x0F) == 6);
    assert((buf[376+3] & 0x0F) == 7);
    free(buf);
    printf("PASS: test_pes_cc\n");
}

int main(void)
{
    test_pat_structure();
    test_pat_cc_increment();
    test_pmt_structure();
    test_pes_basic();
    test_pes_cc();
    printf("\nAll tests PASS\n");
    return 0;
}
