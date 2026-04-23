/* core/vfr_ctx.c — Context 管理、Stream 生命週期
 *
 * vfr_ctx_t 的完整定義（opaque 給 consumer）。
 * 實作 vfr_open / vfr_close / vfr_get_frame / vfr_put_frame / vfr_get_eventfd。
 *
 * Phase 1（單 process）：
 *   - 直接操作 vfr_pool，無 Unix socket / eventfd / watchdog
 *   - 平台選擇：環境變數 VFR_PLATFORM=mock（預設）或 VFR_PLATFORM=amba
 *
 * 原則四 Signal handler：呼叫者（main / test）負責設定 g_running 旗標；
 *   vfr_ctx 不設定 signal handler，保持模組職責單一。
 */

#include "vfr.h"
#include "vfr_defs.h"
#include "vfr_pool.h"
#include "platform/platform_adapter.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* ─── vfr_ctx_t 完整定義（外部只見 vfr_ctx_t*）──────────────────────────── */
struct vfr_ctx {
    char              stream_name[VFR_SOCKET_NAME_MAX];
    vfr_pool_t       *pool;
    vfr_shm_header_t  shm_hdr;   /* 本地 SHM 副本（Phase 1 無 shared memory） */
};

/* ─── 平台選擇（依環境變數）─────────────────────────────────────────────── */
static const vfr_platform_ops_t *select_platform(void)
{
    const char *env = getenv("VFR_PLATFORM");
    if (!env || strcmp(env, "mock") == 0) {
        return vfr_get_mock_ops();
    }
#ifdef HAVE_IAV_IOCTL_H
    if (strcmp(env, "amba") == 0) {
        return vfr_get_amba_ops();
    }
#endif
    VFR_LOGW("unknown VFR_PLATFORM='%s', falling back to mock", env);
    return vfr_get_mock_ops();
}

/* ─── vfr_open ───────────────────────────────────────────────────────────── */
vfr_ctx_t *vfr_open(const char *stream_name, uint32_t slot_count)
{
    /* 入口驗證：stream_name 長度 < VFR_SOCKET_NAME_MAX（防止 abstract socket overflow） */
    if (!stream_name) {
        VFR_LOGE("stream_name is NULL");
        return NULL;
    }
    if (strlen(stream_name) >= VFR_SOCKET_NAME_MAX) {
        VFR_LOGE("stream_name too long (%zu >= %u)", strlen(stream_name), VFR_SOCKET_NAME_MAX);
        return NULL;
    }

    /* slot_count 驗證（0 → default；超過 MAX → fail） */
    if (slot_count > VFR_MAX_SLOTS) {
        VFR_LOGE("slot_count %u > VFR_MAX_SLOTS %u", slot_count, VFR_MAX_SLOTS);
        return NULL;
    }

    vfr_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        VFR_LOGE("calloc failed: %s", strerror(errno));
        return NULL;
    }

    strncpy(ctx->stream_name, stream_name, VFR_SOCKET_NAME_MAX - 1);
    ctx->stream_name[VFR_SOCKET_NAME_MAX - 1] = '\0';

    /* 初始化 SHM header（Phase 1 本地版本，無跨 process 共享） */
    ctx->shm_hdr.magic      = VFR_SHM_MAGIC;
    ctx->shm_hdr.slot_count = (slot_count == 0) ? VFR_DEFAULT_SLOTS : slot_count;

    /* 記錄 producer 啟動時間（預留欄位，見架構決策表） */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ctx->shm_hdr.producer_boot_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    /* 選擇平台並建立 pool */
    const vfr_platform_ops_t *ops = select_platform();
    ctx->pool = vfr_pool_create(ops, slot_count, &ctx->shm_hdr);
    if (!ctx->pool) {
        VFR_LOGE("vfr_pool_create failed");
        free(ctx);
        return NULL;
    }

    VFR_LOGI("vfr_open: stream='%s' slots=%u platform=%s",
             ctx->stream_name, ctx->shm_hdr.slot_count, ops->name);
    return ctx;
}

/* ─── vfr_close ──────────────────────────────────────────────────────────── */
void vfr_close(vfr_ctx_t **ctx)
{
    /* no-op if NULL or already closed（原則五：重複呼叫安全） */
    if (!ctx || !*ctx) return;

    vfr_ctx_t *c = *ctx;

    /* 原則五釋放順序（Consumer 端，Phase 1 簡化版）：
     * 1. pool destroy（內部按順序清理 platform + slots）
     * 2. free ctx
     * 3. *ctx = NULL
     */
    VFR_LOGI("vfr_close: stream='%s'", c->stream_name);

    vfr_pool_destroy(&c->pool);
    free(c);
    *ctx = NULL;
}

/* ─── vfr_get_frame ──────────────────────────────────────────────────────── */
int vfr_get_frame(vfr_ctx_t *ctx, vfr_frame_t *frame, int flags)
{
    if (!ctx) {
        VFR_LOGE("vfr_get_frame: ctx is NULL");
        return -1;
    }
    if (!frame) {
        VFR_LOGE("vfr_get_frame: frame is NULL");
        return -1;
    }

    /* Phase 1：簡單重試直到有幀（非 NONBLOCK 模式） */
    for (;;) {
        uint32_t slot_idx = 0;
        int ret = vfr_pool_acquire(ctx->pool, &slot_idx);

        if (ret == 0) {
            /* 取到幀：dispatch 給（單一）consumer */
            ret = vfr_pool_dispatch_single(ctx->pool, slot_idx, frame);
            if (ret == 0) {
                /* 更新 SHM header seq（本地計數，Phase 1 不做跨 process 共享） */
                atomic_fetch_add_explicit(&ctx->shm_hdr.seq, 1u, memory_order_relaxed);
                return 0;
            }
            /* dispatch 失敗（不應發生）：繼續重試 */
            VFR_LOGW("dispatch_single slot[%u] failed, retrying", slot_idx);
            continue;
        }

        if (ret == 1) {
            /* EAGAIN：暫無新幀 */
            if (flags & VFR_FLAG_NONBLOCK) {
                errno = EAGAIN;
                return -1;
            }
            /* 阻塞模式：短暫 yield 後重試（不用 sleep 以避免阻塞 epoll）
             * Phase 1 mock adapter 的 get_frame 一定成功，此路徑理論上不觸發 */
            struct timespec ns = { .tv_sec = 0, .tv_nsec = 1000000 };  /* 1ms */
            nanosleep(&ns, NULL);
            continue;
        }

        /* ret == -1：不可恢復錯誤 */
        VFR_LOGE("vfr_pool_acquire failed");
        return -1;
    }
}

/* ─── vfr_put_frame ──────────────────────────────────────────────────────── */
void vfr_put_frame(vfr_frame_t *frame)
{
    /* no-op if NULL（原則五）*/
    if (!frame) return;

    /* dma_fd == -1：已歸還過（防 double-free）*/
    if (frame->dma_fd < 0) return;

    vfr_pool_put_slot(NULL, frame);   /* pool 從 frame->priv 取得 */
}

/* ─── vfr_get_eventfd ────────────────────────────────────────────────────── */
int vfr_get_eventfd(vfr_ctx_t *ctx)
{
    (void)ctx;
    /* Phase 1 單 process 版本尚未實作 eventfd 通知；Phase 3 實作 */
    return -1;
}
