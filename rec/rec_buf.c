/* rec/rec_buf.c — Circular Byte Ring + Frame Index Table + pre_extract_queue */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include "rec_buf.h"

/* ─── 內部 span 型別（用於環形區間展開）────────────────────────────── */
typedef struct {
    uint32_t off;
    uint32_t size;
} rec_span_t;

/* ─── 輔助：將環形區間展開成最多 2 段線性區間 ─────────────────────── */
static int rec_buf_split_span(uint32_t off, uint32_t size,
                              uint32_t ring_size,
                              rec_span_t spans[2])
{
    if (size == 0)
        return 0;

    uint32_t end = off + size;
    if (end <= ring_size) {
        spans[0].off  = off;
        spans[0].size = size;
        return 1;
    }

    spans[0].off  = off;
    spans[0].size = ring_size - off;
    spans[1].off  = 0;
    spans[1].size = end - ring_size;
    return 2;
}

/* ─── 輔助：兩段線性區間是否重疊 ────────────────────────────────────── */
static bool rec_linear_overlap(rec_span_t a, rec_span_t b)
{
    return (a.off < b.off + b.size) && (b.off < a.off + a.size);
}

/* ─── 公開：完整環形區間重疊判斷（雙方都展開，2×2 比較）────────────── */
bool rec_buf_overlaps(uint32_t a_off, uint32_t a_size,
                      uint32_t b_off, uint32_t b_size,
                      uint32_t ring_size)
{
    rec_span_t a_spans[2], b_spans[2];
    int a_n = rec_buf_split_span(a_off, a_size, ring_size, a_spans);
    int b_n = rec_buf_split_span(b_off, b_size, ring_size, b_spans);

    for (int i = 0; i < a_n; ++i)
        for (int j = 0; j < b_n; ++j)
            if (rec_linear_overlap(a_spans[i], b_spans[j]))
                return true;
    return false;
}

/* ─── 輔助：取得單調時鐘微秒數 ─────────────────────────────────────── */
static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
}

/* ─── rec_buf_create ─────────────────────────────────────────────── */
rec_buf_t *rec_buf_create(uint32_t ring_size)
{
    if (ring_size < REC_BUF_SIZE_MIN || ring_size > REC_BUF_SIZE_MAX)
        return NULL;

    rec_buf_t *buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    buf->ring = malloc(ring_size);
    if (!buf->ring) {
        free(buf);
        return NULL;
    }

    buf->ring_size  = ring_size;
    buf->write_pos  = 0;
    buf->index_head = 0;
    buf->index_tail = 0;
    buf->index_count = 0;

    atomic_store_explicit(&buf->protected_read_offset, REC_PROTECT_NONE, memory_order_relaxed);
    atomic_store_explicit(&buf->protected_read_size,   0,                memory_order_relaxed);
    atomic_store_explicit(&buf->protected_gen,         REC_PRE_GEN_NONE, memory_order_relaxed);
    atomic_store_explicit(&buf->next_pre_gen,          0,                memory_order_relaxed);
    atomic_store_explicit(&buf->aborted_pre_gen,       REC_PRE_GEN_NONE, memory_order_relaxed);
    atomic_store_explicit(&buf->pre_queue.head,        0,                memory_order_relaxed);
    atomic_store_explicit(&buf->pre_queue.tail,        0,                memory_order_relaxed);

    return buf;
}

/* ─── rec_buf_destroy ────────────────────────────────────────────── */
void rec_buf_destroy(rec_buf_t **buf)
{
    if (!buf || !*buf)
        return;
    free((*buf)->ring);
    free(*buf);
    *buf = NULL;
}

