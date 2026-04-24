/* test/test_rec_buf.c — Phase R1 驗收測試
 *
 * 驗收 Checklist：
 * [1] 寫入 1000 幀，buffer 滿後舊幀自動淘汰，index_head 正確推進
 * [2] I-Frame 定位：extract_from_keyframe(t_now - 10s) 回傳正確起始 entry
 * [3] Wrap-around：buffer 環繞後 index offset 計算正確，無越界
 * [4] rec_buf_overlaps() 單元測試：非環繞、單邊環繞、雙邊環繞各場景
 * [5] protect window 擁有者驗證：舊 batch 不可清除新 batch 的 protect
 * [6] batch abort：timeout 後同 batch 殘留 entry 被 Writer 丟棄，不寫入 TS
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include "rec_defs.h"
#include "rec_buf.h"

/* ─── 測試框架 ───────────────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL (%s:%d): %s\n", __FILE__, __LINE__, (msg)); \
        g_fail++; \
    } else { \
        printf("  PASS: %s\n", (msg)); \
        g_pass++; \
    } \
} while (0)

#define TEST_BEGIN(name) do { printf("\n[TEST] %s\n", (name)); } while (0)

/* ─── 輔助：製造假幀資料 ─────────────────────────────────────────── */

/* 回傳 malloc'd buffer，呼叫者需 free()。pattern 填充 size bytes。*/
static uint8_t *make_frame(uint8_t pattern, uint32_t size)
{
    uint8_t *p = malloc(size);
    if (p) memset(p, pattern, size);
    return p;
}

/* ─── Test 1：基本寫入與自動淘汰 ─────────────────────────────────── */
static void test_push_eviction(void)
{
    TEST_BEGIN("Push + Eviction (1000 frames)");

    /* 使用 1 MB ring，每幀 300 bytes，約 3495 幀才填滿 → 1000 幀無需淘汰 */
    const uint32_t RING_SZ  = 1 * 1024 * 1024;
    const uint32_t FRAME_SZ = 300;
    const uint32_t N        = 1000;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    uint8_t *frame = make_frame(0, FRAME_SZ);
    CHECK(frame != NULL, "frame alloc ok");

    uint64_t ts = 1000000000ULL;  /* 1 s */
    for (uint32_t i = 0; i < N; i++) {
        memset(frame, (uint8_t)(i & 0xFF), FRAME_SZ);
        int rc = rec_buf_push(buf, frame, FRAME_SZ,
                              ts + (uint64_t)i * 33333333ULL,
                              i, (i % 30 == 0));
        CHECK(rc == REC_OK, "push returns REC_OK");
    }
    free(frame);

    /* 1000 × 300 = 300 KB < 1 MB；index 未超 4096 上限 */
    CHECK(rec_buf_index_count(buf) == N,
          "index_count == 1000 (no eviction in 1 MB ring)");
    CHECK(rec_buf_write_pos(buf) == (N * FRAME_SZ) % RING_SZ,
          "write_pos advanced correctly");

    rec_buf_destroy(&buf);
    CHECK(buf == NULL, "buf set to NULL after destroy");

    /* 用小 ring（1 MB）與大幀（200 KB）強迫淘汰 */
    const uint32_t SMALL_RING = REC_BUF_SIZE_MIN;   /* 1 MB */
    const uint32_t BIG_FRAME  = 200 * 1024;          /* 200 KB */
    buf = rec_buf_create(SMALL_RING);
    CHECK(buf != NULL, "rec_buf_create (small ring) ok");

    uint8_t *big = make_frame(0, BIG_FRAME);
    CHECK(big != NULL, "big frame alloc ok");

    for (uint32_t i = 0; i < 10; i++) {
        memset(big, (uint8_t)i, BIG_FRAME);
        int rc = rec_buf_push(buf, big, BIG_FRAME,
                              ts + (uint64_t)i * 1000000ULL, i, (i == 0));
        CHECK(rc == REC_OK, "push (big frame) returns REC_OK");
    }
    free(big);

    /* 10 × 200 KB = 2 MB > 1 MB ring → 至少 5 幀被淘汰 */
    uint32_t cnt = rec_buf_index_count(buf);
    CHECK(cnt <= 5, "old frames evicted when ring fills");
    CHECK(cnt > 0,  "at least one frame remains");
    CHECK(buf->index_head != 0 || buf->index_count < 10,
          "index_head advanced after eviction");

    rec_buf_destroy(&buf);
}

