/* test/test_multicast.c — Phase 3 驗證：低延遲多端消費 + Backpressure Policy
 *
 * 功能：
 *   單一 binary，透過 --role 切換：
 *     producer  — 建立 server，以 30fps 出幀
 *     rtsp      — DROP_OLDEST policy，快速消費，量測延遲
 *     recorder  — BLOCK_PRODUCER policy，慢速消費（模擬錄影寫 disk）
 *     motion    — SKIP_SELF policy，可變速消費（模擬 AI 推論）
 *
 * 用法：
 *   ./test_multicast --role producer [--stream NAME] [--frames N]
 *   VFR_POLICY=drop_oldest    VFR_MODE=client ./test_multicast --role rtsp     [--stream NAME] [--frames N]
 *   VFR_POLICY=block_producer VFR_MODE=client ./test_multicast --role recorder [--stream NAME] [--frames N]
 *   VFR_POLICY=skip_self      VFR_MODE=client ./test_multicast --role motion   [--stream NAME] [--frames N]
 *
 * Phase 3 驗收條件（plan §3.3 Checklist）：
 *   - 3 consumer 同時跑，端對端延遲 < 16ms（16ms @ 60fps 標準；30fps 下 ≤ 33ms）
 *   - Recorder（BLOCK_PRODUCER）卡住不影響 RTSP/Motion 延遲
 *   - DROP_OLDEST 觸發時 drop_count 遞增（透過 SHM header 觀察）
 *   - CPU 不得有 busy loop（全程 epoll_wait，不用 sleep）
 *   - `-fsanitize=address` 無 error（ASan checklist 3.4）
 */

#include "vfr.h"
#include "vfr_defs.h"
#include "ipc/vfr_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>

/* ─── 常數 ────────────────────────────────────────────────────────────────── */
#define DEFAULT_STREAM          "test_multicast"
#define DEFAULT_PRODUCER_FRAMES 200
#define DEFAULT_CONSUMER_FRAMES  40
#define FRAME_INTERVAL_NS       (33333333ULL)   /* ~30fps */
#define ZERO_COPY_MAGIC         0xDEADBEEFu

/* Recorder 模擬的 disk 寫入延遲（< 1 frame 時間以確保 BLOCK_PRODUCER 不永遠卡住）*/
#define RECORDER_PROCESS_MS      20             /* 20ms < 33ms(1 frame @ 30fps) */
/* Motion 模擬的推論延遲 */
#define MOTION_PROCESS_MS         8

/* ─── Signal 處理（原則四）────────────────────────────────────────────────── */
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

/* ─── /proc/self/fd 計數（fd leak 驗證）─────────────────────────────────── */
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

/* ─── 延遲統計 ───────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t sum_ns;
    uint32_t count;
    uint32_t over_threshold;   /* > 16ms */
} latency_stats_t;

static void stats_reset(latency_stats_t *s)
{
    s->min_ns        = UINT64_MAX;
    s->max_ns        = 0;
    s->sum_ns        = 0;
    s->count         = 0;
    s->over_threshold = 0;
}

static void stats_add(latency_stats_t *s, uint64_t latency_ns)
{
    if (latency_ns < s->min_ns) s->min_ns = latency_ns;
    if (latency_ns > s->max_ns) s->max_ns = latency_ns;
    s->sum_ns += latency_ns;
    s->count++;
    if (latency_ns > 16000000ULL) s->over_threshold++;  /* > 16ms */
}

static void stats_print(const latency_stats_t *s, const char *role)
{
    if (s->count == 0) {
        printf("[%s] no frames measured\n", role);
        return;
    }
    uint64_t avg_ns = s->sum_ns / s->count;
    printf("[%s] latency stats (%u frames):\n", role, s->count);
    printf("  min=%.2fms  avg=%.2fms  max=%.2fms  over16ms=%u/%u\n",
           s->min_ns / 1e6,
           avg_ns   / 1e6,
           s->max_ns / 1e6,
           s->over_threshold, s->count);
}

/* ─── Producer 角色 ──────────────────────────────────────────────────────── */
static int run_producer(const char *stream, int max_frames)
{
    printf("[PRODUCER] stream='%s' max_frames=%d\n", stream, max_frames);

    vfr_server_t *srv = vfr_server_create(stream, 0);
    if (!srv) {
        fprintf(stderr, "[PRODUCER] vfr_server_create failed\n");
        return 1;
    }

    printf("[PRODUCER] listening on \\0/vfr/%s — waiting for consumers...\n", stream);

    int produced = 0;

    while (g_running && (max_frames < 0 || produced < max_frames)) {
        /* 處理連線事件（新 consumer、release msg）*/
        if (vfr_server_handle_events(srv, 0) < 0) break;

        /* 產幀 */
        int ret = vfr_server_produce(srv);
        if (ret == 0) {
            produced++;
            if (produced % 30 == 0) {
                printf("[PRODUCER] produced %d frames\n", produced);
                fflush(stdout);
            }
        } else if (ret == 1) {
            /* EAGAIN — platform 暫無幀 */
        } else {
            fprintf(stderr, "[PRODUCER] produce error, stopping\n");
            break;
        }

        /* 控制幀率（nanosleep，不用 sleep()）*/
        struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)(FRAME_INTERVAL_NS / 2) };
        nanosleep(&ts, NULL);
    }

    printf("[PRODUCER] done. produced=%d\n", produced);
    vfr_server_destroy(&srv);

    if (srv != NULL) {
        fprintf(stderr, "[PRODUCER] FAIL: srv not NULL after destroy\n");
        return 1;
    }
    printf("[PRODUCER] exited cleanly.\n");
    return 0;
}

