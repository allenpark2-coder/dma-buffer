/* test/test_single_proc.c — Phase 1 驗證：單 process 影像讀取
 *
 * 驗收條件（見 plan Phase 1 Checklist）：
 *   3.1 Build：make test_single_proc 獨立編譯
 *   3.1 Standalone：./test_single_proc 直接執行（mock 模式）
 *   3.2 空輸入：vfr_get_frame(NULL, ...) / vfr_put_frame(NULL) 不 crash
 *   3.2 非法 fd：dma_fd=-1 時 vfr_map() 回傳 NULL
 *   3.2 Buffer 邊界：slot_count=1 極限情境
 *   3.3 double-free：連續兩次 vfr_close(&ctx) 為 no-op
 *   3.4 valgrind：valgrind --leak-check=full 無 error
 *   3.6 debug log：DEBUG=1 下可見每幀 seq_num 與 timestamp
 *
 * 執行方式：
 *   ./test_single_proc                      → 讀 10 幀，dump 到 /tmp/frame_xxx.yuv
 *   DEBUG=1 ./test_single_proc              → 同上，含 debug log
 *   VFR_MOCK_BINARY=file.yuv ./test_single_proc → 從預錄 binary 讀取
 *   ./test_single_proc --no-dump            → 只測試 API，不寫 YUV 檔
 */

#include "vfr.h"
#include "vfr_defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

#define TEST_FRAME_COUNT   10
#define TEST_STREAM_NAME   "test_phase1"

/* ─── Signal 處理（原則四：sigaction，不用 signal()）────────────────────── */
static volatile sig_atomic_t g_running = 1;
static void sig_handler(int signo) { (void)signo; g_running = 0; }

static void setup_signals(void)
{
    struct sigaction sa = { .sa_handler = sig_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* 忽略 SIGPIPE（Phase 2+ Unix socket 需要）*/
    struct sigaction sa_pipe = { .sa_handler = SIG_IGN };
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);
}

/* ─── dump 一幀到 /tmp/frame_NNN.yuv ────────────────────────────────────── */
static int dump_frame(const vfr_frame_t *frame, int idx, bool do_dump)
{
    if (!do_dump) return 0;

    char path[64];
    snprintf(path, sizeof(path), "/tmp/frame_%03d.yuv", idx);

    void *ptr = vfr_map(frame);
    if (!ptr) {
        fprintf(stderr, "[TEST] vfr_map failed for frame %d\n", idx);
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[TEST] open(%s) failed: %s\n", path, strerror(errno));
        vfr_unmap(frame, ptr);
        return -1;
    }

    ssize_t written = write(fd, ptr, frame->buf_size);
    close(fd);
    vfr_unmap(frame, ptr);

    if (written < (ssize_t)frame->buf_size) {
        fprintf(stderr, "[TEST] write incomplete: %zd/%u\n", written, frame->buf_size);
        return -1;
    }

    printf("[TEST] frame[%d]: seq=%llu ts_ns=%llu %ux%u buf_size=%u → %s\n",
           idx,
           (unsigned long long)frame->seq_num,
           (unsigned long long)frame->timestamp_ns,
           frame->width, frame->height, frame->buf_size,
           path);
    return 0;
}