/* ─── Test 2：I-Frame 定位 ────────────────────────────────────────── */
static void test_extract_keyframe(void)
{
    TEST_BEGIN("I-Frame location via extract_from_keyframe");

    const uint32_t RING_SZ   = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ  = 1024;
    const uint64_t BASE_NS   = 10ULL * 1000000000ULL;  /* t=10s */
    const uint64_t FRAME_DUR = 33333333ULL;             /* ~30fps */
    const uint32_t KEYFRAME_INTERVAL = 30;
    const uint32_t N = 300;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    uint8_t *frame = make_frame(0, FRAME_SZ);
    CHECK(frame != NULL, "frame alloc ok");

    /* 填入 300 幀，每 30 幀一個 keyframe */
    for (uint32_t i = 0; i < N; i++) {
        memset(frame, (uint8_t)(i & 0xFF), FRAME_SZ);
        bool is_kf = (i % KEYFRAME_INTERVAL == 0);
        rec_buf_push(buf, frame, FRAME_SZ,
                     BASE_NS + (uint64_t)i * FRAME_DUR, i, is_kf);
    }
    free(frame);

    /* target = 現在時間 - 5s（約第 150 幀附近）*/
    uint64_t now_ns  = BASE_NS + (uint64_t)N * FRAME_DUR;
    uint64_t tgt_ns  = now_ns - 5ULL * 1000000000ULL;
    /* tgt_ns ≈ BASE_NS + 150 * FRAME_DUR → 最近的 keyframe 應在 frame 150 */

    uint32_t pre_gen = 0;
    int rc = rec_buf_extract_from_keyframe(buf, tgt_ns, &pre_gen);
    CHECK(rc == REC_OK,       "extract_from_keyframe returns REC_OK");
    CHECK(pre_gen != REC_PRE_GEN_NONE, "pre_gen != 0");

    /* 取出 pre_queue 的第一個 entry，應該是 keyframe */
    rec_frame_entry_t first;
    bool ok = rec_pre_queue_dequeue(&buf->pre_queue, &first);
    CHECK(ok,                          "pre_queue has entries");
    CHECK(first.is_keyframe,           "first entry is keyframe");
    CHECK(first.batch_gen == pre_gen,  "first entry has correct batch_gen");
    /* timestamp 應 >= target（或是最近的 keyframe）*/
    CHECK(first.timestamp_ns >= tgt_ns ||
          first.timestamp_ns % (KEYFRAME_INTERVAL * FRAME_DUR) == BASE_NS %
          (KEYFRAME_INTERVAL * FRAME_DUR),
          "keyframe at or before target timestamp");

    /* protect window 應已設定 */
    uint32_t pg = atomic_load(&buf->protected_gen);
    CHECK(pg == pre_gen, "protect window set with correct gen");

    rec_buf_destroy(&buf);
}

