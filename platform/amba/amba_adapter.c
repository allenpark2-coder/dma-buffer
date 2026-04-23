/* platform/amba/amba_adapter.c
 *
 * Ambarella IAV platform adapter。
 *
 * 編譯條件：
 *   - 真機（CV5/CV52/CV72）：直接編譯，需 iav_ioctl.h
 *   - 開發主機（無 IAV SDK）：不編譯此檔案；改用 platform/mock/mock_adapter.c
 *
 * ⚠️ 實作前必查（見 POOL_DESIGN.md §四）：
 *   - iav_ioctl.h 中 iav_canvas_desc 是否有 .buf.fd 欄位
 *   - CV72 與 CV5 的 struct 欄位順序可能不同，不可直接複製
 *   - use_dma_buf_fd = 1 是否需要在 IAV init 時設定
 */

#include "../../include/vfr_defs.h"
#include "../../include/vfr.h"
#include "../platform_adapter.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef HAVE_IAV_IOCTL_H
#  include <iav_ioctl.h>
#  include <sys/ioctl.h>
#  include <fcntl.h>
#endif

/* ─── Amba context ──────────────────────────────────────────────────────── */
typedef struct {
    int      iav_fd;      /* /dev/iav 或 /dev/iav6 */
    uint32_t canvas_id;   /* 通常為 0（main canvas） */
    uint64_t seq_num;     /* 單調遞增幀序號 */
} amba_ctx_t;

/* ─── init ──────────────────────────────────────────────────────────────── */
static int amba_init(void **ctx, const vfr_shm_header_t *cfg)
{
    (void)cfg;  /* Phase 1：尚未用到格式協商 */

#ifndef HAVE_IAV_IOCTL_H
    VFR_LOGE("amba_adapter: IAV SDK not available; use mock adapter instead");
    return -1;
#else
    amba_ctx_t *a = calloc(1, sizeof(*a));
    if (!a) {
        VFR_LOGE("calloc failed: %s", strerror(errno));
        return -1;
    }

    /* 根據 SDK 版本決定裝置節點（iav5 vs iav6） */
#  ifdef CV72
    const char *dev = "/dev/iav6";
#  else
    const char *dev = "/dev/iav";
#  endif

    a->iav_fd = open(dev, O_RDWR);
    if (a->iav_fd < 0) {
        VFR_LOGE("open %s failed: %s", dev, strerror(errno));
        free(a);
        return -1;
    }
    a->canvas_id = 0;
    *ctx = a;
    VFR_LOGI("amba_adapter: opened %s (canvas_id=%u)", dev, a->canvas_id);
    return 0;
#endif
}

/* ─── get_frame ─────────────────────────────────────────────────────────── */
static int amba_get_frame(void *ctx, vfr_frame_t *out)
{
    (void)ctx; (void)out;

#ifndef HAVE_IAV_IOCTL_H
    VFR_LOGE("amba_adapter: IAV SDK not available");
    return -1;
#else
    amba_ctx_t *a = ctx;

    struct iav_querydesc query = {0};
    query.qid = IAV_DESC_CANVAS;
    query.arg.canvas.id = a->canvas_id;

    if (ioctl(a->iav_fd, IAV_IOC_QUERY_DESC, &query) < 0) {
        if (errno == EAGAIN) return 1;  /* 暫無新幀 */
        VFR_LOGE("IAV_IOC_QUERY_DESC failed: %s", strerror(errno));
        return -1;
    }

    /* 每幀填入 vfr_frame_t（不從 SHM header 讀靜態值）
     * ⚠️ 欄位名稱依實際 SDK header，需 grep 確認 */
    out->dma_fd          = query.arg.canvas.buf.fd;
    out->width           = query.arg.canvas.width;
    out->height          = query.arg.canvas.height;
    out->stride          = query.arg.canvas.pitch;      /* luma pitch */
    out->buf_size        = query.arg.canvas.buf.length;
    out->format          = V4L2_PIX_FMT_NV12;           /* Amba 預設 NV12 */
    out->plane_offset[0] = 0;
    out->plane_offset[1] = out->stride * out->height;
    out->plane_offset[2] = 0;                            /* NV12 只有 2 plane */
    out->timestamp_ns    = (uint64_t)query.arg.canvas.dsp_pts * 1000u;
    out->flags           = 0;
    out->seq_num         = ++a->seq_num;
    out->priv            = NULL;

    VFR_LOGD("get_frame: seq=%llu dma_fd=%d %ux%u stride=%u buf_size=%u",
             (unsigned long long)out->seq_num, out->dma_fd,
             out->width, out->height, out->stride, out->buf_size);
    return 0;
#endif
}

/* ─── put_frame ─────────────────────────────────────────────────────────── */
static void amba_put_frame(void *ctx, vfr_frame_t *frame)
{
    (void)ctx;
    /* IAV canvas buffer 的 dma_fd 生命週期由 IAV driver 管理；
     * vfr_pool 負責 close consumer 端複製的 fd，此處只處理 producer 端的歸還。
     * Phase 1 單 process 版本：直接 close fd */
    if (frame && frame->dma_fd >= 0) {
        close(frame->dma_fd);
        frame->dma_fd = -1;
    }
}

/* ─── destroy ───────────────────────────────────────────────────────────── */
static void amba_destroy(void **ctx)
{
    if (!ctx || !*ctx) return;
    amba_ctx_t *a = *ctx;
    if (a->iav_fd >= 0) {
        close(a->iav_fd);
        a->iav_fd = -1;
    }
    free(a);
    *ctx = NULL;
    VFR_LOGI("amba_adapter: destroyed");
}

/* ─── ops table ─────────────────────────────────────────────────────────── */
static const vfr_platform_ops_t s_amba_ops = {
#ifdef CV72
    .name               = "amba_iav6",
#else
    .name               = "amba_iav5",
#endif
    .init               = amba_init,
    .get_frame          = amba_get_frame,
    .put_frame          = amba_put_frame,
    .destroy            = amba_destroy,
    .has_native_dma_buf = true,
};

const vfr_platform_ops_t *vfr_get_amba_ops(void)
{
    return &s_amba_ops;
}
