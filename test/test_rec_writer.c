#include <stdio.h>
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

int main(void)
{
    test_pat_structure();
    test_pat_cc_increment();
    printf("\nAll tests PASS\n");
    return 0;
}