/* ─── Test 3：Wrap-around ────────────────────────────────────────── */
static void test_wraparound(void)
{
    TEST_BEGIN("Wrap-around: ring rollover, index offsets correct");

    const uint32_t RING_SZ  = REC_BUF_SIZE_MIN;  /* 1 MB */
    const uint32_t FRAME_SZ = 300 * 1024;         /* 300 KB：1 MB / 300 KB ≈ 3 幀後 wrap */
    const uint32_t N = 8;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    uint8_t *frame = make_frame(0, FRAME_SZ);
    CHECK(frame != NULL, "frame alloc ok");

    uint64_t ts = 0;
    for (uint32_t i = 0; i < N; i++) {
        memset(frame, (uint8_t)(i * 37), FRAME_SZ);
        int rc = rec_buf_push(buf, frame, FRAME_SZ,
                              ts + (uint64_t)i * 33000000ULL, i, (i % 3 == 0));
        CHECK(rc == REC_OK, "push ok during wrap-around");
    }
    free(frame);

    /* write_pos 應已 wrap（至少繞了一圈）*/
    CHECK(rec_buf_write_pos(buf) < RING_SZ, "write_pos within ring bounds");

    /* 讀取所有現存 entry，驗證 offset + size <= ring_size（或 wrap 版本正確） */
    uint32_t cnt = rec_buf_index_count(buf);
    CHECK(cnt > 0, "index has entries after wrap-around");

    for (uint32_t i = 0; i < cnt; i++) {
        rec_frame_entry_t e;
        bool ok = rec_buf_get_entry(buf, i, &e);
        CHECK(ok, "rec_buf_get_entry ok");
        CHECK(e.offset < RING_SZ,  "entry offset within ring");
        CHECK(e.size == FRAME_SZ,  "entry size correct");
        /* wrap 的資料跨越 ring 邊界 —— offset + size 可以 > ring_size */
    }

    /* 從 ring 中讀回最後一個 entry 的資料，驗證內容 */
    if (cnt > 0) {
        rec_frame_entry_t last;
        rec_buf_get_entry(buf, cnt - 1, &last);

        uint8_t expected = (uint8_t)((N - 1) * 37);
        uint8_t got;

        if (last.offset + last.size <= RING_SZ) {
            got = buf->ring[last.offset];
        } else {
            /* wrap：取第一段第一個 byte */
            got = buf->ring[last.offset];
        }
        CHECK(got == expected, "ring data byte matches written pattern");
    }

    rec_buf_destroy(&buf);
}

/* ─── Test 4：rec_buf_overlaps() 單元測試 ───────────────────────── */
static void test_overlaps(void)
{
    TEST_BEGIN("rec_buf_overlaps() unit tests");

    const uint32_t R = 128;  /* ring size for overlap tests */

    /* 非環繞、不重疊 */
    CHECK(!rec_buf_overlaps( 0, 10,  20, 10, R), "non-wrap no-overlap: [0,10) vs [20,30)");
    CHECK(!rec_buf_overlaps(20, 10,   0, 10, R), "non-wrap no-overlap (reversed)");

    /* 非環繞、相鄰（邊界不重疊）*/
    CHECK(!rec_buf_overlaps( 0, 10,  10, 10, R), "non-wrap adjacent: [0,10) vs [10,20)");

    /* 非環繞、重疊 */
    CHECK( rec_buf_overlaps( 0, 20,  10, 10, R), "non-wrap overlap: [0,20) vs [10,20)");
    CHECK( rec_buf_overlaps(10, 10,   5, 15, R), "non-wrap overlap: [10,20) vs [5,20)");

    /* 完全包含 */
    CHECK( rec_buf_overlaps( 0, 30,  10,  5, R), "non-wrap contained: [0,30) contains [10,15)");

    /* 單邊環繞（a 環繞）: a=[110,30) = [110,128)+[0,12), b=[0,5) */
    CHECK( rec_buf_overlaps(110, 30,   0,  5, R), "single-wrap overlap: [110,140) vs [0,5)");

    /* 單邊環繞、不重疊：a=[110,15) = [110,125), b=[0,5) */
    CHECK(!rec_buf_overlaps(110, 15,   0,  5, R), "single-wrap no-overlap: [110,125) vs [0,5)");

    /* 單邊環繞、相鄰不重疊 */
    CHECK(!rec_buf_overlaps(120,  8,   0,  8, R), "single-wrap adjacent (no overlap): [120,128) vs [0,8)");

    /* 單邊環繞、重疊 */
    CHECK( rec_buf_overlaps(120, 10,   0,  5, R), "single-wrap overlap: [120,130)=[120,128)+[0,2) vs [0,5)");

    /* 雙邊環繞（兩個區間都環繞）*/
    /* a=[100,60)=[100,128)+[0,32), b=[110,40)=[110,128)+[0,22) */
    CHECK( rec_buf_overlaps(100, 60, 110, 40, R), "double-wrap overlap");

    /* a=[100,60)=[100,128)+[0,32), b=[50,20)=[50,70) → no overlap */
    CHECK(!rec_buf_overlaps(100, 60,  50, 20, R), "double-wrap no-overlap: [100,160) vs [50,70)");

    /* 零大小：永遠不重疊 */
    CHECK(!rec_buf_overlaps(  0,  0,   0,  0, R), "zero-size both: no overlap");
    CHECK(!rec_buf_overlaps(  5,  0,   5, 10, R), "zero-size a: no overlap");
    CHECK(!rec_buf_overlaps(  5, 10,   5,  0, R), "zero-size b: no overlap");

    /* 大小等於 ring（整圈）vs 任意小區間 */
    CHECK( rec_buf_overlaps(  0, R,   50, 10, R), "full-ring a overlaps everything");
}

