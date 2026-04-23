/* platform/platform_adapter.h — 純虛擬介面（原則三）
 * 所有平台實作藏在函式指標表後面，上層邏輯只操作 vfr_platform_ops_t。
 * 禁止上層邏輯直接操作具體平台 struct。
 */
#ifndef VFR_PLATFORM_ADAPTER_H
#define VFR_PLATFORM_ADAPTER_H

/* vfr.h 已 include vfr_defs.h，且定義了 vfr_frame_t（named struct vfr_frame） */
#include "vfr.h"

/*
 * vfr_platform_ops_t：平台硬體抽象介面
 *
 * 所有函式指標均為非阻塞語意，上層負責 retry / epoll 等待。
 */
typedef struct {
    const char *name;   /* "amba_iav5", "amba_iav6", "v4l2", "mock" */

    /*
     * init()：初始化平台資源
     *   cfg   — SHM Header（格式協商完成後傳入）；Phase 1 可傳 NULL
     *   成功回傳 0；失敗回傳 -1
     */
    int  (*init)    (void **ctx, const vfr_shm_header_t *cfg);

    /*
     * get_frame()：非阻塞取一幀
     *   0  = 有幀，out 已填入完整 metadata
     *   1  = 暫無新幀（EAGAIN）
     *  -1  = 不可恢復錯誤（errno 已設定）
     */
    int  (*get_frame)(void *ctx, vfr_frame_t *out);

    /*
     * put_frame()：歸還幀給 platform（DSP 可再使用該 buffer）
     *   Phase 1 mock：close dma_fd 即可
     */
    void (*put_frame)(void *ctx, vfr_frame_t *frame);

    /*
     * destroy()：釋放所有平台資源
     *   必須 idempotent：*ctx 為 NULL 時為 no-op
     */
    void (*destroy) (void **ctx);

    /*
     * has_native_dma_buf：平台是否原生支援 DMA-BUF export
     *   false → 框架改走 memfd 降級路徑（Phase 4）
     */
    bool has_native_dma_buf;
} vfr_platform_ops_t;

/*
 * 各平台 adapter 的 ops 取得函式（各 .c 實作）
 */
const vfr_platform_ops_t *vfr_get_amba_ops(void);   /* amba_adapter.c */
const vfr_platform_ops_t *vfr_get_mock_ops(void);   /* mock_adapter.c / amba_adapter.c mock mode */

#endif /* VFR_PLATFORM_ADAPTER_H */
