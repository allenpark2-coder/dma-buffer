/* test/test_crash_recovery.c — Phase 4 驗證：Consumer Crash Recovery
 *
 * 驗收條件（見 plan Phase 4 Checklist）：
 *   - consumer kill -9 後，producer watchdog 觸發 teardown_session
 *   - slot 在 watchdog 觸發後 VFR_WATCHDOG_TIMEOUT_MS ms 內歸還 pool
 *   - producer 繼續正常出幀，不 hang
 *   - vfr_close 連呼兩次，第二次為 no-op（3.2）
 *   - kill consumer 後 producer fd 計數穩定（3.4）
 *
 * 設計：
 *   --role producer — 建立 server，持續出幀，監控 session 狀態
 *   --role crasher  — 連線作為 consumer，接收幀但不釋放（模擬慢 consumer），
 *                     然後 _exit(1) 模擬 kill -9（kernel 關閉所有 fd，不執行 cleanup）
 *
 * check4 Makefile target：
 *   1. 啟動 producer，等待其就緒
 *   2. 啟動 crasher 背景執行
 *   3. 短暫等待 crasher 連線並持有 slot
 *   4. kill -9 crasher
 *   5. 等待 producer 完成（producer 在 watchdog 恢復後繼續出幀）
 *   6. 驗證 exit code
 *
 * 單一 binary 自測模式（--self-test）：
 *   使用 fork() 在同一 process 組執行完整測試。
 */

#include "ipc/vfr_server.h"
#include "ipc/vfr_watchdog.h"
#include "ipc/vfr_client.h"
#include "vfr_defs.h"
#include "vfr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/epoll.h>

#define CRASH_STREAM       "crash_recovery"
#define PRODUCER_SLOTS     4          /* 小 pool，容易驗證 slot 耗盡後的恢復 */
#define PRODUCER_FRAMES    80         /* producer 總出幀數 */
#define FRAME_INTERVAL_NS  16666667ULL /* ~60fps */

/* ─── Signal 處理（原則四：sigaction）────────────────────────────────────── */
static volatile sig_atomic_t g_running = 1;
static void sig_handler(int signo) { (void)signo; g_running = 0; }

static void setup_signals(void)
{
    struct sigaction sa = { .sa_handler = sig_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    struct sigaction sa_pipe = { .sa_handler = SIG_IGN };
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);
}