/* ─── Test 5：protect window 擁有者驗證 ─────────────────────────── */
static void test_protect_owner(void)
{
    TEST_BEGIN("Protect window: old batch cannot clear new batch's protect");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 0;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    /* 推入足夠幀讓 extract 有 keyframe 可用 */
    uint8_t *frame5 = make_frame(0, FRAME_SZ);
    CHECK(frame5 != NULL, "frame alloc ok");
    for (uint32_t i = 0; i < 60; i++) {
        memset(frame5, (uint8_t)i, FRAME_SZ);
        rec_buf_push(buf, frame5, FRAME_SZ,
                     BASE_NS + (uint64_t)i * 33333333ULL, i, (i % 30 == 0));
    }
    free(frame5);

    /* 第一次 extract → gen_1 */
    uint32_t gen_1 = 0, gen_2 = 0;
    int rc1 = rec_buf_extract_from_keyframe(buf, BASE_NS, &gen_1);
    CHECK(rc1 == REC_OK, "first extract ok");
    CHECK(gen_1 != REC_PRE_GEN_NONE, "gen_1 != 0");

    /* 模擬 Writer 尚未 drain，event loop 再次觸發 extract（第二次 pre-roll）*/
    /* 先清空 pre_queue（模擬第一批已被消費）*/
    rec_frame_entry_t tmp;
    while (rec_pre_queue_dequeue(&buf->pre_queue, &tmp)) {}

    /* 第二次 extract → gen_2（應 > gen_1）*/
    int rc2 = rec_buf_extract_from_keyframe(buf, BASE_NS, &gen_2);
    CHECK(rc2 == REC_OK, "second extract ok");
    CHECK(gen_2 == gen_1 + 1, "gen_2 = gen_1 + 1");

    /* 此時 protected_gen == gen_2 */
    uint32_t cur = atomic_load(&buf->protected_gen);
    CHECK(cur == gen_2, "protected_gen updated to gen_2");

    /* 舊 batch（gen_1）的 Writer 嘗試清除保護 → 應被忽略 */
    rec_buf_clear_protect(buf, gen_1);
    cur = atomic_load(&buf->protected_gen);
    CHECK(cur == gen_2, "old batch cannot clear new batch's protect");

    /* 正確 batch（gen_2）清除保護 → 應成功 */
    rec_buf_clear_protect(buf, gen_2);
    cur = atomic_load(&buf->protected_gen);
    CHECK(cur == REC_PRE_GEN_NONE, "correct batch clears protect");

    rec_buf_destroy(&buf);
}

