/* test/test_rec_state.c — Phase R2 驗收測試
 *
 * 驗收 Checklist：
 * [1] IDLE → EXTRACT_PRE → IN_EVENT：觸發後狀態正確轉移
 * [2] EXTRACT_PRE → WAIT_KEYFRAME：無可用 keyframe 時進入等待
 * [3] WAIT_KEYFRAME + IDR → IN_EVENT：首個 IDR 到來後開始錄影
 * [4] WAIT_KEYFRAME + STOP / schedule off / manual stop → IDLE
 * [5] POST_WAIT → IN_EVENT：延時期間收到新觸發，正確回歸
 * [6] POST_WAIT → IDLE：倒數結束後無新觸發，回到待命
 * [7] 排程關閉時段內，TRIGGER_START 被靜默忽略
 * [8] timerfd 驅動倒數，正確消耗 expiration count
 *
 * 注意：此測試不進行任何檔案寫入（Phase R2 = 無寫入，僅 Log）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include "rec_defs.h"
#include "rec_buf.h"
#include "rec_state.h"
#include "rec_schedule.h"
#include "rec_debounce.h"
#include "rec_trigger.h"

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

/* ─── 狀態轉移記錄 ───────────────────────────────────────────────── */
#define MAX_TRANS 32
static rec_state_t g_from_log[MAX_TRANS];
static rec_state_t g_to_log[MAX_TRANS];
static int g_trans_n = 0;

typedef struct {
    int                count;
    rec_trigger_type_t type;
    uint64_t           timestamp_ns;
    float              confidence;
    char               label[32];
} trigger_cb_log_t;

static trigger_cb_log_t g_trigger_log;

static const char *state_name(rec_state_t s)
{
    switch (s) {
    case REC_STATE_IDLE:          return "IDLE";
    case REC_STATE_EXTRACT_PRE:   return "EXTRACT_PRE";
    case REC_STATE_WAIT_KEYFRAME: return "WAIT_KEYFRAME";
    case REC_STATE_IN_EVENT:      return "IN_EVENT";
    case REC_STATE_POST_WAIT:     return "POST_WAIT";
    default:                      return "?";
    }
}

static void record_transition(void *ctx, rec_state_t from, rec_state_t to)
{
    (void)ctx;
    printf("    [→] %s → %s\n", state_name(from), state_name(to));
    if (g_trans_n < MAX_TRANS) {
        g_from_log[g_trans_n] = from;
        g_to_log[g_trans_n]   = to;
        g_trans_n++;
    }
}

static void clear_trans(void) { g_trans_n = 0; }

static void clear_trigger_log(void)
{
    memset(&g_trigger_log, 0, sizeof(g_trigger_log));
}

/* ─── 輔助：找到第 k 次轉移是否符合（0-based）─────────────────── */
static bool check_trans(int k, rec_state_t from, rec_state_t to)
{
    return (k < g_trans_n &&
            g_from_log[k] == from &&
            g_to_log[k]   == to);
}

/* ─── 輔助：將幀推入 rec_buf ──────────────────────────────────────── */
static void push_frames(rec_buf_t *buf,
                         uint32_t   n,
                         uint32_t   frame_sz,
                         uint64_t   base_ns,
                         uint32_t   kf_interval)
{
    uint8_t *data = malloc(frame_sz);
    if (!data) return;
    for (uint32_t i = 0; i < n; i++) {
        memset(data, (uint8_t)(i & 0xFF), frame_sz);
        rec_buf_push(buf, data, frame_sz,
                      base_ns + (uint64_t)i * 33333333ULL,
                      i,
                      (kf_interval > 0 && i % kf_interval == 0));
    }
    free(data);
}

/* ─── 輔助：建立預設組態（EVENT 模式，排程全允許）────────────────── */
static rec_state_config_t make_config_event(uint32_t pre_sec, uint32_t post_sec)
{
    rec_state_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode            = REC_MODE_EVENT;
    cfg.pre_record_sec  = pre_sec;
    cfg.post_record_sec = post_sec;
    rec_schedule_set_all(&cfg.schedule, REC_SLOT_EVENT);
    return cfg;
}

