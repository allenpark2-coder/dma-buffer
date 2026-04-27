# Phase R2: State Machine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the 5-state Recorder state machine (IDLE/EXTRACT_PRE/WAIT_KEYFRAME/IN_EVENT/POST_WAIT) with injectable event interfaces for deterministic unit testing — no file I/O.

**Architecture:** The state machine (`rec_state.c`) exposes three injectable functions — `rec_state_on_trigger`, `rec_state_on_timer_tick`, `rec_state_on_keyframe` — that are called either by the real Unix-socket listener (`rec_trigger.c`) in production or directly in tests. The debouncer (`rec_debounce.c`) is a standalone time filter embedded in the state context. Tests in `test_rec_state.c` call these functions with fabricated timestamps and never open sockets or sleep.

**Tech Stack:** C11, POSIX (timerfd concept abstracted away), `_Atomic`, existing `rec_buf_t` from Phase R1, `rec_defs.h` constants.

---

## File Map

| Action | Path | Responsibility |
|--------|------|----------------|
| Modify | `ipc/vfr_ipc_types.h` | Add `vfr_event_msg_t` (AI→Recorder IPC message) |
| Create | `rec/rec_debounce.h` | Debouncer struct + API |
| Create | `rec/rec_debounce.c` | Time-window filter for START/STOP events |
| Create | `rec/rec_state.h` | State context struct + injectable API |
| Create | `rec/rec_state.c` | 5-state machine transitions |
| Create | `rec/rec_trigger.h` | Unix socket listener API |
| Create | `rec/rec_trigger.c` | Abstract-namespace socket, accept + dispatch |
| Create | `test/test_rec_state.c` | 8-checklist unit tests, direct injection, no socket |
| Modify | `Makefile` | Add `test_rec_state`, `test_rec_state_asan`, `check_r2` |

---

## Task 1: `vfr_event_msg_t` in `ipc/vfr_ipc_types.h`

**Files:**
- Modify: `ipc/vfr_ipc_types.h:82-84`

- [ ] **Step 1: Add the message type before the `#endif`**

Open `ipc/vfr_ipc_types.h`. Before the final `#endif`, append:

```c
/* ─── AI Process → Recorder：事件觸發訊息（Phase R2+）────────────────────
 * AI process 發送此訊息到 Recorder 的 Unix abstract socket:
 *   \0/vfr/event/<stream_name>
 * 連線後發送一次即關閉（無長連線）。
 */
#define VFR_EVENT_MAGIC  0x45564E54u   /* "EVNT" */

typedef struct {
    uint32_t magic;                          /* VFR_EVENT_MAGIC */
    uint32_t event_type;                     /* rec_trigger_type_t */
    uint64_t timestamp_ns;                   /* CLOCK_MONOTONIC */
    float    confidence;                     /* 0.0 ~ 1.0 */
    char     stream_name[VFR_SOCKET_NAME_MAX];
    char     label[32];
} vfr_event_msg_t;
```

- [ ] **Step 2: Verify it compiles**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
gcc -std=c11 -D_GNU_SOURCE -Iinclude -Irec -c ipc/vfr_ipc_types.h -o /dev/null 2>&1 || echo "FAIL"
```

Expected: no output (header-only compile succeeds) or no FAIL.

- [ ] **Step 3: Commit**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
git add ipc/vfr_ipc_types.h
git commit -m "rec: add vfr_event_msg_t to ipc/vfr_ipc_types.h (Phase R2)"
```

---

## Task 2: `rec/rec_debounce.h` + `rec/rec_debounce.c`

**Files:**
- Create: `rec/rec_debounce.h`
- Create: `rec/rec_debounce.c`

- [ ] **Step 1: Create `rec/rec_debounce.h`**

```c
/* rec/rec_debounce.h — Signal debouncer for Recorder trigger events */
#ifndef REC_DEBOUNCE_H
#define REC_DEBOUNCE_H

#include <stdint.h>
#include <stdbool.h>
#include "rec_defs.h"

typedef struct {
    uint64_t last_start_ns;   /* timestamp of last accepted START (0 = never) */
    uint64_t last_stop_ns;    /* timestamp of last accepted STOP  (0 = never) */
    uint32_t debounce_ms;     /* filter window in milliseconds */
} rec_debounce_t;

/*
 * rec_debounce_init(): zero timestamps, set window.
 */
void rec_debounce_init(rec_debounce_t *d, uint32_t debounce_ms);

/*
 * rec_debounce_filter():
 *   Returns true  → event passes, caller should process it.
 *   Returns false → event is within the debounce window, discard.
 *
 * START rule: filtered if (ts_ns - last_start_ns) < debounce_ns.
 * STOP  rule: filtered if (ts_ns - last_start_ns) < debounce_ns
 *             (prevents glitch STOP that arrives right after a START).
 *
 * On pass, updates last_start_ns or last_stop_ns.
 */
bool rec_debounce_filter(rec_debounce_t *d, rec_trigger_type_t type,
                         uint64_t ts_ns);

#endif /* REC_DEBOUNCE_H */
```

