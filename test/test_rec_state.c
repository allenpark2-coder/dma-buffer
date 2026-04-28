/* test/test_rec_state.c — Phase R2 State Machine Acceptance Tests
 *
 * Build:
 *   gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -Irec \
 *       -o test_rec_state rec/rec_debounce.c rec/rec_buf.c rec/rec_state.c \
 *       test/test_rec_state.c
 *
 * All events injected directly — no sockets, no sleep.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdatomic.h>

#include "rec_state.h"

/* ─── Harness ────────────────────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;

#define PASS(name) do { printf("  PASS: %s\n", (name)); g_pass++; } while(0)
#define FAIL(name, msg) do { \
    printf("  FAIL: %s — %s (line %d)\n", (name), (msg), __LINE__); \
    g_fail++; \
} while(0)
#define CHECK(name, cond) do { \
    if (cond) PASS(name); else FAIL(name, #cond); \
} while(0)

/* Silent transition callback — suppresses stderr during tests. */
static void no_log(void *ud, rec_state_t from, rec_state_t to)
{
    (void)ud; (void)from; (void)to;
}

/* Helper: create a minimal rec_buf and push N keyframes. */
static rec_buf_t *make_buf_with_keyframes(uint32_t n)
{
    rec_buf_t *buf = rec_buf_create(REC_BUF_SIZE_MIN);
    if (!buf) return NULL;
    uint8_t data[4096];
    memset(data, 0xAA, sizeof(data));
    for (uint32_t i = 0; i < n; i++) {
        rec_buf_push(buf, data, sizeof(data),
                     (uint64_t)i * 33000000ULL, i,
                     true /* all keyframes */);
    }
    return buf;
}

/* Helper: shared ctx init with silent log. */
static void ctx_init(rec_state_ctx_t *ctx, rec_buf_t *buf, uint32_t pre_sec)
{
    rec_state_init(ctx, buf, pre_sec);
    ctx->on_transition  = no_log;
    ctx->transition_ud  = NULL;
}

/* ─── Test 1: IDLE → IN_EVENT (keyframe available) ──────────────────── */
static void test_idle_to_in_event(void)
{
    printf("\n[Test 1] IDLE → EXTRACT_PRE(inline) → IN_EVENT (keyframe available)\n");

    rec_buf_t *buf = make_buf_with_keyframes(30);
    CHECK("buf_created", buf != NULL);
    if (!buf) return;

    rec_state_ctx_t ctx;
    ctx_init(&ctx, buf, 2 /* pre_sec */);
    CHECK("initial_idle", ctx.state == REC_STATE_IDLE);

    /* ts_ns = 30 frames × 33ms = 990ms, so "now" is 990ms.
     * pre_sec=2 → target = 990ms - 2000ms = negative → fallback to oldest keyframe.
     * extract_from_keyframe will find frame 0 and return count > 0. */
    uint64_t now = 30ULL * 33000000ULL;
    rec_state_on_trigger(&ctx, REC_TRIGGER_START, now);

    CHECK("state_in_event",     ctx.state == REC_STATE_IN_EVENT);
    CHECK("last_batch_gen_set", ctx.last_batch_gen > 0);

    rec_buf_destroy(&buf);
}

/* ─── Test 2: IDLE → WAIT_KEYFRAME (no keyframe in empty buf) ────────── */
static void test_idle_to_wait_keyframe(void)
{
    printf("\n[Test 2] IDLE → EXTRACT_PRE(inline) → WAIT_KEYFRAME (empty buf)\n");

    rec_buf_t *buf = rec_buf_create(REC_BUF_SIZE_MIN);
    CHECK("buf_created", buf != NULL);
    if (!buf) return;
    /* buf is empty — no keyframes */

    rec_state_ctx_t ctx;
    ctx_init(&ctx, buf, 2);

    rec_state_on_trigger(&ctx, REC_TRIGGER_START, 1000000000ULL);

    CHECK("state_wait_keyframe", ctx.state == REC_STATE_WAIT_KEYFRAME);
    CHECK("batch_gen_zero",      ctx.last_batch_gen == 0);

    rec_buf_destroy(&buf);
}