/* ─── rec_buf_push ───────────────────────────────────────────────── */
int rec_buf_push(rec_buf_t *buf, const uint8_t *data, uint32_t size,
                 uint64_t ts_ns, uint64_t seq_num, bool is_keyframe)
{
    if (!buf || !data || size == 0 || size > buf->ring_size)
        return -EINVAL;

    /* ── Step 1：若與 protect window 重疊，自旋等待 ──────────────── */
    int64_t deadline = now_us() + REC_PROTECT_SPIN_TIMEOUT_US;

    for (;;) {
        uint32_t prot_gen  = atomic_load_explicit(&buf->protected_gen,
                                                   memory_order_acquire);
        uint32_t prot_off  = atomic_load_explicit(&buf->protected_read_offset,
                                                   memory_order_acquire);
        uint32_t prot_size = atomic_load_explicit(&buf->protected_read_size,
                                                   memory_order_acquire);

        /* 無保護：直接繼續 */
        if (prot_gen == REC_PRE_GEN_NONE || prot_size == 0)
            break;

        /* 無重疊：直接繼續 */
        if (!rec_buf_overlaps(buf->write_pos, size,
                              prot_off, prot_size, buf->ring_size))
            break;

        /* 超時：abort pre-roll */
        if (now_us() >= deadline) {
            /* 先記 aborted_pre_gen，再清 protect（順序重要）*/
            atomic_store_explicit(&buf->aborted_pre_gen, prot_gen,
                                  memory_order_release);
            atomic_store_explicit(&buf->protected_gen,
                                  REC_PRE_GEN_NONE, memory_order_release);
            atomic_store_explicit(&buf->protected_read_offset,
                                  REC_PROTECT_NONE, memory_order_release);
            atomic_store_explicit(&buf->protected_read_size,
                                  0, memory_order_release);
            return REC_ERR_WRITER_STUCK;
        }

        sched_yield();
    }

    /* ── Step 2：淘汰被新幀覆蓋的舊 index entry ─────────────────── */
    while (buf->index_count > 0) {
        uint32_t h = buf->index_head;
        if (!rec_buf_overlaps(buf->write_pos, size,
                              buf->index[h].offset, buf->index[h].size,
                              buf->ring_size))
            break;
        buf->index_head  = (buf->index_head + 1) % REC_FRAME_INDEX_MAX;
        buf->index_count--;
    }

    /* ── Step 3：若 index 已滿（小幀充滿 ring 但未觸發位元重疊），
     *            淘汰最舊 entry 騰出一格 ─────────────────────────── */
    if (buf->index_count >= REC_FRAME_INDEX_MAX) {
        buf->index_head  = (buf->index_head + 1) % REC_FRAME_INDEX_MAX;
        buf->index_count--;
    }

    /* ── Step 4：寫入 ring（支援 wrap-around）────────────────────── */
    uint32_t wp = buf->write_pos;
    if (wp + size <= buf->ring_size) {
        memcpy(buf->ring + wp, data, size);
    } else {
        uint32_t first = buf->ring_size - wp;
        memcpy(buf->ring + wp, data, first);
        memcpy(buf->ring,      data + first, size - first);
    }

    /* ── Step 5：新增 index entry ────────────────────────────────── */
    uint32_t t = buf->index_tail;
    buf->index[t].offset       = wp;
    buf->index[t].size         = size;
    buf->index[t].timestamp_ns = ts_ns;
    buf->index[t].seq_num      = seq_num;
    buf->index[t].batch_gen    = 0;
    buf->index[t].is_keyframe  = is_keyframe;
    buf->index_tail  = (buf->index_tail + 1) % REC_FRAME_INDEX_MAX;
    buf->index_count++;

    /* ── Step 6：推進 write_pos ──────────────────────────────────── */
    buf->write_pos = (wp + size) % buf->ring_size;

    return REC_OK;
}

