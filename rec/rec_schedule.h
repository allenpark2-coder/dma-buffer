/* rec/rec_schedule.h — 時間位元遮罩排程引擎（§8）
 *
 * 排程以每 15 分鐘一格（slot）為單位，每格 2 bits 表示模式：
 *   00 = REC_SLOT_OFF
 *   01 = REC_SLOT_CONTINUOUS
 *   10 = REC_SLOT_EVENT
 *
 * 一天 24h × 4 = 96 slots × 2 bits = 192 bits = 24 bytes
 * 一週 7 天 = 7 × 24 = 168 bytes
 */
#ifndef REC_SCHEDULE_H
#define REC_SCHEDULE_H

#include <stdint.h>
#include <time.h>
#include "rec_defs.h"

/* ─── 排程結構 ────────────────────────────────────────────────────── */
typedef struct {
    uint8_t slots[24];   /* 96 slots × 2 bits = 24 bytes */
} rec_schedule_day_t;

typedef struct {
    rec_schedule_day_t days[7];   /* 0 = Sunday */
} rec_schedule_t;

/* ─── 初始化輔助 ──────────────────────────────────────────────────── */

/*
 * rec_schedule_set_all()：將整週每個 slot 設成同一個模式。
 * 用於測試或建立預設排程。
 */
void rec_schedule_set_all(rec_schedule_t *sched, rec_slot_mode_t mode);

/* ─── 排程查詢 ────────────────────────────────────────────────────── */

/*
 * rec_schedule_query()：查詢指定 wall clock 時刻對應的 slot 模式。
 * now_wall_clock：time(NULL) 回傳值（UTC wall clock）。
 * 回傳：該 15 分鐘格對應的 rec_slot_mode_t。
 */
rec_slot_mode_t rec_schedule_query(const rec_schedule_t *sched,
                                    time_t now_wall_clock);

#endif /* REC_SCHEDULE_H */