/* ─── Test 3: WAIT_KEYFRAME → IN_EVENT (IDR arrives) ────────────────── */
static void test_wait_keyframe_to_in_event(void)
{
    printf("\n[Test 3] WAIT_KEYFRAME + on_keyframe() → IN_EVENT\n");

    rec_buf_t *buf = rec_buf_create(REC_BUF_SIZE_MIN);
    CHECK("buf_created", buf != NULL);
    if (!buf) return;

    rec_state_ctx_t ctx;
    ctx_init(&ctx, buf, 2);

    /* Force into WAIT_KEYFRAME (empty buf → no keyframe) */
    rec_state_on_trigger(&ctx, REC_TRIGGER_START, 1000000000ULL);
    CHECK("in_wait_keyframe", ctx.state == REC_STATE_WAIT_KEYFRAME);

    /* IDR arrives → IN_EVENT */
    rec_state_on_keyframe(&ctx);
    CHECK("state_in_event", ctx.state == REC_STATE_IN_EVENT);

    rec_buf_destroy(&buf);
}

/* ─── Test 4: WAIT_KEYFRAME + STOP → IDLE (no post-roll) ────────────── */
static void test_wait_keyframe_stop_to_idle(void)
{
    printf("\n[Test 4] WAIT_KEYFRAME + STOP → IDLE (skip POST_WAIT)\n");

    rec_buf_t *buf = rec_buf_create(REC_BUF_SIZE_MIN);
    CHECK("buf_created", buf != NULL);
    if (!buf) return;

    rec_state_ctx_t ctx;
    ctx_init(&ctx, buf, 2);

    rec_state_on_trigger(&ctx, REC_TRIGGER_START, 1000000000ULL);
    CHECK("in_wait_keyframe", ctx.state == REC_STATE_WAIT_KEYFRAME);

    /* STOP while waiting → IDLE directly (no file was opened) */
    rec_state_on_trigger(&ctx, REC_TRIGGER_STOP, 1500000000ULL);
    CHECK("state_idle", ctx.state == REC_STATE_IDLE);

    rec_buf_destroy(&buf);
}

/* ─── Test 5: POST_WAIT → IN_EVENT (new START during post-roll) ──────── */
static void test_post_wait_retrigger(void)
{
    printf("\n[Test 5] POST_WAIT + TRIGGER_START → IN_EVENT (re-trigger)\n");

    rec_buf_t *buf = make_buf_with_keyframes(30);
    CHECK("buf_created", buf != NULL);
    if (!buf) return;

    rec_state_ctx_t ctx;
    ctx_init(&ctx, buf, 1);

    uint64_t t = 30ULL * 33000000ULL;

    /* Get into IN_EVENT */
    rec_state_on_trigger(&ctx, REC_TRIGGER_START, t);
    CHECK("in_event", ctx.state == REC_STATE_IN_EVENT);

    /* STOP → POST_WAIT */
    rec_state_on_trigger(&ctx, REC_TRIGGER_STOP, t + 1000000000ULL);
    CHECK("in_post_wait",    ctx.state == REC_STATE_POST_WAIT);
    CHECK("pending_false",   ctx.pending_trigger == false);
    CHECK("countdown_set",   ctx.post_remaining_sec == REC_POST_RECORD_SEC_DEFAULT);

    /* New START arrives during post-roll (debounce window > 500ms, use +600ms) */
    rec_state_on_trigger(&ctx, REC_TRIGGER_START, t + 1600000000ULL);
    CHECK("back_to_in_event",  ctx.state == REC_STATE_IN_EVENT);
    CHECK("pending_false",     ctx.pending_trigger == false);  /* fix M3: must be false so POST_WAIT timer can expire */

    rec_buf_destroy(&buf);
}

/* ─── Test 6: POST_WAIT countdown → IDLE ─────────────────────────────── */
static void test_post_wait_countdown(void)
{
    printf("\n[Test 6] POST_WAIT + timer_tick(countdown) → IDLE\n");

    rec_buf_t *buf = make_buf_with_keyframes(30);
    CHECK("buf_created", buf != NULL);
    if (!buf) return;

    rec_state_ctx_t ctx;
    ctx_init(&ctx, buf, 1);

    uint64_t t = 30ULL * 33000000ULL;

    rec_state_on_trigger(&ctx, REC_TRIGGER_START, t);
    CHECK("in_event", ctx.state == REC_STATE_IN_EVENT);

    rec_state_on_trigger(&ctx, REC_TRIGGER_STOP, t + 1000000000ULL);
    CHECK("in_post_wait", ctx.state == REC_STATE_POST_WAIT);

    int initial = ctx.post_remaining_sec;
    CHECK("countdown_default", initial == REC_POST_RECORD_SEC_DEFAULT);

    /* Tick down most of the countdown (not all) — must stay in POST_WAIT */
    rec_state_on_timer_tick(&ctx, (uint64_t)(initial - 1));
    CHECK("still_post_wait", ctx.state == REC_STATE_POST_WAIT);
    CHECK("remaining_1",     ctx.post_remaining_sec == 1);

    /* Final tick — should go to IDLE */
    rec_state_on_timer_tick(&ctx, 1);
    CHECK("state_idle",       ctx.state == REC_STATE_IDLE);

    /* Extra ticks after IDLE — must be no-ops */
    rec_state_on_timer_tick(&ctx, 5);
    CHECK("still_idle_after_extra_ticks", ctx.state == REC_STATE_IDLE);

    rec_buf_destroy(&buf);
}

