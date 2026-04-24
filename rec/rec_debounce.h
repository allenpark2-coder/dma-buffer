/* rec/rec_debounce.h — 訊號消抖器（§5.3）
 *
 * 在 REC_DEBOUNCE_MS 內重複到來的 TRIGGER_START / TRIGGER_STOP 視為同一事件，
 * 只處理第一個，後續忽略。
 */
#ifndef REC_DEBOUNCE_H
#define REC_DEBOUNCE_H

#include <stdint.h>
#include <stdbool.h>
#include "rec_defs.h"

typedef struct {
    uint64_t last_start_ns;   /* 上一次被接受的 TRIGGER_START 時間（CLOCK_MONOTONIC ns）*/
                              /* 0 = 從未觸發 */
    uint64_t debounce_ns;     /* 消抖視窗大小（ns）*/
} rec_debounce_t;

/*
 * rec_debounce_init()：初始化消抖器，debounce_ms 為消抖視窗（毫秒）。
 */
void rec_debounce_init(rec_debounce_t *d, uint32_t debounce_ms);

/*
 * rec_debounce_allow_start()：
 *   若距上次被接受的 START < debounce_ns → 回傳 false（消抖，忽略）。
 *   否則更新 last_start_ns，回傳 true（允許處理）。
 */
bool rec_debounce_allow_start(rec_debounce_t *d, uint64_t now_ns);

/*
 * rec_debounce_allow_stop()：
 *   若距上次 START < debounce_ns → 回傳 false（忽略，防止毛刺）。
 *   否則回傳 true（允許處理）。
 *   注意：STOP 不更新 last_start_ns。
 */
bool rec_debounce_allow_stop(rec_debounce_t *d, uint64_t now_ns);

#endif /* REC_DEBOUNCE_H */
