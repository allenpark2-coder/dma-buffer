/* rec/rec_schedule.c — 時間位元遮罩排程引擎（§8）*/
#define _GNU_SOURCE
#include <string.h>
#include "rec_schedule.h"

/* ─── rec_schedule_set_all ───────────────────────────────────────── */
void rec_schedule_set_all(rec_schedule_t *sched, rec_slot_mode_t mode)
{
    uint8_t nibble   = (uint8_t)mode & 0x3u;
    uint8_t byte_val = (uint8_t)(nibble | (nibble << 2) | (nibble << 4) | (nibble << 6));

    for (int d = 0; d < 7; d++)
        memset(sched->days[d].slots, byte_val, sizeof(sched->days[d].slots));
}

/* ─── rec_schedule_query ─────────────────────────────────────────── */
rec_slot_mode_t rec_schedule_query(const rec_schedule_t *sched,
                                    time_t now_wall_clock)
{
    struct tm t;
    localtime_r(&now_wall_clock, &t);

    /* 計算當天第幾個 15 分鐘格（0 ~ 95）*/
    int slot       = (t.tm_hour * 60 + t.tm_min) / 15;
    int bit_offset = slot * 2;

    uint8_t byte = sched->days[t.tm_wday].slots[bit_offset / 8];
    return (rec_slot_mode_t)((byte >> (bit_offset % 8)) & 0x3u);
}
