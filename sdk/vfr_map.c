/* sdk/vfr_map.c — Zero-copy Mapper
 *
 * 實作 vfr_map() / vfr_unmap()。
 *
 * Cache Coherency 正確順序（見 plan v2 勘誤）：
 *   vfr_map()   : mmap() 先，再 DMA_BUF_IOCTL_SYNC(SYNC_START | READ)
 *   vfr_unmap() : DMA_BUF_IOCTL_SYNC(SYNC_END | READ) 先，再 munmap()
 *
 * 若 frame->flags 含 VFR_FLAG_NO_CPU_SYNC（mock adapter 或 VFR_FLAG_NO_CPU_SYNC），
 * 則跳過 IOCTL_SYNC（memfd 不是真正的 dma_buf，ioctl 會 ENOTTY）。
 */

#include "vfr.h"
#include "vfr_defs.h"

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

/* linux/dma-buf.h 在某些主機環境可能不存在，fallback 定義 */
#ifdef HAVE_DMA_BUF_H
#  include <linux/dma-buf.h>
#else
/* Minimal fallback：從 Linux uapi 複製，確保在老 sysroot 也可編譯 */
#  include <linux/types.h>
#  ifndef DMA_BUF_SYNC_READ
#    define DMA_BUF_SYNC_READ      (1 << 0)
#    define DMA_BUF_SYNC_WRITE     (2 << 0)
#    define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#    define DMA_BUF_SYNC_START     (0 << 2)
#    define DMA_BUF_SYNC_END       (1 << 2)
struct dma_buf_sync { __u64 flags; };
#    define DMA_BUF_BASE           'b'
#    define DMA_BUF_IOCTL_SYNC     _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#  endif
#endif

/* ─── vfr_map ───────────────────────────────────────────────────────────── */
void *vfr_map(const vfr_frame_t *frame)
{
    if (!frame || frame->dma_fd < 0) {
        VFR_LOGE("vfr_map: invalid frame (frame=%p dma_fd=%d)",
                 (void *)frame, frame ? frame->dma_fd : -1);
        return NULL;
    }
    if (frame->buf_size == 0) {
        VFR_LOGE("vfr_map: buf_size == 0");
        return NULL;
    }

    /* 1. mmap 先（必須在 SYNC_START 之前） */
    void *ptr = mmap(NULL, frame->buf_size, PROT_READ, MAP_SHARED, frame->dma_fd, 0);
    if (ptr == MAP_FAILED) {
        VFR_LOGE("mmap(fd=%d size=%u) failed: %s",
                 frame->dma_fd, frame->buf_size, strerror(errno));
        return NULL;
    }

    /* 2. DMA_BUF_IOCTL_SYNC(SYNC_START | READ)（mmap 之後，讀取之前） */
    if (!(frame->flags & VFR_FLAG_NO_CPU_SYNC)) {
        struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        if (ioctl(frame->dma_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
            /* ENOTTY：fd 不是真正的 dma_buf（例如 memfd），視為非致命 */
            if (errno != ENOTTY) {
                VFR_LOGW("DMA_BUF_IOCTL_SYNC SYNC_START failed: %s (fd=%d)",
                         strerror(errno), frame->dma_fd);
            }
        }
    }

    VFR_LOGD("vfr_map: fd=%d ptr=%p size=%u", frame->dma_fd, ptr, frame->buf_size);
    return ptr;
}

/* ─── vfr_unmap ─────────────────────────────────────────────────────────── */
void vfr_unmap(const vfr_frame_t *frame, void *ptr)
{
    if (!frame || !ptr) return;
    if (ptr == MAP_FAILED) return;

    /* 1. DMA_BUF_IOCTL_SYNC(SYNC_END | READ)（munmap 之前） */
    if (!(frame->flags & VFR_FLAG_NO_CPU_SYNC)) {
        struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        if (ioctl(frame->dma_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
            if (errno != ENOTTY) {
                VFR_LOGW("DMA_BUF_IOCTL_SYNC SYNC_END failed: %s (fd=%d)",
                         strerror(errno), frame->dma_fd);
            }
        }
    }

    /* 2. munmap（SYNC_END 之後） */
    if (munmap(ptr, frame->buf_size) < 0) {
        VFR_LOGW("munmap(ptr=%p size=%u) failed: %s", ptr, frame->buf_size, strerror(errno));
    }

    VFR_LOGD("vfr_unmap: fd=%d ptr=%p size=%u", frame->dma_fd, ptr, frame->buf_size);
}
