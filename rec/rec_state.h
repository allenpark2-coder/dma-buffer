/* rec/rec_state.h — 五狀態錄影狀態機（§7）
 *
 * 事件驅動；event loop 在以下情境呼叫對應函式：
 *   - AI trigger 到來         → rec_state_on_trigger()
 *   - timerfd 觸發            → rec_state_on_timer()
 *   - 收到 IDR frame          → rec_state_on_keyframe()
 *   - 排程關閉 / 手動停止     → rec_state_force_idle()
 *
 * Phase R2 限制：writer_eventfd 傳 -1 可省略喚醒 Writer（無寫入模式）。
 */
#ifndef REC_STATE_H
#define REC_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "rec_defs.h"
#include "rec_buf.h"
#include "rec_schedule.h"
#include "rec_debounce.h"

/* ─── 狀態轉移回呼（用於日誌 / 測試驗證）────────────────────────── */
typedef void (*rec_state_cb_t)(void        *ctx,
                                 rec_state_t  from,
                                 rec_state_t  to);

/* ─── 初始化組態 ──────────────────────────────────────────────────── */
typedef struct {
    rec_mode_t     mode;
    rec_schedule_t schedule;
    uint32_t       pre_record_sec;   /* 預錄秒數（0 = 從最近 keyframe）*/
    uint32_t       post_record_sec;  /* 延時秒數 */
} rec_state_config_t;

/* ─── 不透明型別 ──────────────────────────────────────────────────── */
typedef struct rec_state_machine rec_state_machine_t;

/*
 * rec_state_create()：
 *   建立狀態機。初始狀態為 IDLE。
 *   buf：rec_buf_t 指標，供 EXTRACT_PRE 使用；不得為 NULL。
 *   writer_eventfd：Writer thread eventfd；-1 表示 Phase R2 無寫入模式。
 *   on_change / cb_ctx：每次狀態轉移時的回呼（可為 NULL）。
 */
rec_state_machine_t *rec_state_create(const rec_state_config_t *cfg,
                                       rec_buf_t                *buf,
                                       int                       writer_eventfd,
                                       rec_state_cb_t            on_change,
                                       void                     *cb_ctx);

/*
 * rec_state_destroy()：釋放狀態機，*sm = NULL。
 */
void rec_state_destroy(rec_state_machine_t **sm);

/* ─── 事件輸入函式 ────────────────────────────────────────────────── */

/*
 * rec_state_on_trigger()：處理 AI 觸發事件。
 *   now_ns：CLOCK_MONOTONIC ns，用於消抖判斷。
 *   now_wall：time(NULL)，用於排程查詢；傳 0 內部自動呼叫 time(NULL)。
 */
void rec_state_on_trigger(rec_state_machine_t *sm,
                           rec_trigger_type_t   type,
                           uint64_t             now_ns,
                           time_t               now_wall);

/*
 * rec_state_on_timer()：處理 timerfd 到期事件。
 *   expirations：從 timerfd read 到的到期計數（uint64_t）。
 *   now_wall：time(NULL)；傳 0 內部自動呼叫 time(NULL)。
 *
 * 此函式負責：
 *   - POST_WAIT 倒數（每秒扣 expirations 秒）
 *   - 排程格變更偵測與套用
 */
void rec_state_on_timer(rec_state_machine_t *sm,
                        uint64_t             expirations,
                        bool                 pending_trigger,
                        time_t               now_wall);

/*
 * rec_state_on_keyframe()：通知狀態機收到 IDR 幀。
 *   僅在 WAIT_KEYFRAME 狀態有效，其他狀態為 no-op。
 *   回傳 true = 發生了 WAIT_KEYFRAME → IN_EVENT 轉移。
 */
bool rec_state_on_keyframe(rec_state_machine_t *sm);

/*
 * rec_state_force_idle()：強制轉移至 IDLE。
 *   用於：排程關閉（schedule off）、手動停止、destroy 前清理、
 *         rec_buf_push() 超時（abort pre-roll）。
 */
void rec_state_force_idle(rec_state_machine_t *sm);

/*
 * rec_state_set_schedule()：在執行期更新排程（不影響 current_slot 快照）。
 *   下一次 rec_state_on_timer() 會偵測到格值變更並套用。
 */
void rec_state_set_schedule(rec_state_machine_t   *sm,
                              const rec_schedule_t  *sched);

/* ─── 存取函式 ────────────────────────────────────────────────────── */
rec_state_t rec_state_get(const rec_state_machine_t *sm);
int         rec_state_get_post_remaining(const rec_state_machine_t *sm);

#endif /* REC_STATE_H */
