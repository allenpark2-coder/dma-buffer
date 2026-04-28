/* rec/rec_schedule.h — Time-slot scheduling engine */
#ifndef REC_SCHEDULE_H
#define REC_SCHEDULE_H

#include <stdint.h>
#include <time.h>
#include "rec_defs.h"

/*
 * Schedule is stored as a 2-bit field per 15-minute slot.
 * 96 slots/day × 2 bits = 192 bits = 24 bytes per day.
 * days[0] = Sunday, days[6] = Saturday (matches tm_wday).
 */
typedef struct {
    uint8_t slots[24];   /* 96 slots × 2 bits = 24 bytes */
} rec_schedule_day_t;

typedef struct {
    rec_schedule_day_t days[7];   /* 0 = Sunday */
} rec_schedule_t;

/*
 * rec_schedule_init_always():
 *   Fills every slot in every day with the given mode.
 *   Use REC_SLOT_CONTINUOUS for 24/7 continuous recording,
 *   REC_SLOT_EVENT for always-event mode, REC_SLOT_OFF to disable.
 */
void rec_schedule_init_always(rec_schedule_t *sched, rec_slot_mode_t mode);

/*
 * rec_schedule_set_slot():
 *   Set a single 15-minute slot.
 *   wday: 0=Sunday .. 6=Saturday.
 *   slot_idx: 0..95 (slot 0 = 00:00-00:15, slot 95 = 23:45-24:00).
 */
void rec_schedule_set_slot(rec_schedule_t *sched, int wday, int slot_idx,
                           rec_slot_mode_t mode);

/*
 * rec_schedule_query():
 *   Returns the rec_slot_mode_t active at now_wall_clock (from time(NULL)).
 */
rec_slot_mode_t rec_schedule_query(const rec_schedule_t *sched,
                                   time_t now_wall_clock);

#endif /* REC_SCHEDULE_H */
