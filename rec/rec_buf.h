/* rec/rec_buf.h — Circular Byte Ring + Frame Index Table + pre_extract_queue */
#ifndef REC_BUF_H
#define REC_BUF_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "rec_defs.h"

/* ─── 幀索引項 ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t offset;        /* ring 內的起始 byte offset */
    uint32_t size;          /* 幀資料大小（bytes）*/
    uint64_t timestamp_ns;
    uint64_t seq_num;
    uint32_t batch_gen;     /* 0 = 非 pre-roll；>0 = 所屬 pre-roll batch */
    bool     is_keyframe;
} rec_frame_entry_t;

/* ─── pre_extract_queue（SPSC：event loop 寫，Writer 讀）─────────── */
typedef struct {
    rec_frame_entry_t  entries[REC_PRE_QUEUE_DEPTH];
    _Atomic uint32_t   head;   /* Writer 讀取位置（monotonic）*/
    _Atomic uint32_t   tail;   /* event loop 寫入位置（monotonic）*/
} rec_pre_queue_t;

/* ─── Circular Byte Ring + Frame Index Table ─────────────────────── */
typedef struct rec_buf {
    uint8_t           *ring;
    uint32_t           ring_size;
    uint32_t           write_pos;   /* 下一個寫入的 byte offset */

    rec_frame_entry_t  index[REC_FRAME_INDEX_MAX];
    uint32_t           index_head;   /* 最舊有效 entry 的 slot index（mod MAX）*/
    uint32_t           index_tail;   /* 下一個空槽的 slot index（mod MAX）*/
    uint32_t           index_count;  /* 目前有效 entry 數量 */

    /*
     * protect window：Writer 讀 pre-roll 時，event loop 不得覆蓋此 byte 區間。
     * protected_gen 綁定擁有者 batch；只有相同 gen 的 Writer 才能清除保護。
     */
    _Atomic uint32_t   protected_read_offset;  /* 保護區間起點；REC_PROTECT_NONE = 無效 */
    _Atomic uint32_t   protected_read_size;    /* 保護區間大小；0 = 無效 */
    _Atomic uint32_t   protected_gen;          /* REC_PRE_GEN_NONE = 無保護 */

    /*
     * next_pre_gen：每次 EXTRACT_PRE 產生新 batch id。
     * aborted_pre_gen：timeout / 手動取消時標記被放棄的 batch。
     * Writer 在開始 drain 一個 batch 時取 snapshot，整個 batch 使用 snapshot 判斷。
     */
    _Atomic uint32_t   next_pre_gen;
    _Atomic uint32_t   aborted_pre_gen;   /* REC_PRE_GEN_NONE = 無 abort */

    rec_pre_queue_t    pre_queue;
} rec_buf_t;

/* ─── 公開 API ───────────────────────────────────────────────────── */

/*
 * rec_buf_create() / rec_buf_destroy()
 */
rec_buf_t *rec_buf_create(uint32_t ring_size);
void       rec_buf_destroy(rec_buf_t **buf);

/*
 * rec_buf_push()：將一幀 encoded data 寫入 ring。
 *   - 若新幀與 protect window 重疊，自旋等待最多 REC_PROTECT_SPIN_TIMEOUT_US。
 *   - 超時則 abort pre-roll，回傳 REC_ERR_WRITER_STUCK。
 *   - 自動淘汰 index 中被新幀覆蓋的舊幀。
 * 回傳：REC_OK = 成功；REC_ERR_WRITER_STUCK = 超時
 */
int rec_buf_push(rec_buf_t *buf, const uint8_t *data, uint32_t size,
                 uint64_t ts_ns, uint64_t seq_num, bool is_keyframe);

/*
 * rec_buf_extract_from_keyframe()：
 *   掃描 index，從 target_ns 之前最近的 keyframe 開始，
 *   建立 protect window 並將 index entries 複製進 pre_queue。
 *   呼叫者在此函式返回後需喚醒 Writer（write writer_eventfd）。
 *
 * 回傳：
 *   REC_OK               = 成功，*out_pre_gen 填入本次 batch gen
 *   REC_NEED_WAIT_KEYFRAME = buffer 內完全沒有 keyframe，需進入 WAIT_KEYFRAME
 *   < 0                  = 錯誤（-EINVAL / -ENOSPC）
 */
int rec_buf_extract_from_keyframe(rec_buf_t *buf, uint64_t target_ns,
                                  uint32_t *out_pre_gen);

/*
 * rec_pre_queue_dequeue()：Writer thread 取出一個 pre_queue entry。
 * 回傳：true = 成功；false = queue 為空
 */
bool rec_pre_queue_dequeue(rec_pre_queue_t *q, rec_frame_entry_t *out);
void rec_buf_abort_pre_roll(rec_buf_t *buf, uint32_t pre_gen);

/*
 * rec_buf_clear_protect()：Writer thread 在 drain 完同一 batch 後呼叫，
 * 清除 protect window。只有擁有者 batch（pre_gen 匹配）才能清除。
 */
void rec_buf_clear_protect(rec_buf_t *buf, uint32_t pre_gen);

/*
 * rec_buf_overlaps()：完整環形區間重疊判斷（兩段線性展開，2×2 比較）。
 * 開放給 test 直接呼叫。
 */
bool rec_buf_overlaps(uint32_t a_off, uint32_t a_size,
                      uint32_t b_off, uint32_t b_size,
                      uint32_t ring_size);

/* ─── 診斷用存取 ─────────────────────────────────────────────────── */
static inline uint32_t rec_buf_index_count(const rec_buf_t *buf) {
    return buf->index_count;
}

static inline uint32_t rec_buf_write_pos(const rec_buf_t *buf) {
    return buf->write_pos;
}

/*
 * rec_buf_get_entry()：取得 index 中的第 i 個有效 entry（0 = 最舊）。
 * 若 i >= index_count 則回傳 false。
 */
bool rec_buf_get_entry(const rec_buf_t *buf, uint32_t i, rec_frame_entry_t *out);

#endif /* REC_BUF_H */