/* ─── 時間輔助：CLOCK_MONOTONIC ns ────────────────────────────────── */
static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void on_trigger_message(void *ctx,
                               rec_trigger_type_t type,
                               uint64_t timestamp_ns,
                               float confidence,
                               const char *label)
{
    (void)ctx;
    g_trigger_log.count++;
    g_trigger_log.type = type;
    g_trigger_log.timestamp_ns = timestamp_ns;
    g_trigger_log.confidence = confidence;
    snprintf(g_trigger_log.label, sizeof(g_trigger_log.label), "%s", label);
}

static int send_trigger_message(const char *stream_name,
                                const vfr_event_msg_t *msg)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    int path_len = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                            "/vfr/event/%s", stream_name);
    if (path_len <= 0 || path_len >= (int)(sizeof(addr.sun_path) - 1)) {
        close(fd);
        return -1;
    }

    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                     + 1 + (size_t)path_len);

    if (connect(fd, (struct sockaddr *)&addr, addr_len) < 0) {
        close(fd);
        return -1;
    }

    size_t total = 0;
    const uint8_t *src = (const uint8_t *)msg;
    while (total < sizeof(*msg)) {
        ssize_t sent = send(fd, src + total, sizeof(*msg) - total, 0);
        if (sent <= 0) {
            close(fd);
            return -1;
        }
        total += (size_t)sent;
    }
    close(fd);
    return 0;
}

/* ─── Test 1：IDLE → EXTRACT_PRE → IN_EVENT ─────────────────────── */
static void test_trigger_start_to_in_event(void)
{
    TEST_BEGIN("IDLE → EXTRACT_PRE → IN_EVENT via TRIGGER_START");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 10ULL * 1000000000ULL;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    /* 推入 90 幀，每 30 幀一個 keyframe */
    push_frames(buf, 90, FRAME_SZ, BASE_NS, 30);

    rec_state_config_t cfg = make_config_event(5, 10);
    clear_trans();

    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                 record_transition, NULL);
    CHECK(sm != NULL, "rec_state_create ok");
    CHECK(rec_state_get(sm) == REC_STATE_IDLE, "initial state is IDLE");

    uint64_t now_ns = BASE_NS + 90ULL * 33333333ULL;  /* 幀結束後 */

    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));

    CHECK(g_trans_n >= 2, "at least 2 transitions occurred");
    CHECK(check_trans(0, REC_STATE_IDLE, REC_STATE_EXTRACT_PRE),
          "transition 0: IDLE → EXTRACT_PRE");
    CHECK(check_trans(1, REC_STATE_EXTRACT_PRE, REC_STATE_IN_EVENT),
          "transition 1: EXTRACT_PRE → IN_EVENT");
    CHECK(rec_state_get(sm) == REC_STATE_IN_EVENT,
          "final state is IN_EVENT");

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
    CHECK(sm == NULL, "sm = NULL after destroy");
}

/* ─── Test 2：EXTRACT_PRE → WAIT_KEYFRAME（無 keyframe）────────── */
static void test_no_keyframe_wait(void)
{
    TEST_BEGIN("EXTRACT_PRE → WAIT_KEYFRAME: no keyframe in buffer");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 512;
    const uint64_t BASE_NS  = 0;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    /* 只推入 P-frame（kf_interval = 0 → 無 keyframe）*/
    push_frames(buf, 30, FRAME_SZ, BASE_NS, 0);

    rec_state_config_t cfg = make_config_event(5, 10);
    clear_trans();

    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                 record_transition, NULL);
    CHECK(sm != NULL, "rec_state_create ok");

    uint64_t now_ns = BASE_NS + 30ULL * 33333333ULL;
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));

    CHECK(g_trans_n >= 2, "at least 2 transitions");
    CHECK(check_trans(0, REC_STATE_IDLE, REC_STATE_EXTRACT_PRE),
          "transition 0: IDLE → EXTRACT_PRE");
    CHECK(check_trans(1, REC_STATE_EXTRACT_PRE, REC_STATE_WAIT_KEYFRAME),
          "transition 1: EXTRACT_PRE → WAIT_KEYFRAME");
    CHECK(rec_state_get(sm) == REC_STATE_WAIT_KEYFRAME,
          "final state is WAIT_KEYFRAME");

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
}

