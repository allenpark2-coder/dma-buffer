/* include/vfr.h — 消費者唯一需要引用的標頭
 * Consumer 只需 #include "vfr.h"，不需要直接引用其他內部標頭。
 */
#ifndef VFR_H
#define VFR_H

#include "vfr_defs.h"

/* ─── Frame 資料結構 ─────────────────────────────────────────────────────── */
typedef struct vfr_frame {
    int      dma_fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;                        /* fourcc，e.g. V4L2_PIX_FMT_NV12 */
    uint32_t stride;                        /* luma plane pitch，bytes；每幀由 platform adapter 填入 */
    uint32_t buf_size;                      /* 整個 buffer 總大小（bytes）；mmap/munmap 的 length 來源 */
    uint32_t flags;                         /* VFR_FLAG_* bitmask */
    uint32_t plane_offset[VFR_MAX_PLANES];  /* Y / U / V 偏移；每幀由 platform adapter 填入 */
    uint64_t timestamp_ns;
    uint64_t seq_num;
    void    *priv;                          /* platform 私有，消費者不碰 */
} vfr_frame_t;

/* ─── Context（opaque）─────────────────────────────────────────────────────  */
typedef struct vfr_ctx vfr_ctx_t;

/* ─── 公開 API ───────────────────────────────────────────────────────────── */

/*
 * vfr_open()：
 *   stream_name  — 長度必須 < VFR_SOCKET_NAME_MAX，否則回傳 NULL
 *   slot_count   — Buffer Pool 大小；傳 0 使用 VFR_DEFAULT_SLOTS；
 *                  上限：VFR_MAX_SLOTS（超過回傳 NULL）
 *                  不提供動態調整；需要更多 slot 請重新 open
 *
 * 回傳：成功回傳 context 指標；失敗回傳 NULL
 */
vfr_ctx_t *vfr_open(const char *stream_name, uint32_t slot_count);

/*
 * vfr_close()：
 *   釋放所有資源，將 *ctx 設為 NULL；重複呼叫為 no-op。
 *   呼叫前需確保：
 *   1. 已從自己的 epoll 登出 vfr_get_eventfd() 回傳的 fd
 *   2. 未持有 frame（或已先 vfr_put_frame()）
 */
void vfr_close(vfr_ctx_t **ctx);

/*
 * vfr_get_frame()：
 *   flags  — VFR_FLAG_NONBLOCK：無新幀時立即回傳 -1（errno = EAGAIN）
 *            0：阻塞等待（Phase 1 單 process 版透過 platform adapter 輪詢）
 *
 * 回傳：0 = 成功；-1 = 失敗（errno 指示原因）
 */
int vfr_get_frame(vfr_ctx_t *ctx, vfr_frame_t *frame, int flags);

/*
 * vfr_put_frame()：
 *   ownership 語意：
 *   - 呼叫後 frame->dma_fd 由框架負責 close，consumer 不得再使用此 fd
 *   - frame 結構本身由 consumer 管理（stack 變數即可），框架不會 free 它
 *   - 呼叫後不得再對此 frame 呼叫 vfr_map()
 *   - frame 為 NULL 時為 no-op
 */
void vfr_put_frame(vfr_frame_t *frame);

/*
 * vfr_map()：
 *   mmap dma_fd，自動執行 DMA_BUF_IOCTL_SYNC（SYNC_START | READ）。
 *   除非設定 VFR_FLAG_NO_CPU_SYNC，否則強制執行 cache sync。
 *
 * 回傳：成功回傳映射地址；失敗回傳 NULL
 */
void *vfr_map(const vfr_frame_t *frame);

/*
 * vfr_unmap()：
 *   執行 DMA_BUF_IOCTL_SYNC（SYNC_END | READ）後 munmap。
 */
void vfr_unmap(const vfr_frame_t *frame, void *ptr);

/*
 * vfr_get_eventfd()：
 *   ownership 語意：
 *   - 回傳的 fd 由框架持有，consumer 只可加入自己的 epoll，不得 close() 它
 *   - 呼叫 vfr_close() 前，consumer 必須先從自己的 epoll 登出此 fd
 *
 * Phase 1（單 process）：回傳 -1（尚未實作 eventfd 通知）
 */
int vfr_get_eventfd(vfr_ctx_t *ctx);

#endif /* VFR_H */