/* ─── rec_buf_extract_from_keyframe ─────────────────────────────── */
int rec_buf_extract_from_keyframe(rec_buf_t *buf, uint64_t target_ns,
                                  uint32_t *out_pre_gen)
{
    if (!buf)
        return -EINVAL;

    /* ── Step 1：掃描 index，找起始 keyframe ─────────────────────── */
    int start_i = -1;          /* 相對於 index_head 的偏移 */
    int fallback_i = -1;       /* 最老的 keyframe（若無 >= target_ns 的）*/

    for (uint32_t i = 0; i < buf->index_count; i++) {
        uint32_t slot = (buf->index_head + i) % REC_FRAME_INDEX_MAX;
        if (!buf->index[slot].is_keyframe)
            continue;
        if (fallback_i == -1)
            fallback_i = (int)i;
        if (buf->index[slot].timestamp_ns >= target_ns) {
            start_i = (int)i;
            break;
        }
    }

    if (start_i == -1) {
        if (fallback_i != -1)
            start_i = fallback_i;    /* 回退到最老的 keyframe */
        else
            return REC_NEED_WAIT_KEYFRAME;  /* 完全沒有 keyframe */
    }

    /* ── Step 2：分配新的 pre-roll generation ────────────────────── */
    uint32_t pre_gen = atomic_fetch_add_explicit(&buf->next_pre_gen, 1,
                                                  memory_order_relaxed) + 1;
    if (pre_gen == REC_PRE_GEN_NONE)
        pre_gen = 1;  /* 極端 wrap-around 保護 */

    /* 清除上一次遺留的 abort 標記 */
    atomic_store_explicit(&buf->aborted_pre_gen,
                          REC_PRE_GEN_NONE, memory_order_release);

    /* ── Step 3：計算 protect window ─────────────────────────────── */
    uint32_t start_slot  = (buf->index_head + (uint32_t)start_i) % REC_FRAME_INDEX_MAX;
    uint32_t prot_start  = buf->index[start_slot].offset;
    uint32_t wp          = buf->write_pos;
    uint32_t prot_size;

    if (wp >= prot_start)
        prot_size = wp - prot_start;
    else
        prot_size = buf->ring_size - prot_start + wp;

    /* ── Step 4：設定保護（size → offset → gen，均 release store）── */
    atomic_store_explicit(&buf->protected_read_size,   prot_size,
                          memory_order_release);
    atomic_store_explicit(&buf->protected_read_offset, prot_start,
                          memory_order_release);
    atomic_store_explicit(&buf->protected_gen,         pre_gen,
                          memory_order_release);

    /* ── Step 5：將 [start_i .. index_count) 的 entry 複製進 pre_queue ── */
    rec_pre_queue_t *q = &buf->pre_queue;

    for (uint32_t i = (uint32_t)start_i; i < buf->index_count; i++) {
        uint32_t slot = (buf->index_head + i) % REC_FRAME_INDEX_MAX;

        uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);

        if (tail - head >= REC_PRE_QUEUE_DEPTH) {
            /* pre_queue 滿（不應發生，但做防禦性 abort）*/
            atomic_store_explicit(&buf->aborted_pre_gen, pre_gen,
                                  memory_order_release);
            atomic_store_explicit(&buf->protected_gen,
                                  REC_PRE_GEN_NONE, memory_order_release);
            atomic_store_explicit(&buf->protected_read_offset,
                                  REC_PROTECT_NONE, memory_order_release);
            atomic_store_explicit(&buf->protected_read_size,
                                  0, memory_order_release);
            return -ENOSPC;
        }

        rec_frame_entry_t tmp  = buf->index[slot];
        tmp.batch_gen          = pre_gen;
        q->entries[tail % REC_PRE_QUEUE_DEPTH] = tmp;
        atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    }

    if (out_pre_gen)
        *out_pre_gen = pre_gen;

    /* 呼叫者需在此函式返回後 write(writer_eventfd, 1) 喚醒 Writer */
    return REC_OK;
}

/* ─── rec_pre_queue_dequeue ──────────────────────────────────────── */
bool rec_pre_queue_dequeue(rec_pre_queue_t *q, rec_frame_entry_t *out)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head == tail)
        return false;  /* empty */

    *out = q->entries[head % REC_PRE_QUEUE_DEPTH];
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}

void rec_buf_abort_pre_roll(rec_buf_t *buf, uint32_t pre_gen)
{
    if (!buf || pre_gen == REC_PRE_GEN_NONE)
        return;

    uint32_t current = atomic_load_explicit(&buf->protected_gen,
                                            memory_order_acquire);
    if (current != pre_gen)
        return;

    atomic_store_explicit(&buf->aborted_pre_gen, pre_gen,
                          memory_order_release);
    atomic_store_explicit(&buf->protected_gen,
                          REC_PRE_GEN_NONE, memory_order_release);
    atomic_store_explicit(&buf->protected_read_offset,
                          REC_PROTECT_NONE, memory_order_release);
    atomic_store_explicit(&buf->protected_read_size,
                          0, memory_order_release);
}

/* ─── rec_buf_clear_protect ──────────────────────────────────────── */
void rec_buf_clear_protect(rec_buf_t *buf, uint32_t pre_gen)
{
    /* 只有擁有者 batch 才能清除保護 */
    uint32_t current = atomic_load_explicit(&buf->protected_gen,
                                            memory_order_acquire);
    if (current != pre_gen)
        return;

    atomic_store_explicit(&buf->protected_gen,
                          REC_PRE_GEN_NONE, memory_order_release);
    atomic_store_explicit(&buf->protected_read_offset,
                          REC_PROTECT_NONE, memory_order_release);
    atomic_store_explicit(&buf->protected_read_size,
                          0, memory_order_release);
}

/* ─── rec_buf_get_entry ──────────────────────────────────────────── */
bool rec_buf_get_entry(const rec_buf_t *buf, uint32_t i, rec_frame_entry_t *out)
{
    if (!buf || !out || i >= buf->index_count)
        return false;
    uint32_t slot = (buf->index_head + i) % REC_FRAME_INDEX_MAX;
    *out = buf->index[slot];
    return true;
}