/* ─── Test 3：WAIT_KEYFRAME + IDR → IN_EVENT ─────────────────────── */
static void test_wait_keyframe_idr(void)
{
    TEST_BEGIN("WAIT_KEYFRAME + IDR frame → IN_EVENT");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 512;
    const uint64_t BASE_NS  = 0;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");

    /* 無 keyframe → 觸發後進入 WAIT_KEYFRAME */
    push_frames(buf, 10, FRAME_SZ, BASE_NS, 0);

    rec_state_config_t cfg = make_config_event(5, 10);
    clear_trans();

    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                 record_transition, NULL);
    CHECK(sm != NULL, "rec_state_create ok");

    uint64_t now_ns = BASE_NS + 10ULL * 33333333ULL;
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_WAIT_KEYFRAME, "in WAIT_KEYFRAME");

    /* 清除記錄，再測試 IDR 到來 */
    clear_trans();
    bool transitioned = rec_state_on_keyframe(sm);

    CHECK(transitioned, "rec_state_on_keyframe returns true");
    CHECK(g_trans_n == 1, "exactly 1 transition");
    CHECK(check_trans(0, REC_STATE_WAIT_KEYFRAME, REC_STATE_IN_EVENT),
          "transition: WAIT_KEYFRAME → IN_EVENT");
    CHECK(rec_state_get(sm) == REC_STATE_IN_EVENT, "final state is IN_EVENT");

    /* IDR 在非 WAIT_KEYFRAME 狀態應是 no-op */
    clear_trans();
    bool no_op = rec_state_on_keyframe(sm);
    CHECK(!no_op, "on_keyframe in IN_EVENT is no-op");
    CHECK(g_trans_n == 0, "no transitions from IN_EVENT on keyframe");

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
}

/* ─── Test 4：WAIT_KEYFRAME + STOP / force_idle → IDLE ─────────── */
static void test_wait_keyframe_stop(void)
{
    TEST_BEGIN("WAIT_KEYFRAME + TRIGGER_STOP / force_idle → IDLE");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 512;
    const uint64_t BASE_NS  = 0;

    /* ── Sub-test 4a：TRIGGER_STOP ──────────────────────────────── */
    {
        rec_buf_t *buf = rec_buf_create(RING_SZ);
        push_frames(buf, 10, FRAME_SZ, BASE_NS, 0);  /* no keyframe */

        rec_state_config_t cfg = make_config_event(5, 10);
        clear_trans();
        rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                     record_transition, NULL);

        uint64_t now_ns = BASE_NS + 10ULL * 33333333ULL;
        rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
        CHECK(rec_state_get(sm) == REC_STATE_WAIT_KEYFRAME, "4a: in WAIT_KEYFRAME");

        clear_trans();
        /* STOP 消抖視窗已過（now_ns + 600ms）*/
        uint64_t stop_ns = now_ns + 600ULL * 1000000ULL;
        rec_state_on_trigger(sm, REC_TRIGGER_STOP, stop_ns, time(NULL));

        CHECK(rec_state_get(sm) == REC_STATE_IDLE, "4a: WAIT_KEYFRAME + STOP → IDLE");
        CHECK(check_trans(0, REC_STATE_WAIT_KEYFRAME, REC_STATE_IDLE),
              "4a: transition WAIT_KEYFRAME → IDLE (not POST_WAIT)");

        rec_state_destroy(&sm);
        rec_buf_destroy(&buf);
    }

    /* ── Sub-test 4b：force_idle（manual stop）──────────────────── */
    {
        rec_buf_t *buf = rec_buf_create(RING_SZ);
        push_frames(buf, 10, FRAME_SZ, BASE_NS, 0);

        rec_state_config_t cfg = make_config_event(5, 10);
        clear_trans();
        rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                     record_transition, NULL);

        uint64_t now_ns = BASE_NS + 10ULL * 33333333ULL;
        rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
        CHECK(rec_state_get(sm) == REC_STATE_WAIT_KEYFRAME, "4b: in WAIT_KEYFRAME");

        clear_trans();
        rec_state_force_idle(sm);
        CHECK(rec_state_get(sm) == REC_STATE_IDLE, "4b: force_idle → IDLE");
        CHECK(g_trans_n == 1, "4b: exactly 1 transition");

        rec_state_destroy(&sm);
        rec_buf_destroy(&buf);
    }

    /* ── Sub-test 4c：schedule off via on_timer ─────────────────── */
    {
        rec_buf_t *buf = rec_buf_create(RING_SZ);
        push_frames(buf, 10, FRAME_SZ, BASE_NS, 0);

        rec_state_config_t cfg = make_config_event(5, 10);
        clear_trans();
        rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                     record_transition, NULL);

        uint64_t now_ns = BASE_NS + 10ULL * 33333333ULL;
        rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
        CHECK(rec_state_get(sm) == REC_STATE_WAIT_KEYFRAME, "4c: in WAIT_KEYFRAME");

        /* 更新排程為全 OFF，current_slot 仍是 EVENT → 下次 on_timer 偵測到變更 */
        rec_schedule_t off_sched;
        rec_schedule_set_all(&off_sched, REC_SLOT_OFF);
        rec_state_set_schedule(sm, &off_sched);

        clear_trans();
        /* 傳 now_wall=1 (epoch+1s)，此時所有 slot 都是 OFF */
        rec_state_on_timer(sm, 1, false, (time_t)1);
        CHECK(rec_state_get(sm) == REC_STATE_IDLE,
              "4c: WAIT_KEYFRAME + schedule off → IDLE");

        rec_state_destroy(&sm);
        rec_buf_destroy(&buf);
    }
}

