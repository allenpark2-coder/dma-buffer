/* core/vfr_ctx.c — Context 管理、Stream 生命週期
 *
 * vfr_ctx_t 的完整定義（opaque 給 consumer）。
 * 實作 vfr_open / vfr_close / vfr_get_frame / vfr_put_frame / vfr_get_eventfd。
 *
 * 模式選擇（環境變數 VFR_MODE）：
 *   "standalone"（預設）— Phase 1 單 process：直接操作 vfr_pool
 *   "client"            — Phase 2 IPC：連線到 server，透過 Unix socket 接收幀
 *
 * 原則四 Signal handler：呼叫者（main / test）負責設定 g_running 旗標；
 *   vfr_ctx 不設定 signal handler，保持模組職責單一。
 */

#include "vfr.h"
#include "vfr_defs.h"
#include "vfr_pool.h"
#include "platform/platform_adapter.h"
#include "ipc/vfr_client.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* ─── vfr_ctx_t 完整定義 ─────────────────────────────────────────────────── */
struct vfr_ctx {
    char              stream_name[VFR_SOCKET_NAME_MAX];
    bool              is_client;   /* true = Phase 2 IPC client mode */

    /* Standalone mode（Phase 1）*/
    vfr_pool_t       *pool;
    vfr_shm_header_t  shm_hdr;

    /* Client mode（Phase 2）*/
    vfr_client_state_t client;
};

/* ─── 平台選擇（standalone mode 用）──────────────────────────────────────── */
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
    /* 入口驗證：stream_name 長度 < VFR_SOCKET_NAME_MAX（防止 abstract socket overflow）*/
    if (!stream_name) {
        VFR_LOGE("stream_name is NULL");
        return NULL;
    }
    if (strlen(stream_name) >= VFR_SOCKET_NAME_MAX) {
        VFR_LOGE("stream_name too long (%zu >= %u)", strlen(stream_name), VFR_SOCKET_NAME_MAX);
        return NULL;
    }

    /* slot_count 驗證（client mode 忽略此參數，standalone 有上限）*/
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
    ctx->client.socket_fd = -1;  /* 初始化為無效 fd */
    ctx->client.eventfd   = -1;  /* Phase 3：初始化 eventfd 為 -1 */

    /* 模式判斷 */
    const char *mode_env = getenv("VFR_MODE");
    ctx->is_client = (mode_env && strcmp(mode_env, "client") == 0);

    if (ctx->is_client) {
        /* ── Client Mode（Phase 2/3）────────────────────────────────────── */

        /* Phase 3：從環境變數讀取 backpressure policy */
        uint32_t policy = VFR_BP_DROP_OLDEST;   /* 預設 */
        const char *pol_env = getenv("VFR_POLICY");
        if (pol_env) {
            if (strcmp(pol_env, "block_producer") == 0)
                policy = VFR_BP_BLOCK_PRODUCER;
            else if (strcmp(pol_env, "skip_self") == 0)
                policy = VFR_BP_SKIP_SELF;
            else if (strcmp(pol_env, "drop_oldest") != 0)
                VFR_LOGW("unknown VFR_POLICY='%s', using drop_oldest", pol_env);
        }

        if (vfr_client_connect(stream_name, &ctx->client, policy) != 0) {
            VFR_LOGE("vfr_client_connect('%s') failed", stream_name);
            free(ctx);
            return NULL;
        }
        VFR_LOGI("vfr_open(client): stream='%s' session_id=%u policy=%u evfd=%d",
                 stream_name, ctx->client.session_id, policy,
                 ctx->client.eventfd);
    } else {
        /* ── Standalone Mode（Phase 1）──────────────────────────────────── */
        ctx->shm_hdr.magic      = VFR_SHM_MAGIC;
        ctx->shm_hdr.slot_count = (slot_count == 0) ? VFR_DEFAULT_SLOTS : slot_count;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ctx->shm_hdr.producer_boot_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

        const vfr_platform_ops_t *ops = select_platform();
        ctx->pool = vfr_pool_create(ops, slot_count, &ctx->shm_hdr);
        if (!ctx->pool) {
            VFR_LOGE("vfr_pool_create failed");
            free(ctx);
            return NULL;
        }
        VFR_LOGI("vfr_open(standalone): stream='%s' slots=%u platform=%s",
                 ctx->stream_name, ctx->shm_hdr.slot_count, ops->name);
    }

    return ctx;
}

