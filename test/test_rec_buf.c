/* test/test_rec_buf.c — Phase R1 Acceptance Tests
 * Standalone, no external test framework needed.
 * Build: gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -Irec -o test_rec_buf rec/rec_buf.c test/test_rec_buf.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "rec_buf.h"

/* ─── Forward declarations of internal symbols under test ────────── */
bool rec_buf_overlaps_pub(uint32_t a_off, uint32_t a_size,
                           uint32_t b_off, uint32_t b_size,
                           uint32_t ring_size);

/* ─── Test harness ───────────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;

#define PASS(name) do { printf("  PASS: %s\n", (name)); g_pass++; } while(0)
#define FAIL(name, msg) do { \
    printf("  FAIL: %s — %s (line %d)\n", (name), (msg), __LINE__); \
    g_fail++; \
} while(0)

#define CHECK(name, cond) do { \
    if (cond) PASS(name); \
    else       FAIL(name, #cond); \
} while(0)

/* ─── Test 1: Write 1000 frames, buffer fills, old entries evicted ─ */
static void test_eviction(void)
{
    printf("\n[Test 1] Eviction: 1000 frames, auto-evict, index_head advances\n");

    /* Use a small ring (1MB) so eviction happens quickly */
    uint32_t ring_sz = REC_BUF_SIZE_MIN;
    rec_buf_t *buf = rec_buf_create(ring_sz);
    CHECK("create", buf != NULL);
    if (!buf) return;

    /* 1000 frames, each 4096 bytes (1MB ring → ~256 frames fit) */
    const uint32_t FRAME_SZ   = 4096;
    const uint32_t NUM_FRAMES = 1000;
    uint8_t frame_data[FRAME_SZ];
    memset(frame_data, 0xAB, FRAME_SZ);

    uint32_t prev_index_head = buf->index_head;
    int evictions = 0;

    for (uint32_t i = 0; i < NUM_FRAMES; i++) {
        frame_data[0] = (uint8_t)(i & 0xFF);
        int ret = rec_buf_push(buf, frame_data, FRAME_SZ,
                                (uint64_t)i * 33000000ULL, i,
                                (i % 30 == 0));
        CHECK("push_ok", ret == REC_OK);

        if (buf->index_head != prev_index_head) {
            evictions++;
            prev_index_head = buf->index_head;
        }
    }

    CHECK("evictions_occurred",  evictions > 0);
    CHECK("index_count_bounded", buf->index_count <= REC_FRAME_INDEX_MAX);
    CHECK("index_count_positive", buf->index_count > 0);

    /* Verify that the oldest entry's offset is valid */
    rec_frame_entry_t *oldest = &buf->index[buf->index_head];
    CHECK("oldest_offset_in_range", oldest->offset < buf->ring_size);
    CHECK("oldest_size_correct",    oldest->size == FRAME_SZ);

    printf("  info: evictions=%d, final index_count=%u, index_head=%u\n",
           evictions, buf->index_count, buf->index_head);

    rec_buf_destroy(&buf);
    CHECK("destroy_null", buf == NULL);
}