- [ ] **Step 2: Create `rec/rec_debounce.c`**

```c
/* rec/rec_debounce.c */
#include <string.h>
#include "rec_debounce.h"

void rec_debounce_init(rec_debounce_t *d, uint32_t debounce_ms)
{
    memset(d, 0, sizeof(*d));
    d->debounce_ms = debounce_ms;
}

bool rec_debounce_filter(rec_debounce_t *d, rec_trigger_type_t type,
                         uint64_t ts_ns)
{
    uint64_t window_ns = (uint64_t)d->debounce_ms * 1000000ULL;

    if (type == REC_TRIGGER_START) {
        if (d->last_start_ns != 0 &&
            ts_ns - d->last_start_ns < window_ns)
            return false;   /* filtered */
        d->last_start_ns = ts_ns;
        return true;
    } else {
        /* TRIGGER_STOP: discard if too close to last accepted START */
        if (d->last_start_ns != 0 &&
            ts_ns - d->last_start_ns < window_ns)
            return false;
        d->last_stop_ns = ts_ns;
        return true;
    }
}
```

- [ ] **Step 3: Compile-check in isolation**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -Irec -c rec/rec_debounce.c -o /tmp/rec_debounce.o
echo "exit=$?"
```

Expected: `exit=0`, no warnings.

- [ ] **Step 4: Commit**

```bash
git add rec/rec_debounce.h rec/rec_debounce.c
git commit -m "rec: add rec_debounce — 500ms trigger signal debouncer"
```

---

## Task 3: `rec/rec_state.h` + `rec/rec_state.c`

**Files:**
- Create: `rec/rec_state.h`
- Create: `rec/rec_state.c`

- [ ] **Step 1: Create `rec/rec_state.h`**

```c
/* rec/rec_state.h — 5-state Recorder state machine */
#ifndef REC_STATE_H
#define REC_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "rec_defs.h"
#include "rec_buf.h"
#include "rec_debounce.h"

/*
 * rec_state_ctx_t — all state owned in one flat struct (no heap alloc).
 *
 * Lifecycle:
 *   rec_state_init()        → zero + wire up buf/pre_sec
 *   rec_state_on_trigger()  → inject START/STOP event (from socket or test)
 *   rec_state_on_timer_tick() → inject timerfd expiration count
 *   rec_state_on_keyframe() → inject IDR arrival (from VFR consumer loop)
 *   rec_state_force_idle()  → emergency reset (called by rec_buf_push on timeout)
 */
typedef struct rec_state_ctx {
    rec_state_t    state;
    rec_buf_t     *buf;            /* ring buffer — used for EXTRACT_PRE */
    uint32_t       pre_sec;        /* pre-roll window in seconds */
    uint32_t       last_batch_gen; /* batch_gen of the active pre-roll (0 = none) */
    int            post_remaining_sec;  /* POST_WAIT countdown; starts at REC_POST_RECORD_SEC_DEFAULT */
    bool           pending_trigger;     /* new START received while in POST_WAIT */

    /*
     * Schedule gate (optional).
     * If non-NULL, called before processing any TRIGGER_START.
     * Returns true  → recording is allowed in the current time slot.
     * Returns false → TRIGGER_START is silently ignored.
     * Set to NULL to always allow (useful in tests).
     */
    bool (*is_schedule_active)(void *ud);
    void *schedule_ud;

    rec_debounce_t debounce;

    /*
     * Transition log (optional).
     * If non-NULL, called on every state change (before the new state takes effect).
     * If NULL, transitions are printed to stderr.
     * Set to a no-op lambda in tests to suppress output.
     */
    void (*on_transition)(void *ud, rec_state_t from, rec_state_t to);
    void *transition_ud;
} rec_state_ctx_t;