/* ─── Test 5：POST_WAIT → IN_EVENT（新觸發）─────────────────────── */
static void test_post_wait_retrigger(void)
{
    TEST_BEGIN("POST_WAIT → IN_EVENT: new trigger during post-wait");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 10ULL * 1000000000ULL;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");
    push_frames(buf, 90, FRAME_SZ, BASE_NS, 30);

    rec_state_config_t cfg = make_config_event(5, 30);
    clear_trans();
    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                 record_transition, NULL);
    CHECK(sm != NULL, "rec_state_create ok");

    uint64_t now_ns = BASE_NS + 90ULL * 33333333ULL;

    /* 1. TRIGGER_START → IN_EVENT */
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_IN_EVENT, "reached IN_EVENT");

    /* 2. TRIGGER_STOP → POST_WAIT */
    uint64_t stop_ns = now_ns + 600ULL * 1000000ULL;  /* +600ms */
    rec_state_on_trigger(sm, REC_TRIGGER_STOP, stop_ns, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_POST_WAIT, "reached POST_WAIT");
    CHECK(rec_state_get_post_remaining(sm) == 30, "post_remaining = 30s");

    /* 3. TRIGGER_START 再次到來（+600ms 後，通過消抖）→ IN_EVENT */
    clear_trans();
    uint64_t retrig_ns = stop_ns + 600ULL * 1000000ULL;
    rec_state_on_trigger(sm, REC_TRIGGER_START, retrig_ns, time(NULL));

    CHECK(g_trans_n == 1, "exactly 1 transition on re-trigger");
    CHECK(check_trans(0, REC_STATE_POST_WAIT, REC_STATE_IN_EVENT),
          "POST_WAIT → IN_EVENT on new trigger");
    CHECK(rec_state_get(sm) == REC_STATE_IN_EVENT, "final state is IN_EVENT");

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
}