/* ─── Test 2: I-Frame location ───────────────────────────────────── */
static void test_iframe_location(void)
{
    printf("\n[Test 2] I-Frame location: extract_from_keyframe(now-10s)\n");

    uint32_t ring_sz = REC_BUF_SIZE_MIN;
    rec_buf_t *buf = rec_buf_create(ring_sz);
    CHECK("create", buf != NULL);
    if (!buf) return;

    /*
     * Push 90 frames at 33ms intervals (~3 seconds of video).
     * I-frames at frame 0, 30, 60.
     * Timestamps: 0, 33ms, 66ms, ...
     */
    const uint32_t FRAME_SZ    = 8192;
    const uint32_t NUM_FRAMES  = 90;
    const uint64_t FRAME_NS    = 33000000ULL;  /* 33ms */
    uint8_t frame_data[FRAME_SZ];
    memset(frame_data, 0xCD, FRAME_SZ);

    uint64_t base_ts = 1000000000000ULL; /* 1000s epoch */

    for (uint32_t i = 0; i < NUM_FRAMES; i++) {
        bool kf = (i % 30 == 0);
        int ret = rec_buf_push(buf, frame_data, FRAME_SZ,
                                base_ts + (uint64_t)i * FRAME_NS,
                                i, kf);
        CHECK("push_ok", ret == REC_OK);
    }

    /*
     * now_ns = base_ts + 90 * FRAME_NS (just past last frame)
     * pre_sec = 10 → target = now_ns - 10s
     * With only 3s of content, target will be before base_ts,
     * so best_idx will be frame 0 (oldest keyframe) as fallback.
     */
    uint64_t now_ns = base_ts + (uint64_t)NUM_FRAMES * FRAME_NS;
    uint32_t batch_gen = 0;
    int count = rec_buf_extract_from_keyframe(buf, now_ns, 10, &batch_gen);

    CHECK("count_positive",    count > 0);
    CHECK("batch_gen_nonzero", batch_gen > 0);

    /* The pre_queue should have entries; first must be a keyframe */
    rec_frame_entry_t entry;
    bool got = rec_pre_queue_dequeue(&buf->pre_queue, &entry);
    CHECK("dequeue_ok",           got);
    CHECK("first_is_keyframe",    entry.is_keyframe);
    CHECK("batch_gen_matches",    entry.batch_gen == batch_gen);

    /* protect window should be set */
    uint32_t pg = atomic_load_explicit(&buf->protected_gen,
                                        memory_order_acquire);
    CHECK("protect_gen_set", pg == batch_gen);

    /* 2s pre-roll: now - 2s should find frame at 30 (ts = base + 0.99s)
     * or frame at 60 (ts = base + 1.98s). With 3s total and pre_sec=2,
     * target = now - 2s = base + 3s - 2s = base + 1s.
     * frame 30 is at base + 30*33ms = base + 990ms < base + 1s => valid.
     * frame 60 is at base + 60*33ms = base + 1980ms > base + 1s => not valid.
     * So best should be frame 30.
     */
    rec_buf_abort_pre(buf, batch_gen);

    /* Reset pre_queue */
    atomic_store_explicit(&buf->pre_queue.head, 0, memory_order_relaxed);
    atomic_store_explicit(&buf->pre_queue.tail, 0, memory_order_relaxed);

    /*
     * For pre_sec=2: use now = base + 3s so target = base + 1s.
     * frame 30 ts = base + 990ms <= target => selected.
     * frame 60 ts = base + 1980ms > target => not selected.
     */
    uint64_t now_ns2 = base_ts + 3000000000ULL;
    uint32_t batch_gen2 = 0;
    int count2 = rec_buf_extract_from_keyframe(buf, now_ns2, 2, &batch_gen2);
    CHECK("count2_positive", count2 > 0);
    CHECK("batch_gen2_different", batch_gen2 != batch_gen);

    rec_frame_entry_t entry2;
    bool got2 = rec_pre_queue_dequeue(&buf->pre_queue, &entry2);
    CHECK("dequeue2_ok",        got2);
    CHECK("entry2_is_keyframe", entry2.is_keyframe);

    /* The keyframe found should be frame 30 (ts = base + 990ms) */
    uint64_t expected_ts = base_ts + 30ULL * FRAME_NS;
    CHECK("entry2_correct_ts", entry2.timestamp_ns == expected_ts);

    printf("  info: pre_sec=2 → start_ts=%llu expected=%llu\n",
           (unsigned long long)entry2.timestamp_ns,
           (unsigned long long)expected_ts);

    rec_buf_destroy(&buf);
}

/* ─── Test 3: Wrap-around correctness ───────────────────────────── */
static void test_wrap_around(void)
{
    printf("\n[Test 3] Wrap-around: ring wraps, index offsets correct, no OOB\n");

    /* Small ring to force frequent wraps */
    uint32_t ring_sz = REC_BUF_SIZE_MIN; /* 1MB */
    rec_buf_t *buf = rec_buf_create(ring_sz);
    CHECK("create", buf != NULL);
    if (!buf) return;

    const uint32_t FRAME_SZ = 100 * 1024; /* 100KB per frame => 10 fit */
    uint8_t *frame_data = malloc(FRAME_SZ);
    CHECK("frame_alloc", frame_data != NULL);
    if (!frame_data) { rec_buf_destroy(&buf); return; }

    memset(frame_data, 0x55, FRAME_SZ);

    /* Push 50 frames → ring will wrap ~5 times */
    for (uint32_t i = 0; i < 50; i++) {
        frame_data[0] = (uint8_t)(i & 0xFF);
        int ret = rec_buf_push(buf, frame_data, FRAME_SZ,
                                (uint64_t)i * 33000000ULL, i,
                                (i % 10 == 0));
        CHECK("push_ok", ret == REC_OK);

        /* Verify all index entries have valid offsets */
        for (uint32_t j = 0; j < buf->index_count; j++) {
            uint32_t idx = (buf->index_head + j) % REC_FRAME_INDEX_MAX;
            rec_frame_entry_t *e = &buf->index[idx];
            CHECK("offset_in_range", e->offset < ring_sz);
            CHECK("size_correct",    e->size == FRAME_SZ);
        }
    }

    /* Verify read_ring works: read back the most recent frame */
    if (buf->index_count > 0) {
        uint32_t last_pos = (buf->index_tail + REC_FRAME_INDEX_MAX - 1)
                            % REC_FRAME_INDEX_MAX;
        rec_frame_entry_t *last = &buf->index[last_pos];
        uint8_t *readback = malloc(FRAME_SZ);
        CHECK("readback_alloc", readback != NULL);
        if (readback) {
            rec_buf_read_ring(buf, last->offset, last->size, readback);
            CHECK("readback_byte0_match", readback[0] == 49); /* last frame i=49 */
            free(readback);
        }
    }

    free(frame_data);
    rec_buf_destroy(&buf);
}