/*
 * rec_state_init(): initialise ctx.
 *   buf      — ring buffer (must outlive ctx); may be NULL for schedule-only tests.
 *   pre_sec  — pre-roll window in seconds passed to rec_buf_extract_from_keyframe.
 * Debounce window defaults to REC_DEBOUNCE_MS.
 * All callbacks default to NULL.
 */
void rec_state_init(rec_state_ctx_t *ctx, rec_buf_t *buf, uint32_t pre_sec);

/*
 * rec_state_on_trigger():
 *   Applies debounce filter, checks schedule gate, then drives state transitions.
 *   ts_ns — monotonic timestamp of the event (used both for debounce and as
 *            now_ns for rec_buf_extract_from_keyframe).
 *
 *   START transitions:
 *     IDLE         → EXTRACT_PRE (inline) → IN_EVENT  (keyframe found)
 *                                          → WAIT_KEYFRAME (no keyframe)
 *     POST_WAIT    → IN_EVENT             (re-trigger during post-roll)
 *
 *   STOP transitions:
 *     IN_EVENT     → POST_WAIT
 *     WAIT_KEYFRAME → IDLE               (never opened a file, skip post-roll)
 */
void rec_state_on_trigger(rec_state_ctx_t *ctx, rec_trigger_type_t type,
                           uint64_t ts_ns);

/*
 * rec_state_on_timer_tick():
 *   Inject timerfd expiration count. Only acts in POST_WAIT state.
 *   Decrements post_remaining_sec by expirations.
 *   Transitions to IDLE when countdown reaches 0 (and no pending_trigger).
 */
void rec_state_on_timer_tick(rec_state_ctx_t *ctx, uint64_t expirations);

/*
 * rec_state_on_keyframe():
 *   Inject IDR/I-frame arrival notification. Only acts in WAIT_KEYFRAME state.
 *   Transitions to IN_EVENT (the frame itself is enqueued by the VFR consumer loop).
 */
void rec_state_on_keyframe(rec_state_ctx_t *ctx);

/*
 * rec_state_force_idle():
 *   Emergency reset called by rec_buf_push() on protect-window spin timeout.
 *   Aborts the current pre-roll batch (if any) and transitions to IDLE.
 */
void rec_state_force_idle(rec_state_ctx_t *ctx);

/* rec_state_get(): read current state (no locking — single-threaded event loop). */
rec_state_t rec_state_get(const rec_state_ctx_t *ctx);

#endif /* REC_STATE_H */
```

- [ ] **Step 2: Create `rec/rec_state.c`**

```c
/* rec/rec_state.c — Recorder 5-state machine */
#include <stdio.h>
#include <string.h>
#include "rec_state.h"

/* ─── Internal helpers ───────────────────────────────────────────────── */

static const char *state_name(rec_state_t s)
{
    switch (s) {
    case REC_STATE_IDLE:          return "IDLE";
    case REC_STATE_EXTRACT_PRE:   return "EXTRACT_PRE";
    case REC_STATE_WAIT_KEYFRAME: return "WAIT_KEYFRAME";
    case REC_STATE_IN_EVENT:      return "IN_EVENT";
    case REC_STATE_POST_WAIT:     return "POST_WAIT";
    default:                      return "UNKNOWN";
    }
}

static void do_transition(rec_state_ctx_t *ctx, rec_state_t new_state)
{
    rec_state_t old = ctx->state;
    if (old == new_state)
        return;
    ctx->state = new_state;
    if (ctx->on_transition) {
        ctx->on_transition(ctx->transition_ud, old, new_state);
    } else {
        fprintf(stderr, "[rec_state] %s → %s\n",
                state_name(old), state_name(new_state));
    }
}

/* ─── Public API ─────────────────────────────────────────────────────── */

void rec_state_init(rec_state_ctx_t *ctx, rec_buf_t *buf, uint32_t pre_sec)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state   = REC_STATE_IDLE;
    ctx->buf     = buf;
    ctx->pre_sec = pre_sec;
    rec_debounce_init(&ctx->debounce, REC_DEBOUNCE_MS);
}