/* ─── Test 6：POST_WAIT → IDLE（倒數歸零）───────────────────────── */
static void test_post_wait_countdown(void)
{
    TEST_BEGIN("POST_WAIT → IDLE: countdown expires");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 10ULL * 1000000000ULL;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    push_frames(buf, 90, FRAME_SZ, BASE_NS, 30);

    /* post_record_sec = 3s */
    rec_state_config_t cfg = make_config_event(5, 3);
    clear_trans();
    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                 record_transition, NULL);

    uint64_t now_ns = BASE_NS + 90ULL * 33333333ULL;

    /* Start → IN_EVENT */
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_IN_EVENT, "in IN_EVENT");

    /* Stop → POST_WAIT */
    uint64_t stop_ns = now_ns + 600ULL * 1000000ULL;
    rec_state_on_trigger(sm, REC_TRIGGER_STOP, stop_ns, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_POST_WAIT, "in POST_WAIT");
    CHECK(rec_state_get_post_remaining(sm) == 3, "remaining = 3s");

    /* Timer tick 1 → remaining = 2, still POST_WAIT */
    clear_trans();
    rec_state_on_timer(sm, 1, false, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_POST_WAIT, "after tick 1: still POST_WAIT");
    CHECK(rec_state_get_post_remaining(sm) == 2, "remaining = 2s");
    CHECK(g_trans_n == 0, "no transition after tick 1");

    /* Timer tick 1 → remaining = 1, still POST_WAIT */
    rec_state_on_timer(sm, 1, false, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_POST_WAIT, "after tick 2: still POST_WAIT");
    CHECK(rec_state_get_post_remaining(sm) == 1, "remaining = 1s");

    /* Timer tick 1 → remaining = 0 → IDLE */
    clear_trans();
    rec_state_on_timer(sm, 1, false, time(NULL));
    CHECK(g_trans_n == 1, "1 transition after final tick");
    CHECK(check_trans(0, REC_STATE_POST_WAIT, REC_STATE_IDLE),
          "POST_WAIT → IDLE on countdown");
    CHECK(rec_state_get(sm) == REC_STATE_IDLE, "final state is IDLE");

    /* Test: 多餘 expiration 一次消耗（expirations = 5, remaining = 3）*/
    printf("\n  [sub] bulk expiration: expirations=5 with remaining=3\n");
    rec_buf_t *buf2 = rec_buf_create(RING_SZ);
    push_frames(buf2, 90, FRAME_SZ, BASE_NS, 30);
    rec_state_config_t cfg2 = make_config_event(5, 3);
    rec_state_machine_t *sm2 = rec_state_create(&cfg2, buf2, -1, record_transition, NULL);

    uint64_t now2 = BASE_NS + 90ULL * 33333333ULL;
    rec_state_on_trigger(sm2, REC_TRIGGER_START, now2, time(NULL));
    uint64_t stop2 = now2 + 600ULL * 1000000ULL;
    rec_state_on_trigger(sm2, REC_TRIGGER_STOP, stop2, time(NULL));
    CHECK(rec_state_get(sm2) == REC_STATE_POST_WAIT, "sub: in POST_WAIT");

    clear_trans();
    rec_state_on_timer(sm2, 5, false, time(NULL));   /* expirations = 5 > remaining = 3 */
    CHECK(rec_state_get(sm2) == REC_STATE_IDLE,
          "sub: bulk expiration (5) collapses remaining (3) → IDLE");

    rec_state_destroy(&sm2);
    rec_buf_destroy(&buf2);

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
}

/* ─── Test 7：排程關閉時段內 TRIGGER_START 被靜默忽略 ────────────── */
static void test_schedule_off_blocks_trigger(void)
{
    TEST_BEGIN("Schedule OFF: TRIGGER_START silently ignored");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 10ULL * 1000000000ULL;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    push_frames(buf, 90, FRAME_SZ, BASE_NS, 30);

    /* 排程：全 OFF */
    rec_state_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode            = REC_MODE_SCHEDULED;
    cfg.pre_record_sec  = 5;
    cfg.post_record_sec = 10;
    rec_schedule_set_all(&cfg.schedule, REC_SLOT_OFF);

    clear_trans();
    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                 record_transition, NULL);
    CHECK(sm != NULL, "rec_state_create ok");
    CHECK(rec_state_get(sm) == REC_STATE_IDLE, "initial state IDLE");

    uint64_t now_ns = BASE_NS + 90ULL * 33333333ULL;

    /* TRIGGER_START 應被靜默忽略 */
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));

    CHECK(g_trans_n == 0, "no transitions occurred (schedule is OFF)");
    CHECK(rec_state_get(sm) == REC_STATE_IDLE,
          "state remains IDLE when schedule is OFF");

    /* 切換排程為全 EVENT 後，觸發應被接受 */
    rec_schedule_t on_sched;
    rec_schedule_set_all(&on_sched, REC_SLOT_EVENT);
    rec_state_set_schedule(sm, &on_sched);

    clear_trans();
    uint64_t now2 = now_ns + 600ULL * 1000000ULL;  /* 通過消抖 */
    rec_state_on_trigger(sm, REC_TRIGGER_START, now2, time(NULL));

    CHECK(g_trans_n >= 1, "transitions occurred after schedule enabled");
    CHECK(rec_state_get(sm) != REC_STATE_IDLE,
          "state changed from IDLE after schedule on");

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
}