/* ─── Consumer 共用：epoll_wait on eventfd → recv → process → release ────── */
static int run_consumer(const char *role, const char *stream,
                        int max_frames, uint32_t process_ms)
{
    printf("[%s] stream='%s' frames=%d process_ms=%u\n",
           role, stream, max_frames, process_ms);

    int fd_before = count_open_fds();

    /* 連線（policy 由 VFR_POLICY env 控制，由 vfr_ctx.c 讀取）*/
    vfr_ctx_t *ctx = vfr_open(stream, 0);
    if (!ctx) {
        fprintf(stderr, "[%s] vfr_open failed: %s\n", role, strerror(errno));
        return -1;
    }

    /* 取 eventfd，加入自己的 epoll */
    int evfd = vfr_get_eventfd(ctx);
    if (evfd < 0) {
        fprintf(stderr, "[%s] vfr_get_eventfd returned %d\n", role, evfd);
        vfr_close(&ctx);
        return -1;
    }

    int my_epoll = epoll_create1(0);
    if (my_epoll < 0) {
        fprintf(stderr, "[%s] epoll_create1 failed: %s\n", role, strerror(errno));
        vfr_close(&ctx);
        return -1;
    }

    struct epoll_event ev_add = {
        .events  = EPOLLIN | EPOLLET,   /* edge-triggered */
        .data.fd = evfd,
    };
    if (epoll_ctl(my_epoll, EPOLL_CTL_ADD, evfd, &ev_add) < 0) {
        fprintf(stderr, "[%s] epoll_ctl ADD evfd=%d failed: %s\n", role, evfd, strerror(errno));
        close(my_epoll);
        vfr_close(&ctx);
        return -1;
    }

    latency_stats_t stats;
    stats_reset(&stats);

    int received   = 0;
    int failed     = 0;
    int zero_copy_ok = 0;

    printf("[%s] epoll loop started (evfd=%d)\n", role, evfd);

    while (g_running && (max_frames < 0 || received < max_frames)) {
        /* 等待 eventfd 通知（主迴圈禁止 sleep，使用 epoll_wait）*/
        struct epoll_event ready[4];
        int nfds = epoll_wait(my_epoll, ready, 4, 2000 /* 2s timeout */);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[%s] epoll_wait failed: %s\n", role, strerror(errno));
            failed = 1;
            break;
        }
        if (nfds == 0) {
            /* 2s 無通知 — producer 可能還沒出幀或已停止 */
            printf("[%s] epoll_wait timeout, still waiting...\n", role);
            continue;
        }

        /* 逐一處理通知（edge-triggered 需把所有積壓幀取完）*/
        for (int i = 0; i < nfds; i++) {
            if (!(ready[i].events & EPOLLIN)) continue;

            /* edge-triggered：迴圈取幀直到 EAGAIN */
            for (;;) {
                vfr_frame_t frame = { .dma_fd = -1 };
                int ret = vfr_get_frame(ctx, &frame, VFR_FLAG_NONBLOCK);
                if (ret != 0) break;  /* EAGAIN 或 error */

                /* ── 計算端對端延遲 ────────────────────────────── */
                struct timespec ts_now;
                clock_gettime(CLOCK_MONOTONIC, &ts_now);
                uint64_t now_ns = (uint64_t)ts_now.tv_sec * 1000000000ULL + ts_now.tv_nsec;
                uint64_t latency_ns = (now_ns > frame.timestamp_ns)
                                      ? (now_ns - frame.timestamp_ns) : 0;
                stats_add(&stats, latency_ns);

                /* ── Zero-copy 驗證 ─────────────────────────────── */
                void *ptr = vfr_map(&frame);
                if (ptr) {
                    uint32_t magic = *(volatile uint32_t *)ptr;
                    if (magic == ZERO_COPY_MAGIC) zero_copy_ok++;
                    vfr_unmap(&frame, ptr);
                }

                if (received < 3) {
                    printf("[%s] frame[%d]: seq=%llu latency=%.2fms dma_fd=%d %ux%u\n",
                           role, received,
                           (unsigned long long)frame.seq_num,
                           latency_ns / 1e6,
                           frame.dma_fd, frame.width, frame.height);
                }

                /* ── 模擬處理延遲（Recorder 用）─────────────────── */
                if (process_ms > 0) {
                    struct timespec sleep_ts = {
                        .tv_sec  = 0,
                        .tv_nsec = (long)process_ms * 1000000L,
                    };
                    nanosleep(&sleep_ts, NULL);
                }

                vfr_put_frame(&frame);

                if (frame.dma_fd != -1) {
                    fprintf(stderr, "[%s] FAIL: dma_fd should be -1 after put_frame\n", role);
                    failed = 1;
                }

                received++;
                if (max_frames >= 0 && received >= max_frames) break;
            }

            if (max_frames >= 0 && received >= max_frames) break;
        }
    }

    /* ── 從 epoll 登出 eventfd（vfr_close 前必須）────────────────────────── */
    epoll_ctl(my_epoll, EPOLL_CTL_DEL, evfd, NULL);
    close(my_epoll);

    vfr_close(&ctx);

    /* ── fd leak 驗證 ────────────────────────────────────────────────────── */
    int fd_after = count_open_fds();
    int leaked   = fd_after - fd_before;
    if (leaked > 0) {
        fprintf(stderr, "[%s] WARNING: %d fd(s) leaked!\n", role, leaked);
    }

    /* ── 延遲統計報告 ────────────────────────────────────────────────────── */
    stats_print(&stats, role);
    printf("[%s] received=%d zero_copy_ok=%d/%d fd_leaked=%d\n",
           role, received, zero_copy_ok, received, leaked);

    if (ctx != NULL) {
        fprintf(stderr, "[%s] FAIL: ctx should be NULL after vfr_close\n", role);
        return -1;
    }

    /* ── 驗收：延遲不得超過 16ms 太多（允許少量抖動）────────────────────── */
    /* recorder 因為故意 sleep，延遲允許更高；RTSP 和 Motion 要求嚴格 */
    bool is_strict = (process_ms == 0 || process_ms <= MOTION_PROCESS_MS);
    if (is_strict && stats.count > 0 && stats.over_threshold > stats.count / 2) {
        fprintf(stderr, "[%s] FAIL: too many frames over 16ms (%u/%u)\n",
                role, stats.over_threshold, stats.count);
        return -1;
    }

    return failed ? -1 : 0;
}