/* ─── Test 4: rec_buf_overlaps unit tests ────────────────────────── */
static void test_overlaps(void)
{
    printf("\n[Test 4] rec_buf_overlaps: non-wrap, single-side wrap, double-side wrap\n");

    uint32_t ring = 1000;

    /* Non-wrap, no overlap */
    CHECK("no_overlap_1", !rec_buf_overlaps_pub(0,   100, 200, 100, ring));
    CHECK("no_overlap_2", !rec_buf_overlaps_pub(200, 100, 0,   100, ring));

    /* Non-wrap, adjacent (no overlap) */
    CHECK("adjacent_no_overlap", !rec_buf_overlaps_pub(0, 100, 100, 100, ring));

    /* Non-wrap, overlap */
    CHECK("overlap_1", rec_buf_overlaps_pub(0,   150, 100, 100, ring));
    CHECK("overlap_2", rec_buf_overlaps_pub(100, 100, 50,  100, ring));

    /* Single-side wrap: span A wraps around */
    /* A = [950, 100) → [950..999] + [0..49]  (950+100=1050 > ring_size=1000) */
    CHECK("wrap_a_no_overlap",  !rec_buf_overlaps_pub(950, 100, 100, 100, ring));
    CHECK("wrap_a_overlap_end",  rec_buf_overlaps_pub(950, 100, 960,  40, ring)); /* B in A's first chunk */
    CHECK("wrap_a_overlap_start",rec_buf_overlaps_pub(950, 100, 0,    50, ring)); /* B in A's wrapped chunk */

    /* Single-side wrap: span B wraps */
    /* B = [950, 100) → [950..999] + [0..49] */
    CHECK("wrap_b_no_overlap",  !rec_buf_overlaps_pub(100, 100, 950, 100, ring));
    CHECK("wrap_b_overlap_end",  rec_buf_overlaps_pub( 40, 100, 950, 100, ring)); /* A overlaps B's wrapped chunk */
    CHECK("wrap_b_overlap_start",rec_buf_overlaps_pub(  0,  50, 950, 100, ring)); /* A fully inside B's wrapped chunk */

    /* Both spans wrap */
    CHECK("both_wrap_overlap",  rec_buf_overlaps_pub(800, 300, 900, 200, ring));
    CHECK("both_wrap_no_overlap", !rec_buf_overlaps_pub(100, 200, 400, 200, ring));

    /* Zero-size spans */
    CHECK("zero_size_a", !rec_buf_overlaps_pub(0, 0, 0, 100, ring));
    CHECK("zero_size_b", !rec_buf_overlaps_pub(0, 100, 0, 0, ring));
}