void rec_state_on_trigger(rec_state_ctx_t *ctx, rec_trigger_type_t type,
                           uint64_t ts_ns)
{
    /* ── Debounce filter ─────────────────────────────────────────────── */
    if (!rec_debounce_filter(&ctx->debounce, type, ts_ns))
        return;

    if (type == REC_TRIGGER_START) {
        /* ── Schedule gate ───────────────────────────────────────────── */
        if (ctx->is_schedule_active &&
            !ctx->is_schedule_active(ctx->schedule_ud))
            return;

        switch (ctx->state) {
        case REC_STATE_IDLE: {
            /*
             * EXTRACT_PRE (inline): call rec_buf_extract_from_keyframe.
             * ts_ns serves as now_ns for the pre-roll window calculation.
             */
            uint32_t batch_gen = 0;
            int count = 0;
            if (ctx->buf)
                count = rec_buf_extract_from_keyframe(ctx->buf, ts_ns,
                                                      ctx->pre_sec, &batch_gen);
            if (count > 0) {
                ctx->last_batch_gen = batch_gen;
                do_transition(ctx, REC_STATE_IN_EVENT);
            } else {
                do_transition(ctx, REC_STATE_WAIT_KEYFRAME);
            }
            break;
        }
        case REC_STATE_POST_WAIT:
            /*
             * New START during post-roll: return to recording immediately.
             * Reset countdown so a subsequent STOP restarts the full window.
             */
            ctx->pending_trigger        = true;
            ctx->post_remaining_sec     = REC_POST_RECORD_SEC_DEFAULT;
            do_transition(ctx, REC_STATE_IN_EVENT);
            break;
        default:
            break;   /* EXTRACT_PRE / WAIT_KEYFRAME / IN_EVENT: ignore */
        }

    } else { /* REC_TRIGGER_STOP */
        switch (ctx->state) {
        case REC_STATE_IN_EVENT:
            ctx->post_remaining_sec = REC_POST_RECORD_SEC_DEFAULT;
            ctx->pending_trigger    = false;
            do_transition(ctx, REC_STATE_POST_WAIT);
            break;
        case REC_STATE_WAIT_KEYFRAME:
            /* Never opened a file — skip POST_WAIT, go straight to IDLE. */
            do_transition(ctx, REC_STATE_IDLE);
            break;
        default:
            break;
        }
    }
}

void rec_state_on_timer_tick(rec_state_ctx_t *ctx, uint64_t expirations)
{
    if (ctx->state != REC_STATE_POST_WAIT)
        return;

    ctx->post_remaining_sec -= (int)expirations;
    if (ctx->post_remaining_sec <= 0 && !ctx->pending_trigger)
        do_transition(ctx, REC_STATE_IDLE);
}

void rec_state_on_keyframe(rec_state_ctx_t *ctx)
{
    if (ctx->state != REC_STATE_WAIT_KEYFRAME)
        return;
    do_transition(ctx, REC_STATE_IN_EVENT);
}

void rec_state_force_idle(rec_state_ctx_t *ctx)
{
    if (ctx->buf && ctx->last_batch_gen != 0) {
        rec_buf_abort_pre(ctx->buf, ctx->last_batch_gen);
        ctx->last_batch_gen = 0;
    }
    do_transition(ctx, REC_STATE_IDLE);
}

rec_state_t rec_state_get(const rec_state_ctx_t *ctx)
{
    return ctx->state;
}
```

- [ ] **Step 3: Compile-check both files**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -Irec \
    -c rec/rec_debounce.c -o /tmp/rec_debounce.o
gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -Irec \
    -c rec/rec_state.c -o /tmp/rec_state.o
echo "exit=$?"
```

Expected: `exit=0`, no warnings.

- [ ] **Step 4: Commit**

```bash
git add rec/rec_state.h rec/rec_state.c
git commit -m "rec: add rec_state — 5-state machine with injectable event API"
```

---

## Task 4: `test/test_rec_state.c` — Phase R2 acceptance tests

**Files:**
- Create: `test/test_rec_state.c`

All 8 checklist items. No sockets, no `sleep()`. Tests use fabricated timestamps.

- [ ] **Step 1: Create `test/test_rec_state.c`**

```c
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
    CHECK("pending_true",      ctx.pending_trigger == true);

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
```

- [ ] **Step 2: Build (expect link errors — rec_state.c not yet in Makefile, use direct gcc)**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -Irec \
    -o test_rec_state \
    rec/rec_debounce.c rec/rec_buf.c rec/rec_state.c \
    test/test_rec_state.c
echo "build exit=$?"
```

Expected: `build exit=0` with no warnings.

- [ ] **Step 3: Run tests**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
./test_rec_state 2>/dev/null
```

Expected output ends with:
```
=== Results: N PASS, 0 FAIL ===
=== Phase R2: PASS ===
```

