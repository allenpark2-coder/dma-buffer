/* rec/rec_defs.h — Recording Engine 常數、型別、錯誤碼（唯一常數定義點）
 * 所有 rec/ 模組以此為第一個 include。
 */
#ifndef REC_DEFS_H
#define REC_DEFS_H

#include "vfr_defs.h"

/* ─── Buffer 配置 ────────────────────────────────────────────────── */
#define REC_BUF_SIZE_DEFAULT   (15 * 1024 * 1024)  /* 15 MB，4K H265 約 10s */
#define REC_BUF_SIZE_MIN       (1  * 1024 * 1024)  /* 1 MB 下限 */
#define REC_BUF_SIZE_MAX       (64 * 1024 * 1024)  /* 64 MB 上限 */
#define REC_FRAME_INDEX_MAX    4096                 /* 最多追蹤 4096 幀 */

/* ─── 排程 ───────────────────────────────────────────────────────── */
#define REC_SCHEDULE_SLOTS_PER_DAY  96   /* 24h × 4（每 15 分鐘一格）*/

/* ─── 分段 ───────────────────────────────────────────────────────── */
#define REC_SEGMENT_DURATION_SEC  600    /* 預設每 10 分鐘分段 */
#define REC_SEGMENT_SIZE_MAX      (2LL * 1024 * 1024 * 1024)  /* 2 GB */

/* ─── 延時 ───────────────────────────────────────────────────────── */
#define REC_POST_RECORD_SEC_DEFAULT  30  /* 觸發消失後繼續錄影 30 秒 */
#define REC_DEBOUNCE_MS              500 /* 500 ms 內重複觸發視為同一事件 */

/* ─── epoll fd 上限 ──────────────────────────────────────────────── */
#define REC_MAX_EPOLL_FDS  8

/* ─── pre_extract_queue 容量 ─────────────────────────────────────── */
#define REC_PRE_QUEUE_DEPTH  REC_FRAME_INDEX_MAX

/* ─── write queue 容量 ───────────────────────────────────────────── */
#define REC_WRITE_QUEUE_DEPTH  256

/* ─── wrap-around 保護自旋逾時 ──────────────────────────────────── */
#define REC_PROTECT_SPIN_TIMEOUT_US  100000   /* 100 ms */

/* ─── ring 保護無效哨兵值 ────────────────────────────────────────── */
#define REC_PROTECT_NONE  UINT32_MAX
#define REC_PRE_GEN_NONE  0u

/* ─── stream_id accessor（stride 欄位語意重用）────────────────────── */
#define REC_FRAME_STREAM_ID(frame)         ((uint32_t)(frame)->stride)
#define REC_FRAME_SET_STREAM_ID(frame, id) ((frame)->stride = (uint32_t)(id))

/* ─── 錄影模式 ───────────────────────────────────────────────────── */
typedef enum {
    REC_MODE_CONTINUOUS = 0,
    REC_MODE_SCHEDULED  = 1,
    REC_MODE_EVENT      = 2,
} rec_mode_t;

/* ─── 狀態機狀態 ─────────────────────────────────────────────────── */
typedef enum {
    REC_STATE_IDLE          = 0,
    REC_STATE_EXTRACT_PRE   = 1,
    REC_STATE_WAIT_KEYFRAME = 2,
    REC_STATE_IN_EVENT      = 3,
    REC_STATE_POST_WAIT     = 4,
} rec_state_t;

/* ─── 排程格 ─────────────────────────────────────────────────────── */
typedef enum {
    REC_SLOT_OFF        = 0,
    REC_SLOT_CONTINUOUS = 1,
    REC_SLOT_EVENT      = 2,
} rec_slot_mode_t;

/* ─── 觸發事件 ───────────────────────────────────────────────────── */
typedef enum {
    REC_TRIGGER_START = 1,
    REC_TRIGGER_STOP  = 2,
} rec_trigger_type_t;

/* ─── Recorder 內部錯誤碼 ────────────────────────────────────────── */
typedef enum {
    REC_OK = 0,
    REC_ERR_WRITER_STUCK = -1001,
    REC_ERR_PRE_ABORTED  = -1002,
    REC_ERR_QUEUE_FULL   = -1003,
} rec_error_t;

/* ─── EXTRACT_PRE 回傳語意 ───────────────────────────────────────── */
/* rec_buf_extract_from_keyframe() 回傳 > 0 表示需進入 WAIT_KEYFRAME */
#define REC_NEED_WAIT_KEYFRAME  1

#endif /* REC_DEFS_H */