/* ─── Test 8：timerfd 驅動倒數，正確消耗 expiration count ────────── */
static void test_timerfd_countdown(void)
{
    TEST_BEGIN("timerfd-driven countdown: expirations consumed correctly");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 10ULL * 1000000000ULL;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    push_frames(buf, 90, FRAME_SZ, BASE_NS, 30);

    /* post_record_sec = 1s，用真實 timerfd 驗證 */
    rec_state_config_t cfg = make_config_event(5, 1);
    clear_trans();
    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                                 record_transition, NULL);
    CHECK(sm != NULL, "rec_state_create ok");

    uint64_t now_ns = BASE_NS + 90ULL * 33333333ULL;

    /* → IN_EVENT */
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_IN_EVENT, "in IN_EVENT");

    /* → POST_WAIT */
    uint64_t stop_ns = now_ns + 600ULL * 1000000ULL;
    rec_state_on_trigger(sm, REC_TRIGGER_STOP, stop_ns, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_POST_WAIT, "in POST_WAIT");

    /* 建立真實 timerfd：每 200ms 觸發一次 */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    CHECK(tfd >= 0, "timerfd_create ok");

    struct itimerspec its;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = 200000000L;  /* 200ms */
    its.it_value.tv_sec     = 0;
    its.it_value.tv_nsec    = 200000000L;  /* 首次 200ms 後觸發 */
    timerfd_settime(tfd, 0, &its, NULL);

    /* 等待至少 1 個 expiration（約 250ms 後讀取）*/
    struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 250000000L };
    nanosleep(&sleep_ts, NULL);

    uint64_t expirations = 0;
    ssize_t n = read(tfd, &expirations, sizeof(expirations));
    close(tfd);

    CHECK(n == sizeof(expirations), "timerfd read ok");
    CHECK(expirations >= 1, "at least 1 expiration read");

    clear_trans();
    rec_state_on_timer(sm, expirations, false, time(NULL));

    /*
     * post_record_sec = 1，expirations >= 1 → remaining <= 0 → IDLE。
     * 若 expirations = 1: remaining = 1 - 1 = 0 → IDLE。
     */
    CHECK(rec_state_get(sm) == REC_STATE_IDLE,
          "timerfd expiration drives countdown to IDLE");

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
}

/* ─── Test 9：消抖器單元測試 ─────────────────────────────────────── */
static void test_debounce_unit(void)
{
    TEST_BEGIN("rec_debounce: START debounce, STOP anti-glitch");

    rec_debounce_t d;
    rec_debounce_init(&d, REC_DEBOUNCE_MS);   /* 500ms */

    const uint64_t WIN = (uint64_t)REC_DEBOUNCE_MS * 1000000ULL;  /* 500ms in ns */

    /* 第一次 START 應被允許 */
    uint64_t t0 = 1000000000ULL;
    CHECK(rec_debounce_allow_start(&d, t0), "first START allowed");

    /* 200ms 內重複 → 消抖，拒絕 */
    CHECK(!rec_debounce_allow_start(&d, t0 + 200000000ULL),
          "START within 200ms debounced");

    /* 600ms 後 → 通過 */
    CHECK(rec_debounce_allow_start(&d, t0 + WIN + 100000000ULL),
          "START after window passed allowed");

    /* STOP 在新的 START 後 200ms → 消抖 */
    uint64_t t1 = t0 + WIN + 100000000ULL;  /* 上面的 START 時間 */
    CHECK(!rec_debounce_allow_stop(&d, t1 + 200000000ULL),
          "STOP within 200ms of START: anti-glitch");

    /* STOP 在 600ms 後 → 允許 */
    CHECK(rec_debounce_allow_stop(&d, t1 + WIN + 100000000ULL),
          "STOP after window passed allowed");
}