/* ─── vfr_close ──────────────────────────────────────────────────────────── */
void vfr_close(vfr_ctx_t **ctx)
{
    if (!ctx || !*ctx) return;
    vfr_ctx_t *c = *ctx;

    VFR_LOGI("vfr_close: stream='%s' mode=%s",
             c->stream_name, c->is_client ? "client" : "standalone");

    if (c->is_client) {
        /* Client mode 清理（原則五 Consumer 端順序）：
         * 1. 呼叫者已從 epoll 登出 eventfd（Phase 2 無 eventfd）
         * 2. 若持有 frame，呼叫者應先 vfr_put_frame()
         * 3. close socket */
        vfr_client_disconnect(&c->client);
    } else {
        /* Standalone mode 清理：pool destroy（內部按順序清理 platform + slots）*/
        vfr_pool_destroy(&c->pool);
    }

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

    if (ctx->is_client) {
        /* Client mode：recvmsg 從 server 接收幀 */
        int ret = vfr_client_recv_frame(&ctx->client, frame, flags);
        if (ret == 1) {
            /* EAGAIN（NONBLOCK）*/
            errno = EAGAIN;
            return -1;
        }
        return ret;   /* 0 = ok, -1 = error */
    }

    /* Standalone mode：pool acquire + dispatch */
    for (;;) {
        uint32_t slot_idx = 0;
        int ret = vfr_pool_acquire(ctx->pool, &slot_idx);

        if (ret == 0) {
            ret = vfr_pool_dispatch_single(ctx->pool, slot_idx, frame);
            if (ret == 0) {
                atomic_fetch_add_explicit(&ctx->shm_hdr.seq, 1u, memory_order_relaxed);
                return 0;
            }
            VFR_LOGW("dispatch_single slot[%u] failed, retrying", slot_idx);
            continue;
        }

        if (ret == 1) {
            if (flags & VFR_FLAG_NONBLOCK) {
                errno = EAGAIN;
                return -1;
            }
            struct timespec ns = { .tv_sec = 0, .tv_nsec = 1000000 };  /* 1ms */
            nanosleep(&ns, NULL);
            continue;
        }

        VFR_LOGE("vfr_pool_acquire failed");
        return -1;
    }
}

/* ─── vfr_put_frame ──────────────────────────────────────────────────────── */
void vfr_put_frame(vfr_frame_t *frame)
{
    if (!frame) return;
    if (frame->dma_fd < 0) return;

    /* 根據 priv 的 mode 標記決定清理路徑 */
    if (!frame->priv) {
        /* 無 priv：只 close fd */
        close(frame->dma_fd);
        frame->dma_fd = -1;
        return;
    }

    uint32_t mode = *(uint32_t *)frame->priv;

    if (mode == VFR_PRIV_CLIENT) {
        /* Client mode（Phase 2）：
         * 1. close consumer 的 dma_fd
         * 2. 發送 vfr_release_msg_t 給 server */
        vfr_client_ref_t *ref = (vfr_client_ref_t *)frame->priv;
        close(frame->dma_fd);
        frame->dma_fd = -1;
        frame->priv   = NULL;
        vfr_client_send_release(ref);
    } else {
        /* Standalone mode（VFR_PRIV_POOL）：pool put_slot */
        vfr_pool_put_slot(NULL, frame);
    }
}

/* ─── vfr_get_eventfd ────────────────────────────────────────────────────── */
int vfr_get_eventfd(vfr_ctx_t *ctx)
{
    if (!ctx) return -1;

    if (ctx->is_client) {
        /* Phase 3：回傳 server 在握手時傳來的 eventfd
         * fd 由框架持有；consumer 只可加入自己的 epoll，不得 close() */
        return vfr_client_get_eventfd(&ctx->client);
    }

    /* Standalone mode（Phase 1）：無 eventfd */
    return -1;
}