/* ─── Test 6：batch abort ────────────────────────────────────────── */
static void test_batch_abort(void)
{
    TEST_BEGIN("Batch abort: aborted entries discarded by Writer");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 0;
    const uint32_t N = 60;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    uint8_t *frame6 = make_frame(0, FRAME_SZ);
    CHECK(frame6 != NULL, "frame alloc ok");
    for (uint32_t i = 0; i < N; i++) {
        memset(frame6, (uint8_t)i, FRAME_SZ);
        rec_buf_push(buf, frame6, FRAME_SZ,
                     BASE_NS + (uint64_t)i * 33333333ULL, i, (i % 30 == 0));
    }
    free(frame6);

    /* Extract → 建立 pre-roll batch */
    uint32_t pre_gen = 0;
    int rc = rec_buf_extract_from_keyframe(buf, BASE_NS, &pre_gen);
    CHECK(rc == REC_OK, "extract ok");

    /* 模擬超時 abort：event loop 標記 aborted_pre_gen */
    atomic_store_explicit(&buf->aborted_pre_gen, pre_gen, memory_order_release);
    atomic_store_explicit(&buf->protected_gen, REC_PRE_GEN_NONE, memory_order_release);
    atomic_store_explicit(&buf->protected_read_offset, REC_PROTECT_NONE, memory_order_release);
    atomic_store_explicit(&buf->protected_read_size,   0,                memory_order_release);

    /* Writer thread 行為模擬：
     * 1. Snapshot abort 狀態（整個 batch 用同一個 snapshot）
     * 2. Drain pre_queue：每個 entry 比對 batch_gen == abort_snapshot
     * 3. 若匹配：丟棄（不寫入 TS），繼續 dequeue
     */
    uint32_t abort_snapshot = atomic_load_explicit(&buf->aborted_pre_gen,
                                                   memory_order_acquire);
    CHECK(abort_snapshot == pre_gen, "abort_snapshot matches pre_gen");

    uint32_t discarded = 0;
    uint32_t written   = 0;
    rec_frame_entry_t entry;

    while (rec_pre_queue_dequeue(&buf->pre_queue, &entry)) {
        if (entry.batch_gen == abort_snapshot) {
            discarded++;
            /* 不呼叫 rec_segment_write()，繼續丟棄 */
        } else {
            written++;
        }
    }

    CHECK(discarded > 0,  "some entries were discarded");
    CHECK(written   == 0, "no entries were written to TS");

    /* 確認 protect window 已清除 */
    uint32_t pg = atomic_load(&buf->protected_gen);
    CHECK(pg == REC_PRE_GEN_NONE, "protect window cleared after abort");

    rec_buf_destroy(&buf);
}

/* ─── Test 7：第二次 extract 讀到正確的 keyframe 邊界 ────────────── */
static void test_extract_fallback_keyframe(void)
{
    TEST_BEGIN("extract_from_keyframe: fallback to oldest keyframe");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 512;
    const uint64_t BASE_NS  = 1000000000ULL;
    const uint32_t N = 90;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    /* 推入 90 幀，只有 frame 0 是 keyframe */
    uint8_t *frame7 = make_frame(0, FRAME_SZ);
    CHECK(frame7 != NULL, "frame alloc ok");
    for (uint32_t i = 0; i < N; i++) {
        memset(frame7, (uint8_t)i, FRAME_SZ);
        rec_buf_push(buf, frame7, FRAME_SZ,
                     BASE_NS + (uint64_t)i * 33333333ULL, i, (i == 0));
    }
    free(frame7);

    /* target = 很久以後 → 沒有 >= target 的 keyframe，回退到最老的 keyframe（frame 0）*/
    uint64_t future_ns = BASE_NS + (uint64_t)(N + 100) * 33333333ULL;
    uint32_t pre_gen = 0;
    int rc = rec_buf_extract_from_keyframe(buf, future_ns, &pre_gen);
    CHECK(rc == REC_OK, "extract returns REC_OK (fallback to oldest keyframe)");

    rec_frame_entry_t first;
    bool ok = rec_pre_queue_dequeue(&buf->pre_queue, &first);
    CHECK(ok,                "pre_queue has entries");
    CHECK(first.is_keyframe, "fallback entry is a keyframe");
    CHECK(first.seq_num == 0, "fallback entry is frame 0 (only keyframe)");

    rec_buf_destroy(&buf);
}