/* ─── Test 10：rec_schedule 單元測試 ────────────────────────────── */
static void test_schedule_unit(void)
{
    TEST_BEGIN("rec_schedule: set_all + query");

    rec_schedule_t sched;

    /* 全 EVENT → 任意時刻查詢均為 EVENT */
    rec_schedule_set_all(&sched, REC_SLOT_EVENT);
    time_t t = time(NULL);
    CHECK(rec_schedule_query(&sched, t) == REC_SLOT_EVENT,
          "all-EVENT schedule: query returns EVENT");

    /* 全 OFF */
    rec_schedule_set_all(&sched, REC_SLOT_OFF);
    CHECK(rec_schedule_query(&sched, t) == REC_SLOT_OFF,
          "all-OFF schedule: query returns OFF");

    /* 全 CONTINUOUS */
    rec_schedule_set_all(&sched, REC_SLOT_CONTINUOUS);
    CHECK(rec_schedule_query(&sched, t) == REC_SLOT_CONTINUOUS,
          "all-CONTINUOUS schedule: query returns CONTINUOUS");
}

static void test_trigger_socket_roundtrip(void)
{
    TEST_BEGIN("rec_trigger: abstract socket round-trip");

    rec_trigger_t *trig = rec_trigger_create("cam0", on_trigger_message, NULL);
    CHECK(trig != NULL, "rec_trigger_create ok");
    CHECK(rec_trigger_get_fd(trig) >= 0, "rec_trigger_get_fd returns listen fd");

    vfr_event_msg_t msg1;
    memset(&msg1, 0, sizeof(msg1));
    msg1.magic = VFR_EVENT_MAGIC;
    msg1.event_type = REC_TRIGGER_START;
    msg1.timestamp_ns = 123456789ULL;
    msg1.confidence = 0.75f;
    snprintf(msg1.stream_name, sizeof(msg1.stream_name), "%s", "cam0");
    snprintf(msg1.label, sizeof(msg1.label), "%s", "person");

    vfr_event_msg_t msg2 = msg1;
    msg2.event_type = REC_TRIGGER_STOP;
    msg2.timestamp_ns = 223456789ULL;
    snprintf(msg2.label, sizeof(msg2.label), "%s", "vehicle");

    clear_trigger_log();
    CHECK(send_trigger_message("cam0", &msg1) == 0, "send START event");
    CHECK(send_trigger_message("cam0", &msg2) == 0, "send STOP event");
    CHECK(rec_trigger_handle_readable(trig) == 0, "drain trigger socket");
    CHECK(g_trigger_log.count == 2, "all queued events delivered");
    CHECK(g_trigger_log.type == REC_TRIGGER_STOP, "last delivered type is STOP");
    CHECK(g_trigger_log.timestamp_ns == msg2.timestamp_ns, "STOP timestamp delivered");
    CHECK(strcmp(g_trigger_log.label, "vehicle") == 0, "STOP label delivered");

    vfr_event_msg_t bad_magic = msg1;
    bad_magic.magic = 0;
    clear_trigger_log();
    CHECK(send_trigger_message("cam0", &bad_magic) == 0, "send invalid magic event");
    CHECK(rec_trigger_handle_readable(trig) == 0, "drain invalid magic event");
    CHECK(g_trigger_log.count == 0, "invalid magic ignored");

    vfr_event_msg_t wrong_stream = msg1;
    snprintf(wrong_stream.stream_name, sizeof(wrong_stream.stream_name), "%s", "cam1");
    clear_trigger_log();
    CHECK(send_trigger_message("cam0", &wrong_stream) == 0, "send mismatched stream payload");
    CHECK(rec_trigger_handle_readable(trig) == 0, "drain mismatched stream payload");
    CHECK(g_trigger_log.count == 0, "mismatched stream ignored");

    rec_trigger_destroy(&trig);
    CHECK(trig == NULL, "rec_trigger_destroy clears pointer");
}

