/* test/test_ipc_consumer.c — Phase 2 驗證：IPC Consumer（Client 端）
 *
 * 功能：
 *   連線到 vfr_server，接收 N 幀，驗證 zero-copy（magic number）。
 *   設定 VFR_MODE=client 以啟用 IPC client 模式。
 *
 * 用法：
 *   VFR_MODE=client ./test_ipc_consumer [--stream NAME] [--frames N]
 *
 * 驗收條件（見 plan Phase 2 Checklist）：
 *   3.1 Build  : make test_ipc 獨立編譯
 *   3.2 magic  : producer 在 buf[0..3] 寫入 0xDEADBEEF，consumer 讀到相同值
 *   3.4 fd leak: /proc/PID/fd 計數在 consumer 斷線後恢復
 *   3.5 kill -9: kill consumer，producer 繼續正常出幀
 *   3.6 EPIPE  : 連線異常有 __func__:__LINE__ log
 */

#include "vfr.h"
#include "vfr_defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dirent.h>

#define DEFAULT_STREAM   "test_ipc"
#define DEFAULT_FRAMES   20
#define ZERO_COPY_MAGIC  0xDEADBEEFu

/* ─── Signal 處理 ─────────────────────────────────────────────────────────── */
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

/* ─── 計算 /proc/self/fd 中的 fd 數量 ─────────────────────────────────────── */
static int count_open_fds(void)
{
    int count = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] != '.') count++;
    }
    closedir(d);
    return count;
}

/* ─── test_ipc_recv：接收幀並驗證 ─────────────────────────────────────────── */
static int test_ipc_recv(const char *stream_name, int max_frames)
{
    printf("\n=== test_ipc_recv: stream='%s' frames=%d ===\n", stream_name, max_frames);

    /* 記錄開始前 fd 數量（用於驗收 3.4 fd leak）*/
    int fd_before = count_open_fds();

    vfr_ctx_t *ctx = vfr_open(stream_name, 0);
    if (!ctx) {
        fprintf(stderr, "[CONSUMER] vfr_open('%s') failed: %s\n",
                stream_name, strerror(errno));
        return -1;
    }

    int fd_after_open = count_open_fds();
    printf("[CONSUMER] fd count: before=%d after_open=%d (delta=%d)\n",
           fd_before, fd_after_open, fd_after_open - fd_before);

    int received   = 0;
    int failed     = 0;
    int zero_copy_ok = 0;

    while (g_running && (max_frames < 0 || received < max_frames)) {
        vfr_frame_t frame = {0};
        frame.dma_fd = -1;

        int ret = vfr_get_frame(ctx, &frame, 0);
        if (ret != 0) {
            fprintf(stderr, "[CONSUMER] vfr_get_frame[%d] failed\n", received);
            failed = 1;
            break;
        }

        /* ── Zero-copy 驗收：mmap 並讀取 magic number ── */
        void *ptr = vfr_map(&frame);
        if (ptr) {
            uint32_t magic = *(volatile uint32_t *)ptr;
            if (magic == ZERO_COPY_MAGIC) {
                zero_copy_ok++;
                if (received == 0) {
                    printf("[CONSUMER] frame[0]: zero-copy magic 0x%08X verified!\n", magic);
                }
            }
            /* 不印每幀（高頻）；只印前幾幀 */
            if (received < 3) {
                printf("[CONSUMER] frame[%d]: seq=%llu dma_fd=%d %ux%u buf_size=%u magic=0x%08X\n",
                       received,
                       (unsigned long long)frame.seq_num,
                       frame.dma_fd,
                       frame.width, frame.height,
                       frame.buf_size,
                       magic);
            }
            vfr_unmap(&frame, ptr);
        } else {
            printf("[CONSUMER] frame[%d]: seq=%llu dma_fd=%d %ux%u (no map)\n",
                   received,
                   (unsigned long long)frame.seq_num,
                   frame.dma_fd,
                   frame.width, frame.height);
        }

        vfr_put_frame(&frame);

        /* 驗證 put_frame 後 dma_fd == -1 */
        if (frame.dma_fd != -1) {
            fprintf(stderr, "[CONSUMER] FAIL: dma_fd should be -1 after put_frame\n");
            failed = 1;
        }

        received++;
    }

    vfr_close(&ctx);

    /* fd 洩漏驗證 */
    int fd_after_close = count_open_fds();
    int fd_leaked = fd_after_close - fd_before;
    printf("[CONSUMER] fd count: after_close=%d leaked=%d\n",
           fd_after_close, fd_leaked);
    if (fd_leaked > 0) {
        fprintf(stderr, "[CONSUMER] WARNING: %d fd(s) leaked!\n", fd_leaked);
    }

    printf("[CONSUMER] received=%d failed=%d zero_copy_ok=%d/%d\n",
           received, failed, zero_copy_ok, received);

    if (ctx != NULL) {
        fprintf(stderr, "[CONSUMER] FAIL: ctx should be NULL after vfr_close\n");
        return -1;
    }

    return failed ? -1 : 0;
}

/* ─── test_reconnect：連線 → 斷線 → 重連，驗證無 fd 累積 ─────────────────── */
static int test_reconnect(const char *stream_name)
{
    printf("\n=== test_reconnect ===\n");

    int fd_start = count_open_fds();
    int failed = 0;

    for (int round = 0; round < 3 && !failed; round++) {
        vfr_ctx_t *ctx = vfr_open(stream_name, 0);
        if (!ctx) {
            fprintf(stderr, "[CONSUMER] round %d: vfr_open failed\n", round);
            failed = 1;
            break;
        }

        /* 接收一幀後斷線 */
        vfr_frame_t frame = {0};
        frame.dma_fd = -1;
        if (vfr_get_frame(ctx, &frame, 0) == 0) {
            vfr_put_frame(&frame);
        }

        vfr_close(&ctx);
        printf("[CONSUMER] reconnect round %d: fd_count=%d\n", round, count_open_fds());
    }

    int fd_end = count_open_fds();
    int leaked = fd_end - fd_start;
    printf("[CONSUMER] reconnect fd_leaked=%d\n", leaked);
    if (leaked > 0) {
        fprintf(stderr, "[CONSUMER] FAIL: fd leaked across reconnects\n");
        failed = 1;
    }

    printf("[%s] test_reconnect\n", failed ? "FAIL" : "PASS");
    return failed ? -1 : 0;
}

int main(int argc, char *argv[])
{
    setup_signals();

    /* 確認 VFR_MODE=client */
    const char *mode = getenv("VFR_MODE");
    if (!mode || strcmp(mode, "client") != 0) {
        fprintf(stderr, "[CONSUMER] ERROR: set VFR_MODE=client before running\n");
        fprintf(stderr, "  Usage: VFR_MODE=client ./test_ipc_consumer\n");
        return 1;
    }

    const char *stream_name = DEFAULT_STREAM;
    int max_frames = DEFAULT_FRAMES;
    bool do_reconnect_test = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc) {
            stream_name = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-reconnect") == 0) {
            do_reconnect_test = false;
        }
    }

    printf("[CONSUMER] VFR Phase 2 IPC Consumer\n");
    printf("[CONSUMER] stream='%s' frames=%d\n", stream_name, max_frames);

    int overall = 0;

    /* 主測試：接收幀 */
    overall |= (test_ipc_recv(stream_name, max_frames) != 0) ? 1 : 0;

    /* 重連測試 */
    if (do_reconnect_test) {
        overall |= (test_reconnect(stream_name) != 0) ? 1 : 0;
    }

    printf("\n=== Overall: %s ===\n", overall == 0 ? "PASS" : "FAIL");
    return overall;
}