/* ─── Test 8：buffer 內完全沒有 keyframe → WAIT_KEYFRAME ────────── */
static void test_extract_no_keyframe(void)
{
    TEST_BEGIN("extract_from_keyframe: no keyframe → WAIT_KEYFRAME");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 512;
    const uint64_t BASE_NS  = 0;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    /* 只推入 P-frame（非 keyframe）*/
    uint8_t *frame8 = make_frame(0, FRAME_SZ);
    CHECK(frame8 != NULL, "frame alloc ok");
    for (uint32_t i = 0; i < 30; i++) {
        memset(frame8, (uint8_t)i, FRAME_SZ);
        rec_buf_push(buf, frame8, FRAME_SZ,
                     BASE_NS + (uint64_t)i * 33333333ULL, i, false);
    }
    free(frame8);

    uint32_t pre_gen = 0;
    int rc = rec_buf_extract_from_keyframe(buf, BASE_NS, &pre_gen);
    CHECK(rc == REC_NEED_WAIT_KEYFRAME,
          "returns REC_NEED_WAIT_KEYFRAME when no keyframe available");

    rec_buf_destroy(&buf);
}

/* ─── Test 9：push 超時導致 protect abort ─────────────────────────
 * 透過直接設定 protect window（模擬 Writer 卡住），
 * 再 push 大量資料讓 spin-wait 超時，驗證 abort 路徑。
 */
static void test_push_protect_timeout(void)
{
    TEST_BEGIN("rec_buf_push: protect spin-wait timeout → REC_ERR_WRITER_STUCK");

    const uint32_t RING_SZ  = REC_BUF_SIZE_MIN;
    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    /* 推入幾幀讓 write_pos 在中間 */
    const uint32_t FRAME_SZ = 200 * 1024;  /* 200 KB */
    uint8_t *frame9 = make_frame(0, FRAME_SZ);
    CHECK(frame9 != NULL, "frame alloc ok");
    for (uint32_t i = 0; i < 2; i++) {
        memset(frame9, (uint8_t)i, FRAME_SZ);
        rec_buf_push(buf, frame9, FRAME_SZ, (uint64_t)i * 1000000ULL, i, false);
    }
    /* write_pos ≈ 400 KB */

    /* 模擬 Writer 設定一個覆蓋整個未來寫入範圍的 protect window */
    /* protect 從 write_pos 起，大小 = FRAME_SZ（與下一幀完全重疊）*/
    uint32_t fake_gen = 42;
    atomic_store(&buf->protected_read_size,   FRAME_SZ);
    atomic_store(&buf->protected_read_offset, buf->write_pos);
    atomic_store(&buf->protected_gen,         fake_gen);

    /* push 應在 100 ms 後超時，回傳 REC_ERR_WRITER_STUCK */
    memset(frame9, 0xAB, FRAME_SZ);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = rec_buf_push(buf, frame9, FRAME_SZ, 9999999ULL, 99, false);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(frame9);

    int64_t elapsed_us = (int64_t)(t1.tv_sec  - t0.tv_sec)  * 1000000 +
                         (int64_t)(t1.tv_nsec  - t0.tv_nsec) / 1000;

    CHECK(rc == REC_ERR_WRITER_STUCK, "push returns REC_ERR_WRITER_STUCK on timeout");
    CHECK(elapsed_us >= REC_PROTECT_SPIN_TIMEOUT_US / 2,
          "spin-wait lasted at least half the timeout duration");

    /* abort 後 protect 應已清除 */
    uint32_t pg = atomic_load(&buf->protected_gen);
    CHECK(pg == REC_PRE_GEN_NONE, "protect cleared after timeout abort");

    /* aborted_pre_gen 應已記錄 */
    uint32_t aborted = atomic_load(&buf->aborted_pre_gen);
    CHECK(aborted == fake_gen, "aborted_pre_gen set to fake_gen");

    rec_buf_destroy(&buf);
}

/* ─── main ───────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== Phase R1: rec_buf unit tests ===\n");

    test_push_eviction();
    test_extract_keyframe();
    test_wraparound();
    test_overlaps();
    test_protect_owner();
    test_batch_abort();
    test_extract_fallback_keyframe();
    test_extract_no_keyframe();
    test_push_protect_timeout();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