static void test_force_idle_aborts_active_preroll(void)
{
    TEST_BEGIN("force_idle aborts active pre-roll batch");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 10ULL * 1000000000ULL;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");
    push_frames(buf, 90, FRAME_SZ, BASE_NS, 30);

    rec_state_config_t cfg = make_config_event(5, 10);
    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                               record_transition, NULL);
    CHECK(sm != NULL, "rec_state_create ok");

    uint64_t now_ns = BASE_NS + 90ULL * 33333333ULL;
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_IN_EVENT, "entered IN_EVENT");

    uint32_t active_gen = atomic_load_explicit(&buf->protected_gen, memory_order_acquire);
    CHECK(active_gen != REC_PRE_GEN_NONE, "pre-roll protect is active");

    rec_state_force_idle(sm);
    CHECK(rec_state_get(sm) == REC_STATE_IDLE, "force_idle transitions to IDLE");
    CHECK(atomic_load_explicit(&buf->protected_gen, memory_order_acquire) == REC_PRE_GEN_NONE,
          "force_idle clears protect window");
    CHECK(atomic_load_explicit(&buf->aborted_pre_gen, memory_order_acquire) == active_gen,
          "force_idle marks batch aborted");

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
}

static void test_post_wait_pending_trigger(void)
{
    TEST_BEGIN("POST_WAIT expiry honors pending trigger");

    const uint32_t RING_SZ  = 4 * 1024 * 1024;
    const uint32_t FRAME_SZ = 1024;
    const uint64_t BASE_NS  = 10ULL * 1000000000ULL;

    rec_buf_t *buf = rec_buf_create(RING_SZ);
    CHECK(buf != NULL, "rec_buf_create ok");
    push_frames(buf, 90, FRAME_SZ, BASE_NS, 30);

    rec_state_config_t cfg = make_config_event(5, 1);
    clear_trans();
    rec_state_machine_t *sm = rec_state_create(&cfg, buf, -1,
                                               record_transition, NULL);
    CHECK(sm != NULL, "rec_state_create ok");

    uint64_t now_ns = BASE_NS + 90ULL * 33333333ULL;
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns, time(NULL));
    rec_state_on_trigger(sm, REC_TRIGGER_STOP, now_ns + 600ULL * 1000000ULL, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_POST_WAIT, "entered POST_WAIT");

    clear_trans();
    rec_state_on_timer(sm, 1, true, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_POST_WAIT,
          "pending trigger keeps state in POST_WAIT");
    CHECK(rec_state_get_post_remaining(sm) == 0,
          "remaining time clamps at zero");
    CHECK(g_trans_n == 0, "no IDLE transition while trigger is pending");

    clear_trans();
    rec_state_on_trigger(sm, REC_TRIGGER_START, now_ns + 1200ULL * 1000000ULL, time(NULL));
    CHECK(rec_state_get(sm) == REC_STATE_IN_EVENT, "pending trigger resumes IN_EVENT");
    CHECK(check_trans(0, REC_STATE_POST_WAIT, REC_STATE_IN_EVENT),
          "transition stays POST_WAIT -> IN_EVENT");

    rec_state_destroy(&sm);
    rec_buf_destroy(&buf);
}

/* ─── main ───────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== Phase R2: rec_state unit tests ===\n");

    test_trigger_start_to_in_event();
    test_no_keyframe_wait();
    test_wait_keyframe_idr();
    test_wait_keyframe_stop();
    test_post_wait_retrigger();
    test_post_wait_countdown();
    test_schedule_off_blocks_trigger();
    test_timerfd_countdown();
    test_debounce_unit();
    test_schedule_unit();
    test_trigger_socket_roundtrip();
    test_force_idle_aborts_active_preroll();
    test_post_wait_pending_trigger();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