/* ─── Test 5: Protect window owner check ─────────────────────────── */
static void test_protect_owner(void)
{
    printf("\n[Test 5] Protect owner: old batch cannot clear new batch protect\n");

    uint32_t ring_sz = REC_BUF_SIZE_MIN;
    rec_buf_t *buf = rec_buf_create(ring_sz);
    CHECK("create", buf != NULL);
    if (!buf) return;

    const uint32_t FRAME_SZ = 4096;
    uint8_t frame_data[FRAME_SZ];
    memset(frame_data, 0xAA, FRAME_SZ);

    /* Push some frames with keyframes */
    for (uint32_t i = 0; i < 60; i++) {
        rec_buf_push(buf, frame_data, FRAME_SZ,
                     (uint64_t)i * 33000000ULL, i, (i % 30 == 0));
    }

    /* Extract batch 1 */
    uint64_t now_ns = 60ULL * 33000000ULL + 1000000ULL;
    uint32_t batch1 = 0;
    int c1 = rec_buf_extract_from_keyframe(buf, now_ns, 1, &batch1);
    CHECK("batch1_ok", c1 > 0 && batch1 > 0);

    uint32_t pg = atomic_load_explicit(&buf->protected_gen, memory_order_acquire);
    CHECK("protect_gen_is_batch1", pg == batch1);

    /* Simulate a NEW batch (batch2) by aborting batch1 and extracting again */
    rec_buf_abort_pre(buf, batch1);

    /* Reset pre_queue */
    atomic_store_explicit(&buf->pre_queue.head, 0, memory_order_relaxed);
    atomic_store_explicit(&buf->pre_queue.tail, 0, memory_order_relaxed);

    /* Push more frames */
    for (uint32_t i = 60; i < 120; i++) {
        rec_buf_push(buf, frame_data, FRAME_SZ,
                     (uint64_t)i * 33000000ULL, i, (i % 30 == 0));
    }

    uint32_t batch2 = 0;
    now_ns = 120ULL * 33000000ULL + 1000000ULL;
    int c2 = rec_buf_extract_from_keyframe(buf, now_ns, 1, &batch2);
    CHECK("batch2_ok", c2 > 0 && batch2 > 0);
    CHECK("batch2_gt_batch1", batch2 > batch1);

    pg = atomic_load_explicit(&buf->protected_gen, memory_order_acquire);
    CHECK("protect_gen_is_batch2", pg == batch2);

    /* Now try to abort with old batch1 — must NOT clear batch2's protect */
    rec_buf_abort_pre(buf, batch1);

    uint32_t pg_after = atomic_load_explicit(&buf->protected_gen,
                                              memory_order_acquire);
    CHECK("protect_still_batch2", pg_after == batch2);

    /* Abort with correct batch2 — MUST clear protect */
    rec_buf_abort_pre(buf, batch2);
    uint32_t pg_final = atomic_load_explicit(&buf->protected_gen,
                                              memory_order_acquire);
    CHECK("protect_cleared", pg_final == REC_PRE_GEN_NONE);

    rec_buf_destroy(&buf);
}

/* ─── Test 6: Batch abort — Writer discards remaining entries ─────── */
static void test_batch_abort(void)
{
    printf("\n[Test 6] Batch abort: writer discards remaining entries of aborted batch\n");

    uint32_t ring_sz = REC_BUF_SIZE_MIN;
    rec_buf_t *buf = rec_buf_create(ring_sz);
    CHECK("create", buf != NULL);
    if (!buf) return;

    const uint32_t FRAME_SZ = 4096;
    uint8_t frame_data[FRAME_SZ];
    memset(frame_data, 0xBB, FRAME_SZ);

    /* Push 30 frames (1 keyframe at 0) */
    for (uint32_t i = 0; i < 30; i++) {
        rec_buf_push(buf, frame_data, FRAME_SZ,
                     (uint64_t)i * 33000000ULL, i, (i == 0));
    }

    uint64_t now_ns = 30ULL * 33000000ULL + 1000000ULL;
    uint32_t batch_gen = 0;
    int count = rec_buf_extract_from_keyframe(buf, now_ns, 1, &batch_gen);
    CHECK("extract_ok", count > 0);
    CHECK("batch_gen_ok", batch_gen > 0);

    /* Abort the batch */
    rec_buf_abort_pre(buf, batch_gen);

    uint32_t aborted = atomic_load_explicit(&buf->aborted_pre_gen,
                                             memory_order_acquire);
    CHECK("aborted_gen_set", aborted == batch_gen);

    /*
     * Simulate Writer: dequeue entries and discard those from aborted batch.
     * In real code the Writer checks: if entry.batch_gen == aborted_pre_gen → skip.
     */
    int discarded = 0;
    int processed = 0;
    rec_frame_entry_t entry;
    while (rec_pre_queue_dequeue(&buf->pre_queue, &entry)) {
        uint32_t ab = atomic_load_explicit(&buf->aborted_pre_gen,
                                            memory_order_acquire);
        if (entry.batch_gen == ab) {
            discarded++;
        } else {
            processed++;
        }
    }

    CHECK("all_discarded",   discarded == count);
    CHECK("none_processed",  processed == 0);
    printf("  info: count=%d discarded=%d processed=%d\n",
           count, discarded, processed);

    /* After abort, protect window is cleared */
    uint32_t pg = atomic_load_explicit(&buf->protected_gen, memory_order_acquire);
    CHECK("protect_cleared_after_abort", pg == REC_PRE_GEN_NONE);

    rec_buf_destroy(&buf);
}

/* ─── main ───────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== Phase R1: rec_buf Unit Tests ===\n");

    test_eviction();
    test_iframe_location();
    test_wrap_around();
    test_overlaps();
    test_protect_owner();
    test_batch_abort();

    printf("\n=== Results: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("=== Phase R1: PASS ===\n");
        return 0;
    } else {
        printf("=== Phase R1: FAIL ===\n");
        return 1;
    }
}