If any test fails, read the FAIL line, identify the assertion, trace the state machine logic in `rec_state.c`, and fix.

- [ ] **Step 4: Run with ASan**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -Irec \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o test_rec_state_asan \
    rec/rec_debounce.c rec/rec_buf.c rec/rec_state.c \
    test/test_rec_state.c
./test_rec_state_asan 2>/dev/null
echo "asan exit=$?"
```

Expected: `asan exit=0`.

- [ ] **Step 5: Commit**

```bash
git add test/test_rec_state.c
git commit -m "rec: add test_rec_state — Phase R2 acceptance tests (8 tests)"
```

---

## Task 5: `rec/rec_trigger.h` + `rec/rec_trigger.c`

Implements the Unix abstract-namespace socket listener. Not tested in Phase R2 — integration happens in Phase R4.

**Files:**
- Create: `rec/rec_trigger.h`
- Create: `rec/rec_trigger.c`

- [ ] **Step 1: Create `rec/rec_trigger.h`**

```c
/* rec/rec_trigger.h — AI-process → Recorder Unix socket listener */
#ifndef REC_TRIGGER_H
#define REC_TRIGGER_H

#include <stdint.h>
#include "rec_defs.h"

typedef struct rec_trigger rec_trigger_t;

/*
 * rec_trigger_create():
 *   Binds a SOCK_STREAM socket at the abstract path \0/vfr/event/<stream_name>.
 *   on_trigger is called for each valid vfr_event_msg_t received.
 *   Returns NULL on failure (errno set).
 *
 *   The returned fd is SOCK_NONBLOCK — caller must poll (epoll) it.
 */
rec_trigger_t *rec_trigger_create(
    const char *stream_name,
    void (*on_trigger)(void *ud, rec_trigger_type_t type, uint64_t ts_ns),
    void *ud);

void rec_trigger_destroy(rec_trigger_t **t);

/*
 * rec_trigger_get_fd(): returns the listening fd to register with epoll.
 */
int rec_trigger_get_fd(const rec_trigger_t *t);

/*
 * rec_trigger_handle_accept():
 *   Call when epoll reports EPOLLIN on the listen fd.
 *   Accepts one connection, reads exactly sizeof(vfr_event_msg_t), validates
 *   magic, calls on_trigger, closes the connection.
 *   Short reads or bad magic are silently discarded.
 */
void rec_trigger_handle_accept(rec_trigger_t *t);

#endif /* REC_TRIGGER_H */
```

- [ ] **Step 2: Create `rec/rec_trigger.c`**

```c
/* rec/rec_trigger.c — Unix abstract-namespace socket listener */
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rec_trigger.h"
#include "../ipc/vfr_ipc_types.h"

struct rec_trigger {
    int    listen_fd;
    void (*on_trigger)(void *ud, rec_trigger_type_t type, uint64_t ts_ns);
    void  *ud;
};

static socklen_t make_addr(const char *stream_name, struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    /* Abstract namespace: sun_path[0] = '\0', remainder is the name */
    int n = snprintf(addr->sun_path + 1, sizeof(addr->sun_path) - 1,
                     "/vfr/event/%s", stream_name);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

rec_trigger_t *rec_trigger_create(
    const char *stream_name,
    void (*on_trigger)(void *ud, rec_trigger_type_t type, uint64_t ts_ns),
    void *ud)
{
    if (!stream_name || !on_trigger)
        return NULL;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;

    struct sockaddr_un addr;
    socklen_t addrlen = make_addr(stream_name, &addr);

    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        close(fd);
        return NULL;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return NULL;
    }

    rec_trigger_t *t = malloc(sizeof(*t));
    if (!t) {
        close(fd);
        return NULL;
    }
    t->listen_fd  = fd;
    t->on_trigger = on_trigger;
    t->ud         = ud;
    return t;
}

void rec_trigger_destroy(rec_trigger_t **t)
{
    if (!t || !*t)
        return;
    close((*t)->listen_fd);
    free(*t);
    *t = NULL;
}

int rec_trigger_get_fd(const rec_trigger_t *t)
{
    return t ? t->listen_fd : -1;
}