/* ─── test_normal_flow：讀 N 幀，dump 到 /tmp ────────────────────────────── */
static int test_normal_flow(bool do_dump)
{
    printf("\n=== test_normal_flow (stream='%s', frames=%d) ===\n",
           TEST_STREAM_NAME, TEST_FRAME_COUNT);

    vfr_ctx_t *ctx = vfr_open(TEST_STREAM_NAME, 0 /* default slots */);
    if (!ctx) {
        fprintf(stderr, "[FAIL] vfr_open failed\n");
        return -1;
    }

    int failed = 0;
    for (int i = 0; i < TEST_FRAME_COUNT && g_running; i++) {
        vfr_frame_t frame = {0};
        frame.dma_fd = -1;

        int ret = vfr_get_frame(ctx, &frame, 0);
        if (ret != 0) {
            fprintf(stderr, "[FAIL] vfr_get_frame[%d] returned %d\n", i, ret);
            failed = 1;
            break;
        }

        if (dump_frame(&frame, i, do_dump) != 0) {
            failed = 1;
        }

        vfr_put_frame(&frame);

        /* 驗證 put_frame 後 dma_fd 已設為 -1（double-put 防護）*/
        if (frame.dma_fd != -1) {
            fprintf(stderr, "[FAIL] frame.dma_fd should be -1 after put_frame, got %d\n",
                    frame.dma_fd);
            failed = 1;
        }
    }

    vfr_close(&ctx);

    if (ctx != NULL) {
        fprintf(stderr, "[FAIL] vfr_close should set *ctx to NULL\n");
        return -1;
    }

    printf("[%s] test_normal_flow\n", failed ? "FAIL" : "PASS");
    return failed ? -1 : 0;
}

/* ─── test_slot_boundary：slot_count=1 極限情境 ─────────────────────────── */
static int test_slot_boundary(void)
{
    printf("\n=== test_slot_boundary (slot_count=1) ===\n");

    vfr_ctx_t *ctx = vfr_open("test_boundary", 1);
    if (!ctx) {
        fprintf(stderr, "[FAIL] vfr_open(slot_count=1) failed\n");
        return -1;
    }

    int failed = 0;
    /* 讀 3 幀，每幀立即歸還（slot_count=1 時必須如此） */
    for (int i = 0; i < 3; i++) {
        vfr_frame_t frame = {0};
        frame.dma_fd = -1;

        if (vfr_get_frame(ctx, &frame, 0) != 0) {
            fprintf(stderr, "[FAIL] vfr_get_frame[%d] failed with slot_count=1\n", i);
            failed = 1;
            break;
        }
        printf("[TEST] slot_count=1 frame[%d]: seq=%llu dma_fd=%d\n",
               i, (unsigned long long)frame.seq_num, frame.dma_fd);
        vfr_put_frame(&frame);
    }

    vfr_close(&ctx);
    printf("[%s] test_slot_boundary\n", failed ? "FAIL" : "PASS");
    return failed ? -1 : 0;
}

/* ─── test_null_inputs：NULL 入口不 crash ───────────────────────────────── */
static int test_null_inputs(void)
{
    printf("\n=== test_null_inputs ===\n");
    int failed = 0;

    /* vfr_open(NULL) */
    vfr_ctx_t *ctx_null = vfr_open(NULL, 0);
    if (ctx_null != NULL) {
        fprintf(stderr, "[FAIL] vfr_open(NULL) should return NULL\n");
        vfr_close(&ctx_null);
        failed = 1;
    }

    /* vfr_open with name too long */
    char long_name[VFR_SOCKET_NAME_MAX + 2];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    vfr_ctx_t *ctx_long = vfr_open(long_name, 0);
    if (ctx_long != NULL) {
        fprintf(stderr, "[FAIL] vfr_open(too_long_name) should return NULL\n");
        vfr_close(&ctx_long);
        failed = 1;
    }

    /* vfr_get_frame(NULL, ...) */
    vfr_frame_t dummy = {0};
    dummy.dma_fd = -1;
    int ret = vfr_get_frame(NULL, &dummy, VFR_FLAG_NONBLOCK);
    if (ret == 0) {
        fprintf(stderr, "[FAIL] vfr_get_frame(NULL, ...) should fail\n");
        failed = 1;
    }

    /* vfr_get_frame(ctx, NULL) */
    vfr_ctx_t *ctx = vfr_open(TEST_STREAM_NAME, 0);
    if (ctx) {
        ret = vfr_get_frame(ctx, NULL, VFR_FLAG_NONBLOCK);
        if (ret == 0) {
            fprintf(stderr, "[FAIL] vfr_get_frame(ctx, NULL) should fail\n");
            failed = 1;
        }
        vfr_close(&ctx);
    }

    /* vfr_put_frame(NULL) */
    vfr_put_frame(NULL);

    /* vfr_map with dma_fd=-1 */
    vfr_frame_t bad_frame = {0};
    bad_frame.dma_fd  = -1;
    bad_frame.buf_size = 1920 * 1080 * 3 / 2;
    void *ptr = vfr_map(&bad_frame);
    if (ptr != NULL) {
        fprintf(stderr, "[FAIL] vfr_map(dma_fd=-1) should return NULL\n");
        vfr_unmap(&bad_frame, ptr);
        failed = 1;
    }

    /* vfr_open(slot_count > VFR_MAX_SLOTS) */
    vfr_ctx_t *ctx_over = vfr_open("test_over", VFR_MAX_SLOTS + 1);
    if (ctx_over != NULL) {
        fprintf(stderr, "[FAIL] vfr_open(slot_count=%u) should return NULL\n",
                VFR_MAX_SLOTS + 1);
        vfr_close(&ctx_over);
        failed = 1;
    }

    printf("[%s] test_null_inputs\n", failed ? "FAIL" : "PASS");
    return failed ? -1 : 0;
}

