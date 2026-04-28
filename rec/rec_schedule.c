/* rec/rec_schedule.c — Time-slot scheduling engine */
#include "rec_schedule.h"
#include <string.h>

void rec_schedule_init_always(rec_schedule_t *sched, rec_slot_mode_t mode)
{
    for (int d = 0; d < 7; d++) {
        for (int s = 0; s < REC_SCHEDULE_SLOTS_PER_DAY; s++) {
            int bit_offset = s * 2;
            uint8_t *byte  = &sched->days[d].slots[bit_offset / 8];
            int      shift = bit_offset % 8;
            *byte = (uint8_t)((*byte & ~(uint8_t)(0x3u << shift)) |
                              ((uint8_t)(mode & 0x3u) << shift));
        }
    }
}

void rec_schedule_set_slot(rec_schedule_t *sched, int wday, int slot_idx,
                           rec_slot_mode_t mode)
{
    if (wday < 0 || wday > 6)                          return;
    if (slot_idx < 0 || slot_idx >= REC_SCHEDULE_SLOTS_PER_DAY) return;

    int      bit_offset = slot_idx * 2;
    uint8_t *byte       = &sched->days[wday].slots[bit_offset / 8];
    int      shift      = bit_offset % 8;
    *byte = (uint8_t)((*byte & ~(uint8_t)(0x3u << shift)) |
                      ((uint8_t)(mode & 0x3u) << shift));
}

rec_slot_mode_t rec_schedule_query(const rec_schedule_t *sched,
                                   time_t now_wall_clock)
{
    struct tm t;
    localtime_r(&now_wall_clock, &t);
    int slot       = (t.tm_hour * 60 + t.tm_min) / 15;
    int bit_offset = slot * 2;
    uint8_t byte   = sched->days[t.tm_wday].slots[bit_offset / 8];
    return (rec_slot_mode_t)((byte >> (bit_offset % 8)) & 0x3u);
}