/* ─── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    setup_signals();

    const char *stream      = DEFAULT_STREAM;
    const char *role        = NULL;
    int         max_frames  = -1;   /* -1 = use role default */

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--role")   == 0 && i + 1 < argc) role   = argv[++i];
        else if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc) stream = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--inf")    == 0)                  max_frames = -1;
    }

    if (!role) {
        fprintf(stderr, "Usage: %s --role producer|rtsp|recorder|motion [options]\n", argv[0]);
        fprintf(stderr, "  --stream NAME     stream name (default: %s)\n", DEFAULT_STREAM);
        fprintf(stderr, "  --frames N        max frames (default: role-dependent)\n");
        return 1;
    }

    printf("[MULTICAST] VFR Phase 3 — role=%s stream=%s\n", role, stream);

    if (strcmp(role, "producer") == 0) {
        if (max_frames < 0) max_frames = DEFAULT_PRODUCER_FRAMES;
        return run_producer(stream, max_frames);
    }

    /* Consumer roles — VFR_MODE=client 必須設定 */
    const char *mode = getenv("VFR_MODE");
    if (!mode || strcmp(mode, "client") != 0) {
        fprintf(stderr, "[%s] ERROR: set VFR_MODE=client\n", role);
        fprintf(stderr, "  Usage: VFR_MODE=client VFR_POLICY=<policy> ./%s --role %s\n",
                argv[0], role);
        return 1;
    }

    if (max_frames < 0) max_frames = DEFAULT_CONSUMER_FRAMES;

    int result;
    if (strcmp(role, "rtsp") == 0) {
        /* DROP_OLDEST：快速消費，量測延遲 */
        result = run_consumer("RTSP", stream, max_frames, 0 /* no delay */);
    } else if (strcmp(role, "recorder") == 0) {
        /* BLOCK_PRODUCER：模擬慢速寫 disk */
        result = run_consumer("RECORDER", stream, max_frames, RECORDER_PROCESS_MS);
    } else if (strcmp(role, "motion") == 0) {
        /* SKIP_SELF：模擬 AI 推論，可變速 */
        result = run_consumer("MOTION", stream, max_frames, MOTION_PROCESS_MS);
    } else {
        fprintf(stderr, "Unknown role: %s\n", role);
        return 1;
    }

    printf("[MULTICAST] role=%s %s\n", role, result == 0 ? "PASS" : "FAIL");
    return result == 0 ? 0 : 1;
}