/* ─── test_double_close：連續兩次 vfr_close 為 no-op ──────────────────── */
static int test_double_close(void)
{
    printf("\n=== test_double_close ===\n");

    vfr_ctx_t *ctx = vfr_open(TEST_STREAM_NAME, 0);
    if (!ctx) {
        fprintf(stderr, "[FAIL] vfr_open failed\n");
        return -1;
    }

    vfr_close(&ctx);   /* 第一次：正常釋放 */
    vfr_close(&ctx);   /* 第二次：no-op，不得 crash 或 double-free */
    vfr_close(NULL);   /* NULL pointer：no-op */

    printf("[PASS] test_double_close\n");
    return 0;
}

/* ─── test_double_put：連續兩次 vfr_put_frame 為 no-op ─────────────────── */
static int test_double_put(void)
{
    printf("\n=== test_double_put ===\n");

    vfr_ctx_t *ctx = vfr_open(TEST_STREAM_NAME, 0);
    if (!ctx) {
        fprintf(stderr, "[FAIL] vfr_open failed\n");
        return -1;
    }

    vfr_frame_t frame = {0};
    frame.dma_fd = -1;
    if (vfr_get_frame(ctx, &frame, 0) != 0) {
        fprintf(stderr, "[FAIL] vfr_get_frame failed\n");
        vfr_close(&ctx);
        return -1;
    }

    vfr_put_frame(&frame);   /* 第一次：正常歸還 */
    vfr_put_frame(&frame);   /* 第二次：dma_fd=-1，應為 no-op */

    vfr_close(&ctx);
    printf("[PASS] test_double_put\n");
    return 0;
}

/* ─── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    setup_signals();

    bool do_dump = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-dump") == 0) do_dump = false;
    }

    printf("VFR Phase 1 Test — Single Process\n");
    printf("Platform: %s\n", getenv("VFR_PLATFORM") ? getenv("VFR_PLATFORM") : "mock");
    printf("Log level: %d\n", VFR_LOG_LEVEL);
    if (do_dump) printf("Dumping frames to /tmp/frame_NNN.yuv\n");

    int overall = 0;
    overall |= (test_null_inputs()    != 0) ? 1 : 0;
    overall |= (test_double_close()   != 0) ? 1 : 0;
    overall |= (test_double_put()     != 0) ? 1 : 0;
    overall |= (test_slot_boundary()  != 0) ? 1 : 0;
    overall |= (test_normal_flow(do_dump) != 0) ? 1 : 0;

    printf("\n=== Overall: %s ===\n", overall == 0 ? "PASS" : "FAIL");

    if (do_dump && overall == 0) {
        printf("\nYUV files written to /tmp/frame_000.yuv ~ /tmp/frame_%03d.yuv\n",
               TEST_FRAME_COUNT - 1);
        printf("View with: ffplay -f rawvideo -pix_fmt nv12 -video_size 1920x1080 /tmp/frame_000.yuv\n");
    }

    return overall;
}