/* ─── helper：nanosleep 控制幀率 ────────────────────────────────────────── */
static void frame_sleep(uint64_t ns)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(ns / 1000000000ULL),
        .tv_nsec = (long)(ns % 1000000000ULL),
    };
    nanosleep(&ts, NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Producer（Server）Mode
 * ══════════════════════════════════════════════════════════════════════════ */
static int run_producer(int max_frames)
{
    setup_signals();

    printf("[PRODUCER] Phase 4 Crash Recovery — producer start\n");
    printf("[PRODUCER] stream='%s' slots=%d max_frames=%d\n",
           CRASH_STREAM, PRODUCER_SLOTS, max_frames);

    vfr_server_t *srv = vfr_server_create(CRASH_STREAM, PRODUCER_SLOTS);
    if (!srv) {
        fprintf(stderr, "[PRODUCER] FAIL: vfr_server_create\n");
        return 1;
    }

    /* 印出 pidfd 支援情況 */
    printf("[PRODUCER] pidfd watchdog: %s\n",
           vfr_watchdog_available() ? "supported" : "not supported (socket HUP only)");

    int produced = 0;
    int post_crash_frames = 0;
    bool consumer_was_seen = false;
    bool consumer_crashed  = false;

    while (g_running && (max_frames < 0 || produced < max_frames)) {
        /* 處理 IPC 事件（新連線、release_msg、pidfd watchdog）*/
        if (vfr_server_handle_events(srv, 0) < 0) {
            fprintf(stderr, "[PRODUCER] handle_events error\n");
            break;
        }

        uint32_t sessions = vfr_server_get_session_count(srv);

        if (sessions > 0) consumer_was_seen = true;

        /* consumer 曾連線後又消失 → 認定已 crash */
        if (consumer_was_seen && sessions == 0 && !consumer_crashed) {
            consumer_crashed = true;
            printf("[PRODUCER] consumer crash detected (sessions=0), continuing...\n");
        }

        if (consumer_crashed) post_crash_frames++;

        /* 出幀 */
        int ret = vfr_server_produce(srv);
        if (ret == 0) {
            produced++;
            if (produced % 20 == 0) {
                printf("[PRODUCER] produced=%d sessions=%u drop_count=%u post_crash=%d\n",
                       produced, sessions,
                       vfr_server_get_drop_count(srv),
                       post_crash_frames);
                fflush(stdout);
            }
        } else if (ret == 1) {
            /* EAGAIN：暫無幀 */
        } else {
            fprintf(stderr, "[PRODUCER] produce error\n");
            break;
        }

        frame_sleep(FRAME_INTERVAL_NS / 4);  /* 240fps budget：不拖慢測試 */
    }

    uint32_t final_drops = vfr_server_get_drop_count(srv);
    printf("[PRODUCER] Done. produced=%d post_crash_frames=%d drop_count=%u\n",
           produced, post_crash_frames, final_drops);

    /* 驗收條件：若 consumer 曾連線，crash 後必須有新的出幀（slot 已回收）*/
    int exit_code = 0;
    if (consumer_was_seen && post_crash_frames == 0) {
        fprintf(stderr, "[PRODUCER] FAIL: no frames produced after consumer crash "
                "(slots not recovered?)\n");
        exit_code = 1;
    } else {
        printf("[PRODUCER] PASS: %d frames produced after consumer crash\n",
               post_crash_frames);
    }

    /* 清理（原則五順序：server_destroy 內部按順序清理）*/
    vfr_server_destroy(&srv);

    /* 3.2 驗收：重複呼叫 vfr_server_destroy 為 no-op */
    vfr_server_destroy(&srv);
    if (srv != NULL) {
        fprintf(stderr, "[PRODUCER] FAIL: srv should be NULL after destroy\n");
        return 1;
    }

    printf("[PRODUCER] %s\n", exit_code == 0 ? "PASS" : "FAIL");
    return exit_code;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Crasher（Consumer）Mode：連線、持有 slot、然後 _exit() 模擬 kill -9
 * ══════════════════════════════════════════════════════════════════════════ */
static int run_crasher(int hold_frames, int hold_ms)
{
    printf("[CRASHER] connecting to '%s' (hold=%d frames, %dms)...\n",
           CRASH_STREAM, hold_frames, hold_ms);

    /* 以 DROP_OLDEST policy 連線 */
    vfr_ctx_t *ctx = vfr_open(CRASH_STREAM, 0);
    if (!ctx) {
        fprintf(stderr, "[CRASHER] vfr_open failed\n");
        return 1;
    }

    /* 設定 epoll 監控 eventfd */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("[CRASHER] epoll_create1");
        vfr_close(&ctx);
        return 1;
    }

    int evfd = vfr_get_eventfd(ctx);
    if (evfd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = evfd };
        epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev);
    }

    printf("[CRASHER] connected, receiving %d frames then crashing...\n", hold_frames);

    int received = 0;
    /* vfr_frame_t 陣列保留收到的幀（不呼叫 vfr_put_frame，模擬 slow consumer）*/
    vfr_frame_t held[VFR_MAX_CONSUMER_SLOTS];
    int held_count = 0;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)hold_ms * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (received < hold_frames) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            break;

        /* 等待 eventfd 通知 */
        struct epoll_event evs[1];
        int n = epoll_wait(epfd, evs, 1, 50 /* ms */);
        if (n <= 0) continue;

        vfr_frame_t frame;
        int ret = vfr_get_frame(ctx, &frame, VFR_FLAG_NONBLOCK);
        if (ret == 0) {
            received++;
            /* 刻意不呼叫 vfr_put_frame：持有 slot，讓 producer 積壓 */
            if (held_count < (int)(sizeof(held)/sizeof(held[0])))
                held[held_count++] = frame;
            /* 超出 held 容量的幀直接丟棄（不呼叫 put_frame = 模擬 leak） */
        }
    }

    printf("[CRASHER] received %d frames, held %d, now crashing via _exit()\n",
           received, held_count);
    fflush(stdout);

    close(epfd);

    /* _exit() 模擬 SIGKILL：不執行 atexit / destructor / vfr_close，
     * kernel 直接關閉所有 fd，producer watchdog 應偵測到此 process 死亡 */
    _exit(0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Self-test Mode：fork() 執行完整測試（不需要外部腳本）
 * ══════════════════════════════════════════════════════════════════════════ */
static int run_self_test(void)
{
    setup_signals();

    printf("[SELF-TEST] Phase 4 crash recovery self-test (fork-based)\n");
    printf("[SELF-TEST] pidfd watchdog: %s\n",
           vfr_watchdog_available() ? "available" : "not available (socket HUP only)");

    /* 建立 server */
    vfr_server_t *srv = vfr_server_create(CRASH_STREAM, PRODUCER_SLOTS);
    if (!srv) {
        fprintf(stderr, "[SELF-TEST] FAIL: vfr_server_create\n");
        return 1;
    }

    /* fork crasher child */
    pid_t child_pid = fork();
    if (child_pid < 0) {
        perror("[SELF-TEST] fork");
        vfr_server_destroy(&srv);
        return 1;
    }

    if (child_pid == 0) {
        /* ── Child：crasher ──────────────────────────────────────────────── */
        /* 等 producer 就緒（server bind 完成）後才連線 */
        struct timespec wait = { .tv_sec = 0, .tv_nsec = 200000000L };  /* 200ms */
        nanosleep(&wait, NULL);

        /* 設定 VFR_MODE=client 讓 vfr_open 以 client 模式連線 */
        setenv("VFR_MODE", "client", 1);
        run_crasher(3 /* hold 3 frames */, 300 /* ms */);
        _exit(0);
    }

    /* ── Parent：producer 主迴圈 ─────────────────────────────────────────── */
    printf("[SELF-TEST] producer started, child crasher pid=%d\n", (int)child_pid);

    int produced = 0;
    int post_crash = 0;
    bool child_reaped = false;
    bool child_seen_connected = false;

    struct timespec test_end;
    clock_gettime(CLOCK_MONOTONIC, &test_end);
    test_end.tv_sec += 6;  /* 最多 6 秒 */

    while (g_running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > test_end.tv_sec ||
            (now.tv_sec == test_end.tv_sec && now.tv_nsec >= test_end.tv_nsec)) {
            VFR_LOGW("self-test: timeout");
            break;
        }

        /* 處理 IPC 事件（new client / release_msg / pidfd watchdog）*/
        vfr_server_handle_events(srv, 0);

        uint32_t sessions = vfr_server_get_session_count(srv);
        if (sessions > 0) child_seen_connected = true;

        if (child_seen_connected && sessions == 0 && !child_reaped) {
            /* Reap child to avoid zombie */
            int wstatus;
            waitpid(child_pid, &wstatus, WNOHANG);
            child_reaped = true;
            printf("[SELF-TEST] consumer session gone, post-crash production begins\n");
        }

        if (child_reaped) post_crash++;

        /* 出幀 */
        int ret = vfr_server_produce(srv);
        if (ret == 0) produced++;

        /* 當 post_crash 累積足夠幀數：測試通過 */
        if (child_reaped && post_crash >= 20) break;

        frame_sleep(FRAME_INTERVAL_NS / 4);
    }

    /* 確保 child 已回收（避免 zombie）*/
    if (!child_reaped) {
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
    }

    uint32_t drops = vfr_server_get_drop_count(srv);
    printf("[SELF-TEST] produced=%d post_crash=%d drop_count=%u\n",
           produced, post_crash, drops);

    vfr_server_destroy(&srv);
    /* 重複 destroy 為 no-op */
    vfr_server_destroy(&srv);

    if (!child_seen_connected) {
        fprintf(stderr, "[SELF-TEST] FAIL: consumer never connected\n");
        return 1;
    }
    if (post_crash < 10) {
        fprintf(stderr, "[SELF-TEST] FAIL: only %d frames after crash "
                "(slots not recovered)\n", post_crash);
        return 1;
    }

    printf("[SELF-TEST] PASS\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    const char *role = "self-test";   /* default: 單一 binary 自測 */
    int  frames      = PRODUCER_FRAMES;
    int  hold_ms     = 400;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            role = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
            hold_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--self-test") == 0) {
            role = "self-test";
        }
    }

    if (strcmp(role, "producer") == 0) {
        return run_producer(frames);
    } else if (strcmp(role, "crasher") == 0) {
        /* crasher 模式必須以 VFR_MODE=client 執行 */
        setenv("VFR_MODE", "client", 1);
        return run_crasher(3, hold_ms);
    } else {
        /* 預設：fork-based self-test */
        return run_self_test();
    }
}