void rec_trigger_handle_accept(rec_trigger_t *t)
{
    if (!t)
        return;

    int conn = accept4(t->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (conn < 0)
        return;

    vfr_event_msg_t msg;
    ssize_t n = recv(conn, &msg, sizeof(msg), MSG_WAITALL);
    close(conn);

    if (n != (ssize_t)sizeof(msg) || msg.magic != VFR_EVENT_MAGIC)
        return;

    t->on_trigger(t->ud, (rec_trigger_type_t)msg.event_type, msg.timestamp_ns);
}
```

- [ ] **Step 3: Compile-check**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -Irec -Iinclude \
    -c rec/rec_trigger.c -o /tmp/rec_trigger.o
echo "exit=$?"
```

Expected: `exit=0`, no warnings.

- [ ] **Step 4: Commit**

```bash
git add rec/rec_trigger.h rec/rec_trigger.c
git commit -m "rec: add rec_trigger — Unix abstract-namespace AI event listener"
```

---

## Task 6: Makefile — `test_rec_state`, `test_rec_state_asan`, `check_r2`

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add source variable and targets after the `check_r1` block**

Find the line `check_r1: test_rec_buf` in `Makefile` and add the following block **after** the `check_r1` / `clean` targets (before `clean`):

```makefile
# ── Phase R2 — State Machine ─────────────────────────────────────────────────
SRCS_REC_STATE = \
    rec/rec_debounce.c \
    rec/rec_buf.c \
    rec/rec_state.c

SRCS_TEST_REC_STATE = \
    test/test_rec_state.c

test_rec_state: $(SRCS_REC_STATE) $(SRCS_TEST_REC_STATE)
	$(CC) $(CFLAGS) $(INCLUDES) -Irec -o $@ $^

test_rec_state_asan: $(SRCS_REC_STATE) $(SRCS_TEST_REC_STATE)
	$(CC) $(CFLAGS) $(INCLUDES) -Irec \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ $^
	./$@

check_r2: test_rec_state
	@echo "=== Running Phase R2 Tests ==="
	./test_rec_state
	@echo "=== Phase R2: PASS ==="
```

Also add `test_rec_state test_rec_state_asan` to the `clean` target's `rm -f` list.

- [ ] **Step 2: Build and run via Makefile**

```bash
cd /home/allen/vivotek/dma-buffer/.worktrees/phase-r1
make test_rec_state
./test_rec_state 2>/dev/null
```

Expected: `=== Phase R2: PASS ===`

- [ ] **Step 3: Run check_r2**

```bash
make check_r2 2>/dev/null
```

Expected: `=== Phase R2: PASS ===`

- [ ] **Step 4: Run ASan via make**

```bash
make test_rec_state_asan 2>/dev/null
echo "asan exit=$?"
```

Expected: `asan exit=0`

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "build: add test_rec_state, test_rec_state_asan, check_r2 targets"
```

---

## Self-Review Checklist

### Spec Coverage

| Checklist item | Task/Step |
|----------------|-----------|
| IDLE → EXTRACT_PRE → IN_EVENT | Task 4 Test 1 |
| EXTRACT_PRE → WAIT_KEYFRAME (no keyframe) | Task 4 Test 2 |
| WAIT_KEYFRAME + IDR → IN_EVENT | Task 4 Test 3 |
| WAIT_KEYFRAME + STOP → IDLE | Task 4 Test 4 |
| POST_WAIT → IN_EVENT (new trigger) | Task 4 Test 5 |
| POST_WAIT → IDLE (countdown done) | Task 4 Test 6 |
| Schedule off → TRIGGER_START ignored | Task 4 Test 7 |
| timerfd expirations consumed correctly | Task 4 Test 8 |
| `vfr_event_msg_t` IPC message | Task 1 |
| `rec_debounce.c` time filter | Task 2 |
| `rec_trigger.c` Unix socket listener | Task 5 |

All 8 checklist items are covered. ✅

### Type Consistency

- `rec_state_ctx_t.state` → `rec_state_t` (from `rec_defs.h`) ✅
- `rec_state_on_trigger(ctx, REC_TRIGGER_START/STOP, ts_ns)` — same signature across tasks ✅
- `rec_buf_extract_from_keyframe(buf, ts_ns, pre_sec, &batch_gen)` — matches Phase R1 `rec_buf.h` ✅
- `rec_buf_abort_pre(buf, batch_gen)` — matches Phase R1 ✅
- `VFR_EVENT_MAGIC` defined in `ipc/vfr_ipc_types.h`, used in `rec_trigger.c` ✅
- `REC_POST_RECORD_SEC_DEFAULT` defined in `rec_defs.h` ✅
- `REC_DEBOUNCE_MS` defined in `rec_defs.h` ✅