/* ─── Test 7: Schedule gate — TRIGGER_START silently ignored ─────────── */
static bool sched_inactive(void *ud) { (void)ud; return false; }
static bool sched_active(void *ud)   { (void)ud; return true;  }

static void test_schedule_gate(void)
{
    printf("\n[Test 7] Schedule off → TRIGGER_START silently ignored\n");

    rec_buf_t *buf = make_buf_with_keyframes(30);
    CHECK("buf_created", buf != NULL);
    if (!buf) return;

    rec_state_ctx_t ctx;
    ctx_init(&ctx, buf, 1);

    /* Attach inactive schedule */
    ctx.is_schedule_active = sched_inactive;

    uint64_t t = 30ULL * 33000000ULL;

    /* START must be ignored */
    rec_state_on_trigger(&ctx, REC_TRIGGER_START, t);
    CHECK("stays_idle_when_inactive", ctx.state == REC_STATE_IDLE);

    /* Activate schedule → START now works */
    ctx.is_schedule_active = sched_active;
    rec_state_on_trigger(&ctx, REC_TRIGGER_START, t + 1000000000ULL);
    CHECK("in_event_when_active", ctx.state == REC_STATE_IN_EVENT);

    rec_buf_destroy(&buf);
}

/* ─── Test 8: timerfd expirations consumed correctly ─────────────────── */
static void test_timerfd_expirations(void)
{
    printf("\n[Test 8] timerfd: multi-expiration batch consumed correctly\n");

    rec_buf_t *buf = make_buf_with_keyframes(30);
    CHECK("buf_created", buf != NULL);
    if (!buf) return;

    rec_state_ctx_t ctx;
    ctx_init(&ctx, buf, 1);

    uint64_t t = 30ULL * 33000000ULL;

    rec_state_on_trigger(&ctx, REC_TRIGGER_START, t);
    rec_state_on_trigger(&ctx, REC_TRIGGER_STOP, t + 1000000000ULL);
    CHECK("in_post_wait", ctx.state == REC_STATE_POST_WAIT);

    /* Simulate timerfd returning a batch of expirations that overshoots zero */
    int cdown = ctx.post_remaining_sec;
    CHECK("cdown_positive", cdown > 0);

    /* Deliver more expirations than remaining — still must transition to IDLE */
    rec_state_on_timer_tick(&ctx, (uint64_t)(cdown + 10));
    CHECK("state_idle_after_overshoot", ctx.state == REC_STATE_IDLE);
    CHECK("remaining_negative",         ctx.post_remaining_sec <= 0);

    /* timerfd tick in non-POST_WAIT state is a no-op */
    rec_state_ctx_t ctx2;
    ctx_init(&ctx2, buf, 1);
    CHECK("ctx2_idle", ctx2.state == REC_STATE_IDLE);
    rec_state_on_timer_tick(&ctx2, 5);
    CHECK("tick_noop_in_idle", ctx2.state == REC_STATE_IDLE);

    rec_buf_destroy(&buf);
}

/* ─── main ───────────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== Phase R2: rec_state Unit Tests ===\n");

    test_idle_to_in_event();
    test_idle_to_wait_keyframe();
    test_wait_keyframe_to_in_event();
    test_wait_keyframe_stop_to_idle();
    test_post_wait_retrigger();
    test_post_wait_countdown();
    test_schedule_gate();
    test_timerfd_expirations();

    printf("\n=== Results: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("=== Phase R2: PASS ===\n");
        return 0;
    }
    printf("=== Phase R2: FAIL ===\n");
    return 1;
}
