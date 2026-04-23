/* test/test_ipc_producer.c — Phase 2 驗證：IPC Producer（Server 端）
 *
 * 功能：
 *   建立 vfr_server，持續產幀並 dispatch 給連線中的 consumer。
 *   在 /tmp/vfr_test_ipc.buf 寫入 magic number（驗收條件：zero-copy）。
 *
 * 用法：
 *   ./test_ipc_producer [--frames N] [--stream NAME]
 *
 * 驗收條件（見 plan Phase 2 Checklist）：
 *   - producer 在 dispatch 前於 frame buffer 起始位置寫入 0xDEADBEEF
 *   - consumer mmap 後讀到相同值，證明 zero-copy
 *   - producer 收到 SIGTERM，按釋放順序清理後退出
 *   - producer crash 後 consumer 不 hang（fd 被 kernel 關閉）
 */

#include "ipc/vfr_server.h"
#include "vfr_defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#define DEFAULT_STREAM   "test_ipc"
#define DEFAULT_FRAMES   50
#define FRAME_INTERVAL_NS  (33333333ULL)   /* ~30fps */

/* ─── Signal 處理（原則四：sigaction）────────────────────────────────────── */
static volatile sig_atomic_t g_running = 1;
static void sig_handler(int signo) { (void)signo; g_running = 0; }

static void setup_signals(void)
{
    struct sigaction sa = { .sa_handler = sig_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* 忽略 SIGPIPE：write 到 dead consumer 改由 sendmsg 回傳 EPIPE 處理 */
    struct sigaction sa_pipe = { .sa_handler = SIG_IGN };
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);
}

int main(int argc, char *argv[])
{
    setup_signals();

    const char *stream_name = DEFAULT_STREAM;
    int max_frames = DEFAULT_FRAMES;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc) {
            stream_name = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--inf") == 0) {
            max_frames = -1;   /* 無限模式 */
        }
    }

    printf("[PRODUCER] VFR Phase 2 IPC Producer\n");
    printf("[PRODUCER] stream='%s' max_frames=%d\n", stream_name,
           max_frames < 0 ? -1 : max_frames);
    printf("[PRODUCER] Platform: %s\n",
           getenv("VFR_PLATFORM") ? getenv("VFR_PLATFORM") : "mock");

    /* 建立 server */
    vfr_server_t *srv = vfr_server_create(stream_name, 0 /* default slots */);
    if (!srv) {
        fprintf(stderr, "[PRODUCER] vfr_server_create failed\n");
        return 1;
    }

    printf("[PRODUCER] Listening on \\0/vfr/%s (waiting for consumers...)\n", stream_name);

    int produced = 0;
    int frames_this_second = 0;
    struct timespec sec_start;
    clock_gettime(CLOCK_MONOTONIC, &sec_start);

    while (g_running && (max_frames < 0 || produced < max_frames)) {
        /* 處理 IPC 事件（新連線、release_msg）*/
        if (vfr_server_handle_events(srv, 0) < 0) {
            fprintf(stderr, "[PRODUCER] handle_events error\n");
            break;
        }

        /* 產幀 */
        int ret = vfr_server_produce(srv);
        if (ret == 0) {
            produced++;
            frames_this_second++;

            /* 每 10 幀印一次狀態 */
            if (produced % 10 == 0) {
                printf("[PRODUCER] produced %d frames\n", produced);
                fflush(stdout);
            }
        } else if (ret == 1) {
            /* 暫無幀（EAGAIN）：稍後重試 */
        } else {
            fprintf(stderr, "[PRODUCER] produce error\n");
            break;
        }

        /* 控制幀率（約 30fps，使用 nanosleep）*/
        struct timespec sleep_ts = {
            .tv_sec  = 0,
            .tv_nsec = (long)(FRAME_INTERVAL_NS / 2),  /* 16ms sleep */
        };
        nanosleep(&sleep_ts, NULL);
    }

    printf("[PRODUCER] Done. Produced %d frames. Cleaning up...\n", produced);

    /* 清理（原則五順序：server_destroy 內部按順序清理）*/
    vfr_server_destroy(&srv);

    if (srv != NULL) {
        fprintf(stderr, "[PRODUCER] FAIL: srv should be NULL after destroy\n");
        return 1;
    }

    printf("[PRODUCER] Exited cleanly.\n");
    return 0;
}
